// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MP_HASH_H
#define MP_HASH_H

#include <stddef.h>
#include <stdint.h>

// XXH64 (la copia de xxhash que trae zstd, en línea).
uint64_t mp_xxh64(const void *p, size_t n, uint64_t seed);

// SHA-256 (FIPS 180-4), en C portable.
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

// FNV-1a de 64 bits. Ojo: AMPRZPK1 usa la base estándar y AMPRIDX3 la de build_ampr_index.py;
// mezclarlas ya rompió juegos en exFAT2AMPR.
#define MP_FNV_BASIS_STD 14695981039346656037ULL
#define MP_FNV_BASIS_IDX3 1469598103934665603ULL
uint64_t mp_fnv1a64(const void *p, size_t n, uint64_t basis);
// Hash de ruta de AMPRZPK1: base estándar sobre la ruta relativa tal cual (con mayúsculas).
uint64_t mp_zpk_path_hash(const char *rel);
// Hash de ruta de AMPRIDX3: "/app0/..." con A-Z pasadas a a-z, base IDX3; si sale 0, 1.
uint64_t mp_idx3_path_hash(const char *app0_path);

// CRC-32 IEEE (el de zlib). prev = 0 para empezar.
uint32_t mp_crc32(const void *p, size_t n, uint32_t prev);

#endif
