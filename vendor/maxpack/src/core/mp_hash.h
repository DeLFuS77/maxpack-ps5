// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MP_HASH_H
#define MP_HASH_H

#include <stddef.h>
#include <stdint.h>

// XXH64 (the copy of xxhash that zstd ships, inline).
uint64_t mp_xxh64(const void *p, size_t n, uint64_t seed);

// SHA-256 (FIPS 180-4), in portable C.
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} mp_sha256;
void mp_sha256_init(mp_sha256 *s);
void mp_sha256_update(mp_sha256 *s, const void *p, size_t n);
void mp_sha256_final(mp_sha256 *s, uint8_t out[32]);
void mp_sha256_hex(const uint8_t d[32], char out[65]);
// 1 if SHA-256 runs on the CPU's SHA instructions (64-bit x86 PCs that have them), 0 if on the
// portable code; the digests are the same. mp_sha256_use_accel(0) forces the portable code (for the
// tests) and mp_sha256_use_accel(1) goes back to the instructions where there are any.
int mp_sha256_accel(void);
void mp_sha256_use_accel(int on);

// 64-bit FNV-1a. Careful: AMPRZPK1 uses the standard basis and AMPRIDX3 the one from build_ampr_index.py;
// mixing them already broke games in exFAT2AMPR.
#define MP_FNV_BASIS_STD 14695981039346656037ULL
#define MP_FNV_BASIS_IDX3 1469598103934665603ULL
uint64_t mp_fnv1a64(const void *p, size_t n, uint64_t basis);
// AMPRZPK1 path hash: standard basis over the relative path as is (with its capitals).
uint64_t mp_zpk_path_hash(const char *rel);
// AMPRIDX3 path hash: "/app0/..." with A-Z turned into a-z, IDX3 basis; if it comes out 0, 1.
uint64_t mp_idx3_path_hash(const char *app0_path);

// CRC-32 IEEE (zlib's). prev = 0 to start.
uint32_t mp_crc32(const void *p, size_t n, uint32_t prev);

#endif
