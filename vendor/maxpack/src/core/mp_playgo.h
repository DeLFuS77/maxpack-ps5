// SPDX-License-Identifier: GPL-3.0-or-later
// The list of the files of a PS5 package's image that the package carries in
// sce_sys/playgo-hash-table.dat: one 64-bit hash per path (the PS5 flat path table hash), without
// the names. Origin of the hash and of the layout: see mp_playgo.c.
#ifndef MP_PLAYGO_H
#define MP_PLAYGO_H

#include <stddef.h>
#include <stdint.h>

#define MP_PLAYGO_TABLE_PATH "sce_sys/playgo-hash-table.dat"
#define MP_PLAYGO_HEADER 0x38u
// The most a table is read: 8 million paths, far more than any game has.
#define MP_PLAYGO_TABLE_MAX (64u << 20)

// The flat path table hash of a path: its bytes as they are (UTF-8), ASCII letters in upper case,
// without the leading '/'.
uint64_t mp_flt_hash(const char *path);
uint64_t mp_flt_hash_n(const char *path, size_t n);

// Reads a playgo-hash-table.dat: 0 with its hashes in *hashes (malloc, sorted, each once) and their
// number in *count; -1 with *why "unknown_format" (not this format) or "damaged" (this format, but
// its size disagrees with its count, or it lists nothing). Never reads beyond n.
int mp_playgo_parse(const uint8_t *d, size_t n, uint64_t **hashes, size_t *count, const char **why);

#endif
