// SPDX-License-Identifier: GPL-3.0-or-later
// Images as bytes, and the PFSC decoder. PFSC layout (little-endian), as MkPFS writes it and the
// PS4/PS5 read it: +0x00 "PFSC", +0x04 u32 0, +0x08 u32 6, +0x0C u32 block size, +0x10 u64 block
// size again, +0x18 u64 where the block table starts, +0x20 u64 where the data starts, +0x28 u64
// logical length (a whole number of blocks). The table has one u64 per block plus one: block i is
// [t[i], t[i+1]) of the stream; exactly a block long means stored as is, shorter means a zlib
// stream that inflates to exactly one block.
#include "mp_image.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "mp_platform.h"

#define PFSC_MAGIC 0x43534650u   // "PFSC"
#define PFSC_MIN_BLOCK 4096u
#define PFSC_MAX_BLOCK (1u << 20)
#define PFSC_MAX_BLOCKS (1ull << 25)   // the table of a larger file would take more than 256 MiB
#define PFSC_BATCH 32u                 // whole blocks read from the image at once

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static char *dup_name(const char *s) {
    char *p = mp_strdup(s ? s : "");
    return p;
}

int mp_image_pread(mp_image *m, void *buf, size_t len, uint64_t off, size_t *got, mp_err *e) {
    *got = 0;
    if (off >= m->size || len == 0) return 0;
    if (len > m->size - off) len = (size_t)(m->size - off);
    if (m->read(m, buf, len, off, e) != 0) return -1;
    *got = len;
    return 0;
}

void mp_image_close(mp_image *m) {
    if (!m) return;
    if (m->close) m->close(m);
    free(m->name);
    free(m);
}

// ---- a file -------------------------------------------------------------------------------

static int file_read(mp_image *m, void *buf, size_t len, uint64_t off, mp_err *e) {
    size_t got = 0;
    if (mp_file_pread(m->impl, buf, len, off, &got, e) != 0) return -1;
    if (got != len) {
        mp_err_set(e, "truncated image: %s ended while reading it", m->name);
        return -1;
    }
    return 0;
}

static void file_close(mp_image *m) { mp_file_close(m->impl); }

int mp_image_open_file(const char *path, mp_image **out, mp_err *e) {
    *out = NULL;
    mp_image *m = calloc(1, sizeof *m);
    mp_file *f = NULL;
    if (!m || !(m->name = dup_name(path))) {
        free(m);
        mp_err_set(e, "out of memory");
        return -1;
    }
    if (mp_file_open_read(path, &f, e) != 0 || mp_file_size(f, &m->size, e) != 0) {
        mp_file_close(f);
        mp_image_close(m);
        return -1;
    }
    m->impl = f;
    m->read = file_read;
    m->close = file_close;
    *out = m;
    return 0;
}

// ---- a range of another image -------------------------------------------------------------

typedef struct {
    mp_image *parent;
    uint64_t base;
} slice;

static int slice_read(mp_image *m, void *buf, size_t len, uint64_t off, mp_err *e) {
    slice *s = m->impl;
    return s->parent->read(s->parent, buf, len, s->base + off, e);
}

static void slice_close(mp_image *m) {
    slice *s = m->impl;
    mp_image_close(s->parent);
    free(s);
}

int mp_image_slice(mp_image *parent, uint64_t off, uint64_t len, const char *name, mp_image **out,
                   mp_err *e) {
    *out = NULL;
    if (off > parent->size || len > parent->size - off) {
        mp_err_set(e, "truncated image: %s does not fit in %s", name, parent->name);
        mp_image_close(parent);
        return -1;
    }
    mp_image *m = calloc(1, sizeof *m);
    slice *s = calloc(1, sizeof *s);
    if (!m || !s || !(m->name = dup_name(name))) {
        free(m);
        free(s);
        mp_image_close(parent);
        mp_err_set(e, "out of memory");
        return -1;
    }
    s->parent = parent;
    s->base = off;
    m->size = len;
    m->compressed = parent->compressed;
    m->impl = s;
    m->read = slice_read;
    m->close = slice_close;
    *out = m;
    return 0;
}

// ---- PFSC ---------------------------------------------------------------------------------

// What one read needs: an inflater and room for the stored bytes of a few blocks and for one block.
typedef struct ctx {
    z_stream z;
    uint8_t *stored, *blk;
    struct ctx *next;
} ctx;

struct mp_pfsc {
    mp_image *img;
    uint64_t base, stored, size, nblocks;
    uint32_t bs;
    unsigned shift;
    uint64_t *offs;   // nblocks + 1, from the start of the stream
    char what[256];
    mp_mutex *mu;
    ctx *idle;        // contexts not in use
    unsigned nslots;  // decompressed blocks kept: slot b % nslots holds block tag - 1
    uint64_t *tag;
    uint8_t *slots;
};

static void ctx_free(ctx *c) {
    if (!c) return;
    inflateEnd(&c->z);
    free(c->stored);
    free(c->blk);
    free(c);
}

static ctx *ctx_take(mp_pfsc *p, mp_err *e) {
    mp_mutex_lock(p->mu);
    ctx *c = p->idle;
    if (c) p->idle = c->next;
    mp_mutex_unlock(p->mu);
    if (c) return c;
    c = calloc(1, sizeof *c);
    if (c) {
        c->stored = malloc((size_t)p->bs * PFSC_BATCH);
        c->blk = malloc(p->bs);
        if (!c->stored || !c->blk || inflateInit(&c->z) != Z_OK) {
            free(c->stored);
            free(c->blk);
            free(c);
            c = NULL;
        }
    }
    if (!c) mp_err_set(e, "out of memory");
    return c;
}

static void ctx_give(mp_pfsc *p, ctx *c) {
    mp_mutex_lock(p->mu);
    c->next = p->idle;
    p->idle = c;
    mp_mutex_unlock(p->mu);
}

// Block b (stored bytes at `in`) into out, a whole block.
static int decode(mp_pfsc *p, ctx *c, uint64_t b, const uint8_t *in, uint8_t *out, mp_err *e) {
    uint64_t n = p->offs[b + 1] - p->offs[b];
    if (n == p->bs) {
        memcpy(out, in, p->bs);
        return 0;
    }
    int rc = inflateReset(&c->z);
    if (rc == Z_OK) {
        c->z.next_in = (Bytef *)in;
        c->z.avail_in = (uInt)n;
        c->z.next_out = out;
        c->z.avail_out = p->bs;
        rc = inflate(&c->z, Z_FINISH);
    }
    if (rc != Z_STREAM_END || c->z.avail_out != 0) {
        mp_err_set(e, "%s: PFSC block %" PRIu64 " is damaged (it does not inflate to %" PRIu32 " bytes)",
                   p->what, b, p->bs);
        return -1;
    }
    return 0;
}

// Blocks [b, b + k) whole into out (k <= PFSC_BATCH).
static int decode_run(mp_pfsc *p, ctx *c, uint64_t b, uint64_t k, uint8_t *out, mp_err *e) {
    uint64_t from = p->offs[b], n = p->offs[b + k] - from;
    size_t got = 0;
    if (mp_image_pread(p->img, c->stored, (size_t)n, p->base + from, &got, e) != 0) return -1;
    if (got != n) {
        mp_err_set(e, "truncated image: the compressed data of %s is missing", p->what);
        return -1;
    }
    for (uint64_t i = 0; i < k; i++)
        if (decode(p, c, b + i, c->stored + (p->offs[b + i] - from), out + i * p->bs, e) != 0) return -1;
    return 0;
}

// Part [boff, boff + n) of block b, through the cache.
static int read_part(mp_pfsc *p, ctx *c, uint64_t b, size_t boff, size_t n, uint8_t *out, mp_err *e) {
    if (p->nslots) {
        unsigned s = (unsigned)(b % p->nslots);
        mp_mutex_lock(p->mu);
        int hit = p->tag[s] == b + 1;
        if (hit) memcpy(out, p->slots + (size_t)s * p->bs + boff, n);
        mp_mutex_unlock(p->mu);
        if (hit) return 0;
    }
    if (decode_run(p, c, b, 1, c->blk, e) != 0) return -1;
    memcpy(out, c->blk + boff, n);
    if (p->nslots) {
        unsigned s = (unsigned)(b % p->nslots);
        mp_mutex_lock(p->mu);
        memcpy(p->slots + (size_t)s * p->bs, c->blk, p->bs);
        p->tag[s] = b + 1;
        mp_mutex_unlock(p->mu);
    }
    return 0;
}

int mp_pfsc_read(mp_pfsc *p, void *buf, size_t len, uint64_t off, mp_err *e) {
    if (len == 0) return 0;
    if (off > p->size || len > p->size - off) {
        mp_err_set(e, "%s: read outside the file", p->what);
        return -1;
    }
    ctx *c = ctx_take(p, e);
    if (!c) return -1;
    uint8_t *o = buf;
    uint64_t end = off + len;
    int rc = 0;
    while (rc == 0 && off < end) {
        uint64_t b = off >> p->shift;
        size_t boff = (size_t)(off & (p->bs - 1));
        uint64_t whole = boff ? 0 : (end - off) >> p->shift;
        if (whole) {
            uint64_t k = whole < PFSC_BATCH ? whole : PFSC_BATCH;
            rc = decode_run(p, c, b, k, o, e);
            o += k * p->bs;
            off += k * p->bs;
        } else {
            size_t n = p->bs - boff;
            if (n > end - off) n = (size_t)(end - off);
            rc = read_part(p, c, b, boff, n, o, e);
            o += n;
            off += n;
        }
    }
    ctx_give(p, c);
    return rc;
}

void mp_pfsc_free(mp_pfsc *p) {
    if (!p) return;
    while (p->idle) {
        ctx *c = p->idle;
        p->idle = c->next;
        ctx_free(c);
    }
    mp_mutex_free(p->mu);
    free(p->offs);
    free(p->tag);
    free(p->slots);
    free(p);
}

static int bad(mp_err *e, const mp_pfsc *p, const char *why) {
    mp_err_set(e, "%s: invalid PFSC data (%s)", p->what, why);
    return -1;
}

static int pfsc_parse(mp_pfsc *p, mp_err *e) {
    uint8_t h[MP_PFSC_HEADER];
    size_t got = 0;
    if (p->stored < sizeof h) return bad(e, p, "shorter than its header");
    if (p->size > MP_PFSC_MAX_LOGICAL) return bad(e, p, "absurd size");
    if (mp_image_pread(p->img, h, sizeof h, p->base, &got, e) != 0) return -1;
    if (got != sizeof h) {
        mp_err_set(e, "truncated image: the compressed data of %s is missing", p->what);
        return -1;
    }
    if (rd32(h) != PFSC_MAGIC) return bad(e, p, "no PFSC signature");
    uint32_t bs = rd32(h + 0x0C);
    uint64_t bs2 = rd64(h + 0x10), table = rd64(h + 0x18), data = rd64(h + 0x20), dlen = rd64(h + 0x28);
    if (bs < PFSC_MIN_BLOCK || bs > PFSC_MAX_BLOCK || (bs & (bs - 1)) || bs2 != bs)
        return bad(e, p, "block size");
    p->bs = bs;
    while ((1u << p->shift) < bs) p->shift++;
    if (dlen % bs) return bad(e, p, "length is not a whole number of blocks");
    if (p->size > dlen) return bad(e, p, "shorter than the file");
    uint64_t all = dlen / bs;   // blocks in the table
    p->nblocks = (p->size + bs - 1) / bs;
    if (p->nblocks > PFSC_MAX_BLOCKS) return bad(e, p, "absurd size");
    if (table < MP_PFSC_HEADER || table > p->stored || all >= (p->stored - table) / 8 ||
        table + (all + 1) * 8 > data || data > p->stored)
        return bad(e, p, "position of the block table or the data");
    size_t tbytes = (size_t)(p->nblocks + 1) * 8;
    uint8_t *raw = malloc(tbytes);
    p->offs = malloc(tbytes);
    if (!raw || !p->offs) {
        free(raw);
        mp_err_set(e, "out of memory");
        return -1;
    }
    int rc = mp_image_pread(p->img, raw, tbytes, p->base + table, &got, e);
    if (rc == 0 && got != tbytes) {
        mp_err_set(e, "truncated image: the compressed data of %s is missing", p->what);
        rc = -1;
    }
    for (uint64_t i = 0; rc == 0 && i <= p->nblocks; i++) p->offs[i] = rd64(raw + 8 * i);
    free(raw);
    if (rc != 0) return -1;
    if (p->offs[0] < data) return bad(e, p, "the first block starts inside the table");
    for (uint64_t i = 0; i < p->nblocks; i++) {
        if (p->offs[i + 1] < p->offs[i]) return bad(e, p, "block offsets go backwards");
        uint64_t n = p->offs[i + 1] - p->offs[i];
        if (n == 0 || n > bs) return bad(e, p, "a block is empty or longer than a block");
    }
    if (p->offs[p->nblocks] > p->stored) return bad(e, p, "blocks past the end of the file");
    return 0;
}

int mp_pfsc_open(mp_image *img, uint64_t off, uint64_t stored, uint64_t size, const char *what,
                 unsigned cache_blocks, mp_pfsc **out, mp_err *e) {
    *out = NULL;
    mp_pfsc *p = calloc(1, sizeof *p);
    if (!p || !(p->mu = mp_mutex_new())) {
        free(p);
        mp_err_set(e, "out of memory");
        return -1;
    }
    p->img = img;
    p->base = off;
    p->stored = stored;
    p->size = size;
    snprintf(p->what, sizeof p->what, "%s", what);
    if (off > img->size || stored > img->size - off) {
        mp_err_set(e, "truncated image: the compressed data of %s is missing", p->what);
        mp_pfsc_free(p);
        return -1;
    }
    if (pfsc_parse(p, e) != 0) {
        mp_pfsc_free(p);
        return -1;
    }
    if (cache_blocks && p->nblocks) {
        p->nslots = cache_blocks;
        p->tag = calloc(cache_blocks, sizeof *p->tag);
        p->slots = malloc((size_t)cache_blocks * p->bs);
        if (!p->tag || !p->slots) {
            mp_pfsc_free(p);
            mp_err_set(e, "out of memory");
            return -1;
        }
    }
    *out = p;
    return 0;
}

// ---- a PFSC file as an image ----------------------------------------------------------------

typedef struct {
    mp_image *parent;
    mp_pfsc *dec;
} pfsc_view;

static int view_read(mp_image *m, void *buf, size_t len, uint64_t off, mp_err *e) {
    pfsc_view *v = m->impl;
    return mp_pfsc_read(v->dec, buf, len, off, e);
}

static void view_close(mp_image *m) {
    pfsc_view *v = m->impl;
    mp_pfsc_free(v->dec);
    mp_image_close(v->parent);
    free(v);
}

int mp_image_pfsc_from(mp_image *parent, mp_pfsc *dec, const char *name, mp_image **out, mp_err *e) {
    *out = NULL;
    mp_image *m = calloc(1, sizeof *m);
    pfsc_view *v = calloc(1, sizeof *v);
    if (!m || !v || !(m->name = dup_name(name))) {
        free(m);
        free(v);
        mp_pfsc_free(dec);
        mp_image_close(parent);
        mp_err_set(e, "out of memory");
        return -1;
    }
    v->parent = parent;
    v->dec = dec;
    m->size = dec->size;
    m->compressed = 1;
    m->impl = v;
    m->read = view_read;
    m->close = view_close;
    *out = m;
    return 0;
}

int mp_image_pfsc(mp_image *parent, uint64_t off, uint64_t stored, uint64_t size, const char *name,
                  mp_image **out, mp_err *e) {
    *out = NULL;
    mp_pfsc *dec = NULL;
    // 64 blocks (4 MiB of 64 KiB blocks): a reader that takes small pieces of one block, as an
    // exFAT image with clusters smaller than a block does, decompresses it once.
    if (mp_pfsc_open(parent, off, stored, size, name, 64, &dec, e) != 0) {
        mp_image_close(parent);
        return -1;
    }
    return mp_image_pfsc_from(parent, dec, name, out, e);
}
