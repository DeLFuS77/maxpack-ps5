// SPDX-License-Identifier: GPL-3.0-or-later
// Kraken ("newLZ") block decoder. Ported from pearlxcore/ProsperoPkgTool (MIT)
// Compression/KrakenDecoder.cs, whose structure follows the public "ooz" decoder, with
// fthdgn-gh/Alembic's src/core/kraken.cpp (GPL-3.0-or-later, a port of the same) as the second
// reading. As in those:
//  * C# masks shift counts to 5 bits and the decoder relies on it (x >> 32 == x), so every variable
//    shift goes through shr/shl, which mask the same way;
//  * a read outside the input gives 0, as the C# helpers do, and every index taken from the input
//    is checked, so a damaged block fails instead of reading or writing out of bounds.
#include "mp_kraken.h"

#include <stdlib.h>
#include <string.h>

typedef int64_t ix;   // positions and counts (the C# code's int and long)

#define SEED 8
#define SYMS 1280
#define MAX_U32_LENGTHS 512

struct mp_kraken {
    uint8_t literals[MP_KRAKEN_CHUNK];
    uint8_t commands[MP_KRAKEN_CHUNK];
    uint8_t offs[MP_KRAKEN_CHUNK];
    uint8_t offs_extra[MP_KRAKEN_CHUNK];
    uint8_t lit_len[MP_KRAKEN_CHUNK / 4];
    int32_t distances[MP_KRAKEN_CHUNK];
    int32_t lengths[MP_KRAKEN_CHUNK / 4];
    uint32_t u32s[MAX_U32_LENGTHS];
};

mp_kraken *mp_kraken_new(void) { return malloc(sizeof(mp_kraken)); }
void mp_kraken_free(mp_kraken *k) { free(k); }

static uint32_t shr(uint32_t x, int n) { return x >> (n & 31); }
static uint32_t shl(uint32_t x, int n) { return x << (n & 31); }
static int ishl(int x, int n) { return (int)((uint32_t)x << (n & 31)); }
static uint32_t rotl(uint32_t x, int n) {
    n &= 31;
    return n == 0 ? x : (x << n) | (x >> (32 - n));
}

static int msb(uint32_t x) {
    if (x == 0) return 0;
    int r = 31;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        --r;
    }
    return r;
}
static int lsb(uint32_t x) {
    if (x == 0) return 0;
    int r = 0;
    while (!(x & 1)) {
        x >>= 1;
        ++r;
    }
    return r;
}
static int clz(uint32_t x) { return x == 0 ? 31 : 31 - msb(x); }   // .NET's LeadingZeroCount, but 31 for 0

// A buffer of known length: reads outside give 0, writes outside do nothing.
typedef struct {
    const uint8_t *p;
    ix n;
} cbuf;
typedef struct {
    uint8_t *p;
    ix n;
} mbuf;

static uint8_t at(cbuf b, ix i) { return i >= 0 && i < b.n ? b.p[i] : 0; }
static uint32_t rd_le32(cbuf b, ix off) {
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) r |= (uint32_t)at(b, off + i) << (8 * i);
    return r;
}
static uint64_t rd_le64(cbuf b, ix off) {
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r |= (uint64_t)at(b, off + i) << (8 * i);
    return r;
}
static void wr_le32(mbuf b, ix off, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        if (off + i >= 0 && off + i < b.n) b.p[off + i] = (uint8_t)(v >> (8 * i));
}
static void wr_le64(mbuf b, ix off, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        if (off + i >= 0 && off + i < b.n) b.p[off + i] = (uint8_t)(v >> (8 * i));
}
static cbuf cb(mbuf b) {
    cbuf c = {b.p, b.n};
    return c;
}
static uint32_t swap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
static uint64_t swap64(uint64_t v) { return ((uint64_t)swap32((uint32_t)v) << 32) | swap32((uint32_t)(v >> 32)); }

// ---- bit reader -----------------------------------------------------------------------------

// MSB-first, with a 24-bit refill window, forward or backward.
typedef struct {
    cbuf d;
    ix p;
    ix bound;   // forward: the exclusive high end; backward: the inclusive low end
    uint32_t bits;
    int bitpos;
    int backward;
} br;

static void refill_fwd(br *r) {
    while (r->bitpos > 0) {
        r->bits |= shl(r->p < r->bound ? at(r->d, r->p) : 0, r->bitpos);
        r->bitpos -= 8;
        ++r->p;
    }
}
static void refill_bwd(br *r) {
    while (r->bitpos > 0) {
        --r->p;
        r->bits |= shl(r->p >= r->bound ? at(r->d, r->p) : 0, r->bitpos);
        r->bitpos -= 8;
    }
}
static void refill(br *r) {
    if (r->backward)
        refill_bwd(r);
    else
        refill_fwd(r);
}
static br br_fwd(cbuf d, ix start, ix end) {
    br r = {d, start, end, 0, 24, 0};
    refill_fwd(&r);
    return r;
}
static br br_bwd(cbuf d, ix low, ix high) {
    br r = {d, high, low, 0, 24, 1};
    refill_bwd(&r);
    return r;
}
static int read_bit(br *r) {
    refill(r);
    int v = (int)(r->bits >> 31);
    r->bits <<= 1;
    r->bitpos += 1;
    return v;
}
static int read_bit_nr(br *r) {
    int v = (int)(r->bits >> 31);
    r->bits <<= 1;
    r->bitpos += 1;
    return v;
}
static uint32_t read_bits_nr(br *r, int n) {
    uint32_t v = shr(r->bits, 32 - n);
    r->bits = shl(r->bits, n);
    r->bitpos += n;
    return v;
}
static uint32_t read_bits_nr_zero(br *r, int n) {
    uint32_t v = shr(r->bits >> 1, 31 - n);
    r->bits = shl(r->bits, n);
    r->bitpos += n;
    return v;
}
static int read_fluff(br *r, int num_symbols) {
    if (num_symbols == 256) return 0;
    int x = 257 - num_symbols;
    if (x > num_symbols) x = num_symbols;
    x *= 2;
    int y = msb((uint32_t)(x - 1)) + 1;
    uint32_t v = shr(r->bits, 32 - y);
    uint32_t z = (uint32_t)(ishl(1, y) - x);
    if ((v >> 1) >= z) {
        r->bits = shl(r->bits, y);
        r->bitpos += y;
        return (int)(v - z);
    }
    r->bits = shl(r->bits, y - 1);
    r->bitpos += y - 1;
    return (int)(v >> 1);
}
static uint32_t read_extra(br *r, int n) {
    uint32_t v;
    if (n <= 24) {
        v = read_bits_nr_zero(r, n);
    } else {
        v = shl(read_bits_nr(r, 24), n - 24);
        refill(r);
        v += read_bits_nr(r, n - 24);
    }
    refill(r);
    return v;
}
static uint32_t read_distance(br *r, uint32_t v) {
    uint32_t w, m, rv;
    int n;
    if (v < 0xF0) {
        n = (int)(v >> 4) + 4;
        w = rotl(r->bits | 1, n);
        r->bitpos += n;
        m = shl(2u, n) - 1;
        r->bits = w & ~m;
        rv = ((w & m) << 4) + (v & 0xF) - 248;
    } else {
        n = (int)(v - 0xF0) + 4;
        w = rotl(r->bits | 1, n);
        r->bitpos += n;
        m = shl(2u, n) - 1;
        r->bits = w & ~m;
        rv = 8322816 + ((w & m) << 12);
        refill(r);
        rv += r->bits >> 20;
        r->bitpos += 12;
        r->bits <<= 12;
    }
    refill(r);
    return rv;
}
static int read_length(br *r, uint32_t *v) {
    int n = 31 - msb(r->bits);
    if (n > 12) return 0;
    r->bitpos += n;
    r->bits = shl(r->bits, n);
    refill(r);
    n += 7;
    r->bitpos += n;
    *v = shr(r->bits, 32 - n) - 64;
    r->bits = shl(r->bits, n);
    refill(r);
    return 1;
}

// ---- Huffman --------------------------------------------------------------------------------

static const uint32_t CODE_PREFIX[12] = {0x0, 0x0, 0x2, 0x6, 0xE, 0x1E, 0x3E, 0x7E, 0xFE, 0x1FE, 0x2FE, 0x3FE};

typedef struct {
    uint8_t len[2048 + 16];
    uint8_t sym[2048 + 16];
} lut_t;

typedef struct {
    int symbol, num;
} hrange;

typedef struct {
    cbuf d;
    ix p, end;
    int bitpos;
} golomb;

static const uint32_t RICE_VALUE[256] = {
    0x80000000, 0x00000007, 0x10000006, 0x00000006, 0x20000005, 0x00000105, 0x10000005, 0x00000005,
    0x30000004, 0x00000204, 0x10000104, 0x00000104, 0x20000004, 0x00010004, 0x10000004, 0x00000004,
    0x40000003, 0x00000303, 0x10000203, 0x00000203, 0x20000103, 0x00010103, 0x10000103, 0x00000103,
    0x30000003, 0x00020003, 0x10010003, 0x00010003, 0x20000003, 0x01000003, 0x10000003, 0x00000003,
    0x50000002, 0x00000402, 0x10000302, 0x00000302, 0x20000202, 0x00010202, 0x10000202, 0x00000202,
    0x30000102, 0x00020102, 0x10010102, 0x00010102, 0x20000102, 0x01000102, 0x10000102, 0x00000102,
    0x40000002, 0x00030002, 0x10020002, 0x00020002, 0x20010002, 0x01010002, 0x10010002, 0x00010002,
    0x30000002, 0x02000002, 0x11000002, 0x01000002, 0x20000002, 0x00000012, 0x10000002, 0x00000002,
    0x60000001, 0x00000501, 0x10000401, 0x00000401, 0x20000301, 0x00010301, 0x10000301, 0x00000301,
    0x30000201, 0x00020201, 0x10010201, 0x00010201, 0x20000201, 0x01000201, 0x10000201, 0x00000201,
    0x40000101, 0x00030101, 0x10020101, 0x00020101, 0x20010101, 0x01010101, 0x10010101, 0x00010101,
    0x30000101, 0x02000101, 0x11000101, 0x01000101, 0x20000101, 0x00000111, 0x10000101, 0x00000101,
    0x50000001, 0x00040001, 0x10030001, 0x00030001, 0x20020001, 0x01020001, 0x10020001, 0x00020001,
    0x30010001, 0x02010001, 0x11010001, 0x01010001, 0x20010001, 0x00010011, 0x10010001, 0x00010001,
    0x40000001, 0x03000001, 0x12000001, 0x02000001, 0x21000001, 0x01000011, 0x11000001, 0x01000001,
    0x30000001, 0x00000021, 0x10000011, 0x00000011, 0x20000001, 0x00001001, 0x10000001, 0x00000001,
    0x70000000, 0x00000600, 0x10000500, 0x00000500, 0x20000400, 0x00010400, 0x10000400, 0x00000400,
    0x30000300, 0x00020300, 0x10010300, 0x00010300, 0x20000300, 0x01000300, 0x10000300, 0x00000300,
    0x40000200, 0x00030200, 0x10020200, 0x00020200, 0x20010200, 0x01010200, 0x10010200, 0x00010200,
    0x30000200, 0x02000200, 0x11000200, 0x01000200, 0x20000200, 0x00000210, 0x10000200, 0x00000200,
    0x50000100, 0x00040100, 0x10030100, 0x00030100, 0x20020100, 0x01020100, 0x10020100, 0x00020100,
    0x30010100, 0x02010100, 0x11010100, 0x01010100, 0x20010100, 0x00010110, 0x10010100, 0x00010100,
    0x40000100, 0x03000100, 0x12000100, 0x02000100, 0x21000100, 0x01000110, 0x11000100, 0x01000100,
    0x30000100, 0x00000120, 0x10000110, 0x00000110, 0x20000100, 0x00001100, 0x10000100, 0x00000100,
    0x60000000, 0x00050000, 0x10040000, 0x00040000, 0x20030000, 0x01030000, 0x10030000, 0x00030000,
    0x30020000, 0x02020000, 0x11020000, 0x01020000, 0x20020000, 0x00020010, 0x10020000, 0x00020000,
    0x40010000, 0x03010000, 0x12010000, 0x02010000, 0x21010000, 0x01010010, 0x11010000, 0x01010000,
    0x30010000, 0x00010020, 0x10010010, 0x00010010, 0x20010000, 0x00011000, 0x10010000, 0x00010000,
    0x50000000, 0x04000000, 0x13000000, 0x03000000, 0x22000000, 0x02000010, 0x12000000, 0x02000000,
    0x31000000, 0x01000020, 0x11000010, 0x01000010, 0x21000000, 0x01001000, 0x11000000, 0x01000000,
    0x40000000, 0x00000030, 0x10000020, 0x00000020, 0x20000010, 0x00001010, 0x10000010, 0x00000010,
    0x30000000, 0x00002000, 0x10001000, 0x00001000, 0x20000000, 0x00100000, 0x10000000, 0x00000000,
};

static const uint8_t RICE_LENGTH[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8,
};

static int put_sym(uint8_t *syms, uint32_t *prefix, int codelen, uint8_t sym) {
    if (codelen < 0 || codelen > 11 || prefix[codelen] >= SYMS) return 0;
    syms[prefix[codelen]++] = sym;
    return 1;
}

static int code_lengths_old(br *bits, uint8_t *syms, uint32_t *prefix) {
    if (read_bit_nr(bits) != 0) {
        int sym = 0, codelen, num_symbols = 0;
        int avg_bits_x4 = 32;
        int forced_bits = (int)read_bits_nr(bits, 2);
        uint32_t thres = shl(1u, 31 - (int)(20u >> forced_bits));
        int n;
        int skip = read_bit(bits) != 0;
        for (;;) {
            if (!skip) {
                if ((bits->bits & 0xFF000000) == 0) return MP_KRAKEN_MALFORMED;
                sym += (int)read_bits_nr(bits, 2 * (clz(bits->bits) + 1)) - 2 + 1;
                if (sym >= 256) break;
            }
            skip = 0;
            refill(bits);
            if ((bits->bits & 0xFF000000) == 0) return MP_KRAKEN_MALFORMED;
            n = (int)read_bits_nr(bits, 2 * (clz(bits->bits) + 1)) - 2 + 1;
            if (sym + n > 256) return MP_KRAKEN_MALFORMED;
            refill(bits);
            num_symbols += n;
            do {
                if (bits->bits < thres) return MP_KRAKEN_MALFORMED;
                int lz = clz(bits->bits);
                int v = (int)read_bits_nr(bits, lz + forced_bits + 1) + ishl(lz - 1, forced_bits);
                codelen = (-(v & 1) ^ (v >> 1)) + ((avg_bits_x4 + 2) >> 2);
                if (codelen < 1 || codelen > 11) return MP_KRAKEN_MALFORMED;
                avg_bits_x4 = codelen + ((3 * avg_bits_x4 + 2) >> 2);
                refill(bits);
                if (!put_sym(syms, prefix, codelen, (uint8_t)sym++)) return MP_KRAKEN_MALFORMED;
            } while (--n != 0);
            if (sym == 256) break;
        }
        return (sym == 256 && num_symbols >= 2) ? num_symbols : MP_KRAKEN_MALFORMED;
    }
    int num_symbols = (int)read_bits_nr(bits, 8);
    if (num_symbols == 0) return MP_KRAKEN_MALFORMED;
    if (num_symbols == 1) {
        syms[0] = (uint8_t)read_bits_nr(bits, 8);
    } else {
        int codelen_bits = (int)read_bits_nr(bits, 3);
        if (codelen_bits > 4) return MP_KRAKEN_MALFORMED;
        for (int i = 0; i < num_symbols; ++i) {
            refill(bits);
            int sym = (int)read_bits_nr(bits, 8);
            int codelen = (int)read_bits_nr_zero(bits, codelen_bits) + 1;
            if (codelen > 11) return MP_KRAKEN_MALFORMED;
            if (!put_sym(syms, prefix, codelen, (uint8_t)sym)) return MP_KRAKEN_MALFORMED;
        }
    }
    return num_symbols;
}

static int golomb_lengths(mbuf dst, int size, golomb *g) {
    ix p = g->p, p_end = g->end;
    ix dst_pos = 0, dst_end = size;
    if (p >= p_end) return 0;
    int count = -g->bitpos;
    uint32_t v = (uint32_t)(at(g->d, p++) & (255 >> g->bitpos));
    for (;;) {
        if (v == 0) {
            count += 8;
        } else {
            uint32_t x = RICE_VALUE[v];
            uint32_t lo = (uint32_t)count + (x & 0x0F0F0F0F);
            wr_le32(dst, dst_pos, lo);
            wr_le32(dst, dst_pos + 4, (x >> 4) & 0x0F0F0F0F);
            dst_pos += RICE_LENGTH[v];
            if (dst_pos >= dst_end) break;
            count = (int)(x >> 28);
        }
        if (p >= p_end) return 0;
        v = at(g->d, p++);
    }
    if (dst_pos > dst_end) {
        ix nn = dst_pos - dst_end;
        do {
            v &= v - 1;
        } while (--nn != 0);
    }
    int bitpos = 0;
    if ((v & 1) == 0) {
        --p;
        bitpos = 8 - lsb(v);
    }
    g->p = p;
    g->bitpos = bitpos;
    return 1;
}

static int golomb_bits(mbuf dst, int size, int bitcount, golomb *g) {
    if (bitcount == 0) return 1;
    ix dst_pos = 0, dst_end = size;
    ix p = g->p;
    int bitpos = g->bitpos;
    ix bits_required = bitpos + (ix)bitcount * size;
    ix bytes_required = (bits_required + 7) >> 3;
    if (bytes_required > g->end - p) return 0;
    g->p = p + (bits_required >> 3);
    g->bitpos = (int)(bits_required & 7);
    uint64_t bak = rd_le64(cb(dst), dst_end);
    if (bitcount == 1) {
        do {
            uint64_t b = (uint8_t)shr(swap32(rd_le32(g->d, p)), 24 - bitpos);
            p += 1;
            b = (b | (b << 28)) & 0xF0000000Full;
            b = (b | (b << 14)) & 0x3000300030003ull;
            b = (b | (b << 7)) & 0x0101010101010101ull;
            wr_le64(dst, dst_pos, rd_le64(cb(dst), dst_pos) * 2 + swap64(b));
            dst_pos += 8;
        } while (dst_pos < dst_end);
    } else if (bitcount == 2) {
        do {
            uint64_t b = (uint16_t)shr(swap32(rd_le32(g->d, p)), 16 - bitpos);
            p += 2;
            b = (b | (b << 24)) & 0xFF000000FFull;
            b = (b | (b << 12)) & 0xF000F000F000Full;
            b = (b | (b << 6)) & 0x0303030303030303ull;
            wr_le64(dst, dst_pos, rd_le64(cb(dst), dst_pos) * 4 + swap64(b));
            dst_pos += 8;
        } while (dst_pos < dst_end);
    } else {
        do {
            uint64_t b = (uint64_t)(shr(swap32(rd_le32(g->d, p)), 8 - bitpos) & 0xFFFFFF);
            p += 3;
            b = (b | (b << 20)) & 0xFFF00000FFFull;
            b = (b | (b << 10)) & 0x3F003F003F003Full;
            b = (b | (b << 5)) & 0x0707070707070707ull;
            wr_le64(dst, dst_pos, rd_le64(cb(dst), dst_pos) * 8 + swap64(b));
            dst_pos += 8;
        } while (dst_pos < dst_end);
    }
    wr_le64(dst, dst_end, bak);
    return 1;
}

static int to_ranges(hrange *range, int num_symbols, int p, cbuf symlen, int symlen_off, br *bits) {
    int num_ranges = p >> 1, v, sym_idx = 0;
    if (num_ranges + 1 > 128) return MP_KRAKEN_MALFORMED;
#define LEN_AT(i) ((i) >= 0 && (i) < symlen.n ? (int)symlen.p[(i)] : 255)
    if ((p & 1) != 0) {
        refill(bits);
        v = LEN_AT(symlen_off);
        symlen_off++;
        if (v >= 8) return MP_KRAKEN_MALFORMED;
        sym_idx = (int)read_bits_nr(bits, v + 1) + ishl(1, v + 1) - 1;
    }
    int syms_used = 0;
    for (int i = 0; i < num_ranges; ++i) {
        refill(bits);
        v = LEN_AT(symlen_off);
        if (v >= 9) return MP_KRAKEN_MALFORMED;
        int num = (int)read_bits_nr_zero(bits, v) + ishl(1, v);
        v = LEN_AT(symlen_off + 1);
        if (v >= 8) return MP_KRAKEN_MALFORMED;
        int space = (int)read_bits_nr(bits, v + 1) + ishl(1, v + 1) - 1;
        range[i].symbol = sym_idx;
        range[i].num = num;
        syms_used += num;
        sym_idx += num + space;
        symlen_off += 2;
    }
#undef LEN_AT
    if (sym_idx >= 256 || syms_used >= num_symbols || sym_idx + num_symbols - syms_used > 256)
        return MP_KRAKEN_MALFORMED;
    range[num_ranges].symbol = sym_idx;
    range[num_ranges].num = num_symbols - syms_used;
    return num_ranges + 1;
}

static int code_lengths_new(br *bits, uint8_t *syms, uint32_t *prefix) {
    int forced_bits = (int)read_bits_nr(bits, 2);
    int num_symbols = (int)read_bits_nr(bits, 8) + 1;
    int fluff = read_fluff(bits, num_symbols);

    uint8_t code_len[512 + 16];
    memset(code_len, 0, sizeof code_len);
    mbuf cl = {code_len, sizeof code_len};
    golomb g = {bits->d, bits->p - ((24 - bits->bitpos + 7) >> 3), bits->bound, (bits->bitpos - 24) & 7};
    if (num_symbols + fluff + 16 > (int)sizeof code_len) return MP_KRAKEN_MALFORMED;
    if (!golomb_lengths(cl, num_symbols + fluff, &g)) return MP_KRAKEN_MALFORMED;
    for (int z = 0; z < 16; ++z) code_len[num_symbols + fluff + z] = 0;
    if (!golomb_bits(cl, num_symbols, forced_bits, &g)) return MP_KRAKEN_MALFORMED;

    bits->bitpos = 24;
    bits->p = g.p;
    bits->bits = 0;
    refill(bits);
    bits->bits = shl(bits->bits, g.bitpos);
    bits->bitpos += g.bitpos;

    uint32_t running_sum = 0x1e;
    for (int i = 0; i < num_symbols; ++i) {
        int v = code_len[i];
        v = -(v & 1) ^ (v >> 1);
        int c = v + (int)(running_sum >> 2) + 1;
        if (c < 1 || c > 11) return MP_KRAKEN_MALFORMED;
        code_len[i] = (uint8_t)c;
        running_sum = (uint32_t)((int)running_sum + v);
    }

    hrange range[128];
    int ranges = to_ranges(range, num_symbols, fluff, cb(cl), num_symbols, bits);
    if (ranges <= 0) return MP_KRAKEN_MALFORMED;
    int cp = 0;
    for (int i = 0; i < ranges; ++i) {
        int sym = range[i].symbol;
        int nn = range[i].num;
        if (nn <= 0) return MP_KRAKEN_MALFORMED;
        do {
            if (cp >= num_symbols) return MP_KRAKEN_MALFORMED;
            if (!put_sym(syms, prefix, code_len[cp++], (uint8_t)sym++)) return MP_KRAKEN_MALFORMED;
        } while (--nn != 0);
    }
    return num_symbols;
}

static int build_lut(const uint32_t *prefix, lut_t *lut, const uint8_t *syms) {
    uint32_t slot = 0;
    for (uint32_t i = 1; i < 11; ++i) {
        uint32_t start = CODE_PREFIX[i];
        uint32_t count = prefix[i] - start;
        if (count == 0) continue;
        uint32_t step = 1u << (11 - i);
        uint32_t num = count << (11 - i);
        if (slot + num > 2048 || start + count > SYMS) return 0;
        memset(lut->len + slot, (int)i, num);
        uint32_t p = slot;
        for (uint32_t j = 0; j != count; ++j, p += step) memset(lut->sym + p, syms[start + j], step);
        slot += num;
    }
    if (prefix[11] - CODE_PREFIX[11] != 0) {
        uint32_t num = prefix[11] - CODE_PREFIX[11];
        if (slot + num > 2048 || CODE_PREFIX[11] + num > SYMS) return 0;
        memset(lut->len + slot, 11, num);
        memcpy(lut->sym + slot, syms + CODE_PREFIX[11], num);
        slot += num;
    }
    return slot == 2048;
}

static int reverse11(int v) {
    int r = 0;
    for (int k = 0; k < 11; ++k) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

typedef struct {
    ix src, src_end, src_mid, src_mid_original;
    ix out_off, out_end;
} hstream;

// The three-stream scalar Huffman core: one stream forward from src, one backward from src_end, one
// forward from src_mid.
static int huffman_core(cbuf data, mbuf out, hstream hr, const lut_t *lut) {
    ix src = hr.src, src_mid = hr.src_mid, src_end = hr.src_end;
    uint32_t src_bits = 0, src_mid_bits = 0, src_end_bits = 0;
    int src_bitpos = 0, src_mid_bitpos = 0, src_end_bitpos = 0;
    ix dst = hr.out_off, dst_end = hr.out_end;
    const uint8_t *d = data.p;
    if (src > src_mid || src_mid > src_end || dst_end > out.n || src_end > data.n || src < 0) return 0;
    for (;;) {
        if (dst >= dst_end) break;
        if (src_mid - src <= 1) {
            if (src_mid - src == 1) src_bits |= shl(d[src], src_bitpos);
        } else {
            src_bits |= shl((uint32_t)d[src] | ((uint32_t)d[src + 1] << 8), src_bitpos);
        }
        uint32_t k = src_bits & 0x7FF;
        int n = lut->len[k];
        src_bitpos -= n;
        src_bits = shr(src_bits, n);
        out.p[dst++] = lut->sym[k];
        src += (7 - src_bitpos) >> 3;
        src_bitpos &= 7;
        if (dst < dst_end) {
            if (src_end - src_mid <= 1) {
                if (src_end - src_mid == 1) {
                    src_end_bits |= shl(d[src_mid], src_end_bitpos);
                    src_mid_bits |= shl(d[src_mid], src_mid_bitpos);
                }
            } else {
                uint32_t vv = (uint32_t)d[src_end - 2] | ((uint32_t)d[src_end - 1] << 8);
                src_end_bits |= shl(((vv >> 8) | (vv << 8)) & 0xFFFF, src_end_bitpos);
                src_mid_bits |= shl((uint32_t)d[src_mid] | ((uint32_t)d[src_mid + 1] << 8), src_mid_bitpos);
            }
            k = src_end_bits & 0x7FF;
            n = lut->len[k];
            out.p[dst++] = lut->sym[k];
            src_end_bitpos -= n;
            src_end_bits = shr(src_end_bits, n);
            src_end -= (7 - src_end_bitpos) >> 3;
            src_end_bitpos &= 7;
            if (dst < dst_end) {
                k = src_mid_bits & 0x7FF;
                n = lut->len[k];
                out.p[dst++] = lut->sym[k];
                src_mid_bitpos -= n;
                src_mid_bits = shr(src_mid_bits, n);
                src_mid += (7 - src_mid_bitpos) >> 3;
                src_mid_bitpos &= 7;
            }
        }
        if (src > src_mid || src_mid > src_end) return 0;
    }
    return src == hr.src_mid_original && src_end == src_mid;
}

static int decode_huffman(cbuf src, ix src_start, int src_size, mbuf out, int output_size, int type) {
    ix src_end = src_start + src_size;
    br bits = br_fwd(src, src_start, src_end);
    uint32_t prefix[12];
    memcpy(prefix, CODE_PREFIX, sizeof prefix);
    uint8_t syms[SYMS];
    memset(syms, 0, sizeof syms);
    int num_syms;
    if (read_bit_nr(&bits) == 0)
        num_syms = code_lengths_old(&bits, syms, prefix);
    else if (read_bit_nr(&bits) == 0)
        num_syms = code_lengths_new(&bits, syms, prefix);
    else
        return MP_KRAKEN_MALFORMED;
    if (num_syms < 1) return MP_KRAKEN_MALFORMED;

    ix sp = bits.p - ((24 - bits.bitpos) / 8);
    if (num_syms == 1) {
        memset(out.p, syms[0], (size_t)output_size);
        return src_size;
    }

    static lut_t zero_lut;
    lut_t lut = zero_lut, rev;
    if (!build_lut(prefix, &lut, syms)) return MP_KRAKEN_MALFORMED;
    for (int i = 0; i < 2048; ++i) {
        int r = reverse11(i);
        rev.len[i] = lut.len[r];
        rev.sym[i] = lut.sym[r];
    }

    if (type == 1) {
        if (sp + 3 > src_end) return MP_KRAKEN_MALFORMED;
        int split_mid = at(src, sp) | (at(src, sp + 1) << 8);
        sp += 2;
        hstream hr = {sp, src_end, sp + split_mid, sp + split_mid, 0, output_size};
        if (!huffman_core(src, out, hr, &rev)) return MP_KRAKEN_MALFORMED;
    } else {
        if (sp + 6 > src_end) return MP_KRAKEN_MALFORMED;
        int half_out = (output_size + 1) >> 1;
        int split_mid = at(src, sp) | (at(src, sp + 1) << 8) | (at(src, sp + 2) << 16);
        sp += 3;
        if (split_mid > src_end - sp) return MP_KRAKEN_MALFORMED;
        ix src_mid = sp + split_mid;
        int split_left = at(src, sp) | (at(src, sp + 1) << 8);
        sp += 2;
        if (src_mid - sp < split_left + 2 || src_end - src_mid < 3) return MP_KRAKEN_MALFORMED;
        int split_right = at(src, src_mid) | (at(src, src_mid + 1) << 8);
        if (src_end - (src_mid + 2) < split_right + 2) return MP_KRAKEN_MALFORMED;
        hstream h1 = {sp, src_mid, sp + split_left, sp + split_left, 0, half_out};
        if (!huffman_core(src, out, h1, &rev)) return MP_KRAKEN_MALFORMED;
        hstream h2 = {src_mid + 2, src_end, src_mid + 2 + split_right, src_mid + 2 + split_right, half_out,
                      output_size};
        if (!huffman_core(src, out, h2, &rev)) return MP_KRAKEN_MALFORMED;
    }
    return src_size;
}

// One entropy-coded array into out (at most cap bytes): the bytes it took, or an error.
static ix decode_bytes(cbuf src, ix sp, ix src_end, uint8_t *out, int cap, int *decoded) {
    *decoded = 0;
    ix org = sp;
    if (src_end - sp < 2) return MP_KRAKEN_MALFORMED;
    int type = (at(src, sp) >> 4) & 0x7;
    int src_size, dst_size;
    if (type == 0) {
        if (at(src, sp) >= 0x80) {
            src_size = ((at(src, sp) << 8) | at(src, sp + 1)) & 0xFFF;
            sp += 2;
        } else {
            if (src_end - sp < 3) return MP_KRAKEN_MALFORMED;
            src_size = (at(src, sp) << 16) | (at(src, sp + 1) << 8) | at(src, sp + 2);
            if ((src_size & ~0x3FFFF) != 0) return MP_KRAKEN_MALFORMED;
            sp += 3;
        }
        if (src_size > cap || src_end - sp < src_size) return MP_KRAKEN_MALFORMED;
        if (src_size) memcpy(out, src.p + sp, (size_t)src_size);
        *decoded = src_size;
        return sp + src_size - org;
    }
    if (at(src, sp) >= 0x80) {
        if (src_end - sp < 3) return MP_KRAKEN_MALFORMED;
        uint32_t b = ((uint32_t)at(src, sp) << 16) | ((uint32_t)at(src, sp + 1) << 8) | at(src, sp + 2);
        src_size = (int)(b & 0x3FF);
        dst_size = (int)(src_size + ((b >> 10) & 0x3FF) + 1);
        sp += 3;
    } else {
        if (src_end - sp < 5) return MP_KRAKEN_MALFORMED;
        uint32_t b = ((uint32_t)at(src, sp + 1) << 24) | ((uint32_t)at(src, sp + 2) << 16) |
                     ((uint32_t)at(src, sp + 3) << 8) | at(src, sp + 4);
        src_size = (int)(b & 0x3FFFF);
        dst_size = (int)((((b >> 18) | ((uint32_t)at(src, sp) << 14)) & 0x3FFFF) + 1);
        if (src_size >= dst_size) return MP_KRAKEN_MALFORMED;
        sp += 5;
    }
    if (src_end - sp < src_size || dst_size > cap) return MP_KRAKEN_MALFORMED;
    memset(out, 0, (size_t)dst_size);
    mbuf o = {out, dst_size};
    int used;
    switch (type) {
    case 2:
    case 4: used = decode_huffman(src, sp, src_size, o, dst_size, type >> 1); break;
    default: return MP_KRAKEN_UNSUPPORTED;   // TANS (1), RLE (3), recursive (5): not written by the package tools
    }
    if (used != src_size) return MP_KRAKEN_MALFORMED;
    *decoded = dst_size;
    return sp + src_size - org;
}

// ---- LZ -------------------------------------------------------------------------------------

typedef struct {
    int literal_count, command_count, distance_count, length_count;
} lzt_t;

static int unpack_offsets(mp_kraken *k, cbuf src, ix bs_begin, ix bs_end, int excess_flag, int excess_count,
                          int offs_count, int offs_scaling, int extra_count, int len_count) {
    br fwd = br_fwd(src, bs_begin, bs_end);
    br bwd = br_bwd(src, bs_begin, bs_end);
    const uint8_t *packed_offs = k->offs, *packed_lit_len = k->lit_len;
    int32_t *offs_stream = k->distances, *len_stream = k->lengths;

    int u32_len = 0;
    if (!excess_flag) {
        if (bwd.bits < 0x2000) return 0;
        int nn = 31 - msb(bwd.bits);
        bwd.bitpos += nn;
        bwd.bits = shl(bwd.bits, nn);
        refill_bwd(&bwd);
        ++nn;
        u32_len = (int)(shr(bwd.bits, 32 - nn) - 1);
        bwd.bitpos += nn;
        bwd.bits = shl(bwd.bits, nn);
        refill_bwd(&bwd);
    } else {
        for (int q = 0; q < len_count; ++q)
            if (packed_lit_len[q] == 255) ++u32_len;
    }

    if (offs_scaling == 0) {
        int i = 0;
        while (i < offs_count) {
            offs_stream[i] = -(int32_t)read_distance(&fwd, packed_offs[i]);
            ++i;
            if (i >= offs_count) break;
            offs_stream[i] = -(int32_t)read_distance(&bwd, packed_offs[i]);
            ++i;
        }
    } else {
        int i = 0;
        while (i < offs_count) {
            uint32_t cmd = packed_offs[i];
            if ((cmd >> 3) > 26) return 0;
            uint32_t off = shl(8u + (cmd & 7), (int)(cmd >> 3)) | read_extra(&fwd, (int)(cmd >> 3));
            offs_stream[i] = 8 - (int32_t)off;
            ++i;
            if (i >= offs_count) break;
            cmd = packed_offs[i];
            if ((cmd >> 3) > 26) return 0;
            off = shl(8u + (cmd & 7), (int)(cmd >> 3)) | read_extra(&bwd, (int)(cmd >> 3));
            offs_stream[i] = 8 - (int32_t)off;
            ++i;
        }
        if (offs_scaling != 1) {
            if (extra_count < offs_count) return 0;
            for (int j = 0; j < offs_count; ++j)
                offs_stream[j] = (int32_t)(int8_t)k->offs_extra[j] - offs_stream[j] * offs_scaling;
        }
    }

    if (u32_len < 0 || u32_len > MAX_U32_LENGTHS) return 0;
    uint32_t *u32s = k->u32s;
    br ea, eb;
    br *ra = &fwd, *rb = &bwd;
    if (excess_flag) {
        ea = br_fwd(src, bs_end, bs_end + excess_count);
        eb = br_bwd(src, bs_end, bs_end + excess_count);
        ra = &ea;
        rb = &eb;
    }
    int i = 0;
    for (; i + 1 < u32_len; i += 2) {
        if (!read_length(ra, &u32s[i])) return 0;
        if (!read_length(rb, &u32s[i + 1])) return 0;
    }
    if (i < u32_len && !read_length(ra, &u32s[i])) return 0;

    ix a_seam = fwd.p - ((24 - fwd.bitpos) >> 3);
    ix b_seam = bwd.p + ((24 - bwd.bitpos) >> 3);
    if (a_seam != b_seam && !excess_flag) return 0;

    int u = 0;
    for (int j = 0; j < len_count; ++j) {
        uint32_t v = packed_lit_len[j];
        if (v == 255) {
            if (u >= u32_len) return 0;
            v = u32s[u++] + 255;
        }
        len_stream[j] = (int32_t)v + 3;
    }
    return u == u32_len;
}

// The table of a sub-chunk: the source position after it, or an error.
static ix read_lz_table(mp_kraken *k, cbuf src, ix sp, ix src_end, uint8_t *dst, ix dst_start, int dst_size,
                        ix offset, lzt_t *lzt) {
    if (offset == 0) {
        if (src_end - sp < SEED || dst_size < SEED) return MP_KRAKEN_MALFORMED;
        memcpy(dst + dst_start, src.p + sp, SEED);
        sp += SEED;
    }
    int excess_flag = 0, excess_count = 0;
    if (sp < src_end && (src.p[sp] & 0x80) != 0) {
        uint8_t flag = src.p[sp++];
        if ((flag & 0xC0) != 0x80) return MP_KRAKEN_MALFORMED;
        excess_flag = 1;
        excess_count = flag & 0x3F;
        if (excess_count > 0x1F) {
            if (sp >= src_end) return MP_KRAKEN_MALFORMED;
            excess_count += src.p[sp++] * 0x20;
        }
        src_end -= excess_count;
        if (src_end < sp) return MP_KRAKEN_MALFORMED;
    }

    ix n = decode_bytes(src, sp, src_end, k->literals, dst_size, &lzt->literal_count);
    if (n < 0) return n;
    sp += n;
    n = decode_bytes(src, sp, src_end, k->commands, dst_size, &lzt->command_count);
    if (n < 0) return n;
    sp += n;
    if (src_end - sp < 3) return MP_KRAKEN_MALFORMED;

    int offs_scaling = 0, offs_count = 0, extra_count = 0;
    if ((src.p[sp] & 0x80) != 0) {
        offs_scaling = src.p[sp] - 127;
        ++sp;
        n = decode_bytes(src, sp, src_end, k->offs, lzt->command_count, &offs_count);
        if (n < 0) return n;
        sp += n;
        if (offs_scaling != 1) {
            n = decode_bytes(src, sp, src_end, k->offs_extra, offs_count, &extra_count);
            if (n < 0) return n;
            if (extra_count != offs_count) return MP_KRAKEN_MALFORMED;
            sp += n;
        }
    } else {
        n = decode_bytes(src, sp, src_end, k->offs, lzt->command_count, &offs_count);
        if (n < 0) return n;
        sp += n;
    }
    lzt->distance_count = offs_count;

    int len_count = 0;
    n = decode_bytes(src, sp, src_end, k->lit_len, dst_size >> 2, &len_count);
    if (n < 0) return n;
    sp += n;
    lzt->length_count = len_count;

    return unpack_offsets(k, src, sp, src_end, excess_flag, excess_count, offs_count, offs_scaling, extra_count,
                          len_count)
               ? sp
               : MP_KRAKEN_MALFORMED;
}

// Rebuilds a sub-chunk from its streams into dst (the whole block, size bytes); sub: delta literals.
static int lz_runs(const mp_kraken *k, int sub, const lzt_t *lzt, uint8_t *dst, ix size, ix dst_pos, ix dst_end,
                   ix window_start) {
    ix cmd_pos = 0, len_pos = 0, lit_pos = 0, offs_pos = 0;
    const ix cmd_end = lzt->command_count, len_end = lzt->length_count, lit_end = lzt->literal_count,
             offs_end = lzt->distance_count;
    int recent[7] = {0, 0, 0, -8, -8, -8, 0};
    ix last_offset = -8;
    const uint8_t *lit = k->literals;

    while (cmd_pos < cmd_end) {
        uint32_t f = k->commands[cmd_pos++];
        ix litlen = f & 3;
        int offs_index = (int)(f >> 6);
        uint32_t matchlen = (f >> 2) & 0xF;
        if (litlen == 3) {
            if (len_pos >= len_end) return 0;
            litlen = k->lengths[len_pos++];
        }
        recent[6] = offs_pos < offs_end ? k->distances[offs_pos] : 0;
        if (litlen != 0) {
            if (litlen < 0 || lit_pos + litlen > lit_end || dst_pos + litlen > dst_end) return 0;
            if (sub) {
                if (dst_pos + last_offset < 0 || dst_pos + last_offset + litlen > size) return 0;
                for (ix c = 0; c < litlen; ++c)
                    dst[dst_pos + c] = (uint8_t)(lit[lit_pos + c] + dst[dst_pos + c + last_offset]);
            } else {
                memcpy(dst + dst_pos, lit + lit_pos, (size_t)litlen);
            }
            dst_pos += litlen;
            lit_pos += litlen;
        }
        int offset = recent[offs_index + 3];
        recent[offs_index + 3] = recent[offs_index + 2];
        recent[offs_index + 2] = recent[offs_index + 1];
        recent[offs_index + 1] = recent[offs_index + 0];
        recent[3] = offset;
        last_offset = offset;
        offs_pos += ((offs_index + 1) & 4) >> 2;

        if (dst_pos + offset < window_start) return 0;
        ix from = dst_pos + offset;
        ix actual;
        if (matchlen != 15) {
            actual = matchlen + 2;
        } else {
            if (len_pos >= len_end) return 0;
            actual = 14 + (ix)k->lengths[len_pos++];
        }
        if (actual < 0 || dst_pos + actual > dst_end || from < 0 || from + actual > size) return 0;
        if (offset <= -16) {
            // the source ends 16 bytes or more behind the destination: copy 16 at a time
            ix c = 0;
            for (; c + 16 <= actual; c += 16) memcpy(dst + dst_pos + c, dst + from + c, 16);
            for (; c < actual; ++c) dst[dst_pos + c] = dst[from + c];
        } else {
            for (ix c = 0; c < actual; ++c) dst[dst_pos + c] = dst[from + c];   // overlapping: byte by byte
        }
        dst_pos += actual;
    }
    if (offs_pos != offs_end || len_pos != len_end) return 0;
    ix final_len = dst_end - dst_pos;
    if (final_len != lit_end - lit_pos) return 0;
    if (sub) {
        if (dst_pos + last_offset < 0) return 0;
        for (ix c = 0; c < final_len; ++c)
            dst[dst_pos + c] = (uint8_t)(lit[lit_pos + c] + dst[dst_pos + c + last_offset]);
    } else if (final_len > 0) {
        memcpy(dst + dst_pos, lit + lit_pos, (size_t)final_len);
    }
    return 1;
}

static int decode_chunk(mp_kraken *k, cbuf src, ix src_start, ix src_len, uint8_t *dst, ix size, ix dst_start,
                        int dst_size, int with_seed, int sub) {
    if (dst_size <= 0 || dst_start + dst_size > size || src_len < 0 || src_start + src_len > src.n)
        return MP_KRAKEN_MALFORMED;
    ix offset = with_seed ? 0 : dst_start;
    lzt_t lzt = {0, 0, 0, 0};
    ix rc = read_lz_table(k, src, src_start, src_start + src_len, dst, dst_start, dst_size, offset, &lzt);
    if (rc < 0) return (int)rc;
    ix start = dst_start + (offset == 0 ? SEED : 0);
    ix window_start = dst_start - offset;
    return lz_runs(k, sub, &lzt, dst, size, start, dst_start + dst_size, window_start) ? MP_KRAKEN_OK
                                                                                     : MP_KRAKEN_MALFORMED;
}

static int decode_bare(cbuf src, ix src_start, ix src_len, uint8_t *dst, ix dst_start, int dst_len) {
    int count = 0;
    ix used = decode_bytes(src, src_start, src_start + src_len, dst + dst_start, dst_len, &count);
    if (used < 0) return (int)used;
    if (used != src_len || count != dst_len) return MP_KRAKEN_MALFORMED;
    return MP_KRAKEN_OK;
}

static int decode_sub_chunk(mp_kraken *k, cbuf src, ix src_start, ix src_len, uint8_t *dst, ix size, ix dst_start,
                            int dst_len, int new_lz, int restart, int sub) {
    if (src_len < 0 || src_start + src_len > src.n || dst_len < 0 || dst_start + dst_len > size)
        return MP_KRAKEN_MALFORMED;
    if (src_len == dst_len) {   // stored as is
        memcpy(dst + dst_start, src.p + src_start, (size_t)dst_len);
        return MP_KRAKEN_OK;
    }
    return new_lz ? decode_chunk(k, src, src_start, src_len, dst, size, dst_start, dst_len, restart, sub)
                  : decode_bare(src, src_start, src_len, dst, dst_start, dst_len);
}

int mp_kraken_decode(mp_kraken *k, const uint8_t *src, size_t src_len, int flags, size_t first_chunk, uint8_t *dst,
                     size_t dst_len) {
    if (!k) return MP_KRAKEN_NOMEM;
    if (dst_len == 0) return src_len == 0 ? MP_KRAKEN_OK : MP_KRAKEN_MALFORMED;
    if (dst_len > MP_KRAKEN_BLOCK || src_len > MP_KRAKEN_BLOCK * 2) return MP_KRAKEN_MALFORMED;
    cbuf s = {src, (ix)src_len};
    ix size = (ix)dst_len;
    int newlz0 = (flags & MP_KRAKEN_NEWLZ0) != 0, sub0 = (flags & MP_KRAKEN_SUB0) != 0;
    if (dst_len <= MP_KRAKEN_CHUNK)
        return decode_sub_chunk(k, s, 0, s.n, dst, size, 0, (int)size, newlz0, 1, sub0);
    if (first_chunk == 0 || first_chunk > src_len) return MP_KRAKEN_MALFORMED;
    ix first = (ix)first_chunk;
    int rc = decode_sub_chunk(k, s, 0, first, dst, size, 0, (int)MP_KRAKEN_CHUNK, newlz0, 1, sub0);
    if (rc != MP_KRAKEN_OK) return rc;
    return decode_sub_chunk(k, s, first, s.n - first, dst, size, MP_KRAKEN_CHUNK, (int)(size - MP_KRAKEN_CHUNK),
                            (flags & MP_KRAKEN_NEWLZ1) != 0, (flags & MP_KRAKEN_RESTART1) != 0,
                            (flags & MP_KRAKEN_SUB1) != 0);
}
