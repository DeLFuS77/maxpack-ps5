// SPDX-License-Identifier: GPL-3.0-or-later
// v2 format experiments: grouping small files (M5) and a trained dictionary (M6).
#ifndef MP_V2EXP_H
#define MP_V2EXP_H

#include <stddef.h>
#include <stdint.h>

#include "mp_measure.h"
#include "mp_parallel.h"
#include "mp_sample.h"
#include "mp_source.h"

#define MP_SMALL_MAX 65535u          // "files < 64 KiB"
#define MP_DICT_SIZE (110u * 1024u)

// Cross-validation: dict[p] is trained only with units of parity p (and applied to those of
// parity 1 - p). If ZDICT cannot train (little data), dict[p] stays NULL: it is not an error.
// Blocks: units = 256 KiB blocks of the sample's windows; parity = ordinal & 1;
// at most 16 MiB per parity, spread across the whole sample.
int mp_dict_train_blocks(mp_source *src, const uint32_t *packable, const mp_sample *s,
                         mp_dict_pair *out, mp_err *e);
// Small: units = whole files; parity = position in `small` & 1; 8 MiB per parity.
int mp_dict_train_small(mp_source *src, const uint32_t *small, size_t n, mp_dict_pair *out,
                        mp_err *e);
void mp_dict_pair_free(mp_dict_pair *p);

typedef struct {
    uint64_t files, raw;
    uint64_t individual;        // one zstd 19 frame per file (what v1 does)
    uint64_t grouped;           // concatenated in path order and cut into 256 KiB blocks
    uint64_t individual_dict;   // one frame per file with dict[1 - parity]
    int dicts;                  // dictionaries available (0-2)
} mp_small_result;

// small: source indices of the small packable files, in path order.
int mp_small_run(mp_source *src, const uint32_t *small, size_t n, const mp_dict_pair *dicts,
                 unsigned threads, mp_progress *prog, mp_small_result *out, mp_err *e);

#endif
