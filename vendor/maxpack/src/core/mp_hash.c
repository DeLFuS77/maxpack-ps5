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

// SHA-256 with the CPU's SHA instructions (x86 SHA extensions: Intel since Goldmont and Ice Lake, AMD
// since Zen), several times faster than the portable code. Only on 64-bit x86 PCs (Windows, Linux,
// macOS); elsewhere, and on CPUs without them, the portable code runs. Same digests either way.
#if (defined(_M_X64) || defined(__x86_64__)) && (defined(_WIN32) || defined(__linux__) || defined(__APPLE__))
#define MP_SHA_NI 1
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#define MP_SHA_TARGET
#else
#include <cpuid.h>
#define MP_SHA_TARGET __attribute__((target("sha,sse4.1,ssse3")))
#endif

static int cpu_has_sha(void) {
    unsigned b, c;
#ifdef _MSC_VER
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return 0;
    __cpuid(r, 1);
    c = (unsigned)r[2];
    __cpuidex(r, 7, 0);
    b = (unsigned)r[1];
#else
    unsigned a, d, c1;
    if (__get_cpuid_max(0, NULL) < 7) return 0;
    __cpuid(1, a, b, c1, d);
    __cpuid_count(7, 0, a, b, c, d);
    c = c1;
#endif
    // SHA (leaf 7, EBX bit 29), SSE4.1 (leaf 1, ECX bit 19) and SSSE3 (leaf 1, ECX bit 9).
    return (b >> 29 & 1) && (c >> 19 & 1) && (c >> 9 & 1);
}

MP_SHA_TARGET static void sha_blocks_ni(uint32_t h[8], const uint8_t *p, size_t nblocks) {
    const __m128i bswap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    // The instructions keep the state as two halves, ABEF and CDGH.
    __m128i t = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&h[0]), 0xB1);
    __m128i s1 = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&h[4]), 0x1B);
    __m128i s0 = _mm_alignr_epi8(t, s1, 8);
    s1 = _mm_blend_epi16(s1, t, 0xF0);
    for (; nblocks; nblocks--, p += 64) {
        __m128i abef = s0, cdgh = s1, w[4];
        for (int i = 0; i < 4; i++)
            w[i] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16 * i)), bswap);
        // Rounds 4g..4g+3 with the message words W(g); W(g+1) comes from W(g-3), W(g-2), W(g-1), W(g).
        for (int g = 0; g < 16; g++) {
            __m128i m = _mm_add_epi32(w[g & 3], _mm_loadu_si128((const __m128i *)&K[4 * g]));
            s1 = _mm_sha256rnds2_epu32(s1, s0, m);
            m = _mm_shuffle_epi32(m, 0x0E);
            s0 = _mm_sha256rnds2_epu32(s0, s1, m);
            if (g >= 3 && g < 15) {
                __m128i x = _mm_sha256msg1_epu32(w[(g + 1) & 3], w[(g + 2) & 3]);
                x = _mm_add_epi32(x, _mm_alignr_epi8(w[g & 3], w[(g + 3) & 3], 4));
                w[(g + 1) & 3] = _mm_sha256msg2_epu32(x, w[g & 3]);
            }
        }
        s0 = _mm_add_epi32(s0, abef);
        s1 = _mm_add_epi32(s1, cdgh);
    }
    t = _mm_shuffle_epi32(s0, 0x1B);   // and back to A..H
    s1 = _mm_shuffle_epi32(s1, 0xB1);
    s0 = _mm_blend_epi16(t, s1, 0xF0);
    s1 = _mm_alignr_epi8(s1, t, 8);
    _mm_storeu_si128((__m128i *)&h[0], s0);
    _mm_storeu_si128((__m128i *)&h[4], s1);
}
#endif

// Which code runs: -1 not decided yet, 0 portable, 1 the CPU's SHA instructions. Every thread
// that decides finds the same value.
#ifdef _MSC_VER
static volatile long g_accel = -1;
#define ACCEL_LOAD() g_accel
#define ACCEL_STORE(v) (g_accel = (v))
#else
static int g_accel = -1;
#define ACCEL_LOAD() __atomic_load_n(&g_accel, __ATOMIC_RELAXED)
#define ACCEL_STORE(v) __atomic_store_n(&g_accel, (v), __ATOMIC_RELAXED)
#endif

int mp_sha256_accel(void) {
    int a = (int)ACCEL_LOAD();
    if (a < 0) {
#ifdef MP_SHA_NI
        a = cpu_has_sha();
#else
        a = 0;
#endif
        ACCEL_STORE(a);
    }
    return a;
}

void mp_sha256_use_accel(int on) { ACCEL_STORE(on ? -1 : 0); }   // -1: decided again

static void sha_blocks(uint32_t h[8], const uint8_t *p, size_t nblocks) {
#ifdef MP_SHA_NI
    if (mp_sha256_accel()) {
        sha_blocks_ni(h, p, nblocks);
        return;
    }
#endif
    for (; nblocks; nblocks--, p += 64) sha_block(h, p);
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
        sha_blocks(s->h, s->buf, 1);
        s->n = 0;
    }
    if (n >= 64) {
        sha_blocks(s->h, p, n / 64);
        p += n / 64 * 64;
        n %= 64;
    }
    memcpy(s->buf, p, n);
    s->n = n;
}

void mp_sha256_final(mp_sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    uint64_t keep = s->len;   // the padding goes through update but does not count as message
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
