// SPDX-License-Identifier: GPL-3.0-or-later
// The PS5 flat path table hash and the layout of sce_sys/playgo-hash-table.dat.
//
// The hash is adapted from FltPathHash (Containers/ProsperoOuterPfsBuilder.cs) and the layout of the
// table from ProsperoPlayGoHashTableBuilder (Containers/ProsperoPlayGoBuilders.cs) of ProsperoPkgTool,
// https://github.com/pearlxcore/ProsperoPkgTool, under this license:
//
// MIT License
//
// Copyright (c) 2026 pearlxcore
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// (end of the license of ProsperoPkgTool)
//
// The same hash, in ps5upload's flt.rs (GPL-3.0-or-later), reproduces the outer tables of real
// packages; its two known answers are in tests/test_complete.c. Unlike FltPathHash, which takes a
// C# string and keeps the low byte of each UTF-16 unit, this hashes the path's UTF-8 bytes as they
// are, as flt.rs does: the images of PS5 packages name their files in ASCII, where both agree.
#include "mp_playgo.h"

#include <stdlib.h>
#include <string.h>

#define FLT_SEED0 0x92CA8AAB26A24F51ull
#define FLT_SEED1 0x09BBB761A41BC44Dull
#define FLT_ROUND 0x8000000080008081ull

static uint64_t rotl(uint64_t x, unsigned n) {
    n &= 63u;
    return n ? (x << n) | (x >> (64u - n)) : x;
}
static uint64_t rotr(uint64_t x, unsigned n) { return rotl(x, (64u - n) & 63u); }

static uint64_t up(unsigned char c) { return (uint64_t)(c >= 'a' && c <= 'z' ? c - 32 : c); }

uint64_t mp_flt_hash_n(const char *path, size_t n) {
    const unsigned char *p = (const unsigned char *)path;
    while (n && *p == '/') {
        p++;
        n--;
    }
    uint64_t a = FLT_SEED0, b = rotl(FLT_SEED0, 11), c = rotl(FLT_SEED0, 23), tail = 0;
    if (n) {
        size_t words = (n - 1) >> 3, pos = 0;
        for (size_t w = 0; w < words; w++, pos += 8) {
            uint64_t word = 0;
            for (unsigned k = 0; k < 8; k++) word |= up(p[pos + k]) << (8 * k);
            a ^= word;
            uint64_t t18 = rotr(rotl(c ^ b, 5) ^ a, 11);
            uint64_t t12 = rotl(rotl(c ^ a, 17) ^ b, 11);
            uint64_t c2 = rotr(rotl(b ^ a, 1) ^ c, 5);
            a = (~t12 & c2) ^ t18 ^ FLT_ROUND;
            b = (~c2 & t18) ^ t12;
            c = (~t18 & t12) ^ c2;
        }
        for (unsigned k = 0; pos + k < n; k++) tail |= up(p[pos + k]) << (8 * k);
    }
    uint64_t x16 = b, x11 = c, x6 = tail ^ a ^ FLT_SEED1;
    uint64_t u17 = rotl(x11 ^ x16, 5) ^ x6;
    uint64_t u18 = rotl(x11 ^ x6, 17) ^ x16;
    uint64_t x11b = rotl(x16 ^ x6, 1) ^ x11;
    return (~rotl(u18, 11) & rotr(x11b, 5)) ^ rotr(u17, 11) ^ FLT_ROUND;
}

uint64_t mp_flt_hash(const char *path) { return mp_flt_hash_n(path, strlen(path)); }

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// The header (0x38 bytes): version 1 at 0x00, the offset of the hashes (0x38) at 0x08, their bytes
// at 0x0C, "\x7FFLT" at 0x18, the count at 0x24, the two seeds of the hash at 0x28. Then the
// hashes, one u64 each, little endian, sorted.
int mp_playgo_parse(const uint8_t *d, size_t n, uint64_t **hashes, size_t *count, const char **why) {
    *hashes = NULL;
    *count = 0;
    *why = "unknown_format";
    if (n < MP_PLAYGO_HEADER || rd32(d) != 1 || rd32(d + 8) != MP_PLAYGO_HEADER || memcmp(d + 0x18, "\x7f" "FLT", 4))
        return -1;
    *why = "damaged";
    uint64_t k = rd32(d + 0x24);
    if (k == 0 || (n - MP_PLAYGO_HEADER) % 8 || (n - MP_PLAYGO_HEADER) / 8 != k || rd32(d + 0x0C) != 8 * k)
        return -1;
    uint64_t *v = malloc((size_t)k * sizeof *v);
    if (!v) {
        *why = "unreadable";
        return -1;
    }
    for (size_t i = 0; i < (size_t)k; i++) v[i] = rd64(d + MP_PLAYGO_HEADER + 8 * i);
    qsort(v, (size_t)k, sizeof *v, cmp_u64);
    size_t m = 0;
    for (size_t i = 0; i < (size_t)k; i++)
        if (!m || v[m - 1] != v[i]) v[m++] = v[i];
    *hashes = v;
    *count = m;
    *why = "";
    return 0;
}
