// SPDX-License-Identifier: GPL-3.0-or-later
// The cryptography a PS5 debug package (FPKG) needs to be read: AES-128 (FIPS-197) in XTS
// (IEEE 1619) and CBC (SP 800-38A) mode, SHA3-256 (FIPS-202) and HMAC-SHA256 (RFC 2104, on
// mp_hash's SHA-256). Portable C11, with the CPU's AES instructions on 64-bit x86 PCs that have them
// (the same results either way). Written for MaxPack from the standards; no code taken.
#ifndef MP_CRYPTO_H
#define MP_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

// An AES-128 key, expanded for both directions.
typedef struct {
    uint32_t ek[44];   // encryption round keys (little-endian words: the bytes of FIPS-197's schedule)
    uint32_t dk[44];   // decryption round keys of the equivalent inverse cipher
} mp_aes128;

void mp_aes128_init(mp_aes128 *a, const uint8_t key[16]);
// n blocks of 16 bytes; in and out may be the same.
void mp_aes128_encrypt(const mp_aes128 *a, const uint8_t *in, uint8_t *out, size_t nblocks);
void mp_aes128_decrypt(const mp_aes128 *a, const uint8_t *in, uint8_t *out, size_t nblocks);

// XTS of a part of one data unit, in place: the n bytes (a multiple of 16) at byte `at` (a multiple
// of 16) of the unit whose tweak is E_tweak(sector as a 128-bit little-endian number). A PS5
// package's outer PFS makes each 64 KiB block one unit.
void mp_xts_decrypt(const mp_aes128 *data, const mp_aes128 *tweak, uint64_t sector, uint64_t at, uint8_t *buf,
                    size_t n);
void mp_xts_encrypt(const mp_aes128 *data, const mp_aes128 *tweak, uint64_t sector, uint64_t at, uint8_t *buf,
                    size_t n);

// CBC with no padding, in place; n is a multiple of 16.
void mp_cbc_decrypt(const mp_aes128 *a, const uint8_t iv[16], uint8_t *buf, size_t n);
void mp_cbc_encrypt(const mp_aes128 *a, const uint8_t iv[16], uint8_t *buf, size_t n);

// 1 if AES runs on the CPU's AES instructions, 0 if on the portable code. mp_aes_use_accel(0) forces
// the portable code (for the tests) and mp_aes_use_accel(1) goes back to the instructions where there are any.
int mp_aes_accel(void);
void mp_aes_use_accel(int on);

// SHA3-256.
typedef struct {
    uint64_t st[25];
    size_t n;   // bytes of the current block absorbed
} mp_sha3;
void mp_sha3_init(mp_sha3 *s);
void mp_sha3_update(mp_sha3 *s, const void *p, size_t n);
void mp_sha3_final(mp_sha3 *s, uint8_t out[32]);
void mp_sha3_256(const void *p, size_t n, uint8_t out[32]);

// HMAC-SHA256 of the message, with a key of any length.
void mp_hmac_sha256(const void *key, size_t klen, const void *msg, size_t n, uint8_t out[32]);

#endif
