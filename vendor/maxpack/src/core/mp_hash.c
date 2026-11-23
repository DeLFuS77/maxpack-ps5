// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_hash.h"

#include <string.h>
#include <zlib.h>

#define XXH_INLINE_ALL
#include "common/xxhash.h"

uint64_t mp_xxh64(const void *p, size_t n, uint64_t seed) { return XXH64(p, n, seed); }

// ---- SHA-256 ------------------------------------------------------------------------------

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
               p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

void mp_sha256_init(mp_sha256 *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->n = 0;
}

void mp_sha256_update(mp_sha256 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->len += n;
    if (s->n) {
        size_t take = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        n -= take;
        if (s->n < 64) return;
        sha_block(s->h, s->buf);
        s->n = 0;
    }
    for (; n >= 64; p += 64, n -= 64) sha_block(s->h, p);
    memcpy(s->buf, p, n);
    s->n = n;
}

void mp_sha256_final(mp_sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    uint64_t keep = s->len;   // el relleno pasa por update pero no cuenta como mensaje
    mp_sha256_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->n != 56) mp_sha256_update(s, &zero, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    mp_sha256_update(s, lb, 8);
    s->len = keep;
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void mp_sha256_hex(const uint8_t d[32], char out[65]) {
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = hexd[d[i] >> 4];
        out[2 * i + 1] = hexd[d[i] & 15];
    }
    out[64] = 0;
}

// ---- FNV-1a y CRC-32 ------------------------------------------------------------------------

uint64_t mp_fnv1a64(const void *data, size_t n, uint64_t basis) {
    const uint8_t *p = data;
    uint64_t h = basis;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t mp_zpk_path_hash(const char *rel) { return mp_fnv1a64(rel, strlen(rel), MP_FNV_BASIS_STD); }

uint64_t mp_idx3_path_hash(const char *app0_path) {
    uint64_t h = MP_FNV_BASIS_IDX3;
    for (const unsigned char *p = (const unsigned char *)app0_path; *p; p++) {
        unsigned char c = *p >= 'A' && *p <= 'Z' ? (unsigned char)(*p + 32) : *p;
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

uint32_t mp_crc32(const void *p, size_t n, uint32_t prev) {
    uLong c = prev;
    const Bytef *b = p;
    while (n) {
        uInt k = n > 0x40000000u ? 0x40000000u : (uInt)n;
        c = crc32(c, b, k);
        b += k;
        n -= k;
    }
    return (uint32_t)c;
}
