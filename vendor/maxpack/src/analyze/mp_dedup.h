// SPDX-License-Identifier: GPL-3.0-or-later
// Exact deduplication (improvement M4): reads everything packable and counts repeated blocks and files.
#ifndef MP_DEDUP_H
#define MP_DEDUP_H

#include <stddef.h>
#include <stdint.h>

#include "mp_parallel.h"
#include "mp_source.h"

typedef struct {
    uint64_t blocks, dup_blocks, dup_block_bytes;   // 256 KiB blocks aligned to the file's start
    uint64_t dup_files, dup_file_bytes;             // files identical to an earlier one in path order
    uint64_t bytes_read, ns;
} mp_dedup_result;

// files: source indices of the packable files, in path order.
int mp_dedup_run(mp_source *src, const uint32_t *files, size_t nfiles, unsigned threads,
                 mp_progress *prog, mp_dedup_result *out, mp_err *e);

#endif
