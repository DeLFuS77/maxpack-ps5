// SPDX-License-Identifier: GPL-3.0-or-later
// ampr_emu.index (AMPRIDX3): the list of all the game's files that the runtime uses to give
// an id to each path. Format identical to build_ampr_index.py's write_index (drakmor/ampr_emu).
#ifndef MP_IDX3_H
#define MP_IDX3_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

typedef struct {
    const char *path;   // relative to the game, without "/app0/"
    uint64_t size;
    int64_t mtime;
} mp_idx3_row;

// Validates the paths and that no two are equal apart from case, and builds the file (malloc).
int mp_idx3_build(const mp_idx3_row *rows, size_t n, uint8_t **out, size_t *out_len, mp_err *e);

typedef struct {
    char *app0_path;   // "/app0/..."
    uint64_t size;
    int64_t mtime;
} mp_idx3_entry;

// Checks the file like ampr-emu-ng's Index::check and returns its rows.
int mp_idx3_parse(const uint8_t *d, size_t n, mp_idx3_entry **out, size_t *count, mp_err *e);
void mp_idx3_entries_free(mp_idx3_entry *v, size_t n);
// Row of the relative path rel ("/app0/" + rel, case-insensitive), or -1. The rows of a valid
// index are sorted by that key (mp_idx3_parse checks it).
long mp_idx3_find(const mp_idx3_entry *v, size_t n, const char *rel);
// Copy of the index (malloc) with row 'rel' changed, or added, to size/mtime. Validates like
// mp_idx3_parse. It is what pack --runtime does with the fakelib/libSceAmpr.sprx row.
int mp_idx3_set_row(const uint8_t *idx, size_t n, const char *rel, uint64_t size, int64_t mtime,
                    uint8_t **out, size_t *out_len, mp_err *e);

// Validates a relative path for both indices (AMPRZPK1 and AMPRIDX3). 0 if it is valid.
int mp_pack_path_check(const char *rel, mp_err *e);

#endif
