// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic sample of 1 MiB windows over the packable files.
#ifndef MP_SAMPLE_H
#define MP_SAMPLE_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

#define MP_WINDOW ((uint32_t)1 << 20)   // aligned to offset 0 of each file

typedef struct {
    uint32_t file;     // index in the packable list
    uint64_t off;
    uint32_t len;
    uint64_t ordinal;  // global index among all the windows, in file order
} mp_window;

typedef struct {
    mp_window *w;
    size_t n;
    uint64_t sample_bytes, total_bytes;
    double rate;
} mp_sample;

uint64_t mp_splitmix64(uint64_t x);
// sizes: sizes of the packable files in path order. Target = everything if full; otherwise,
// min(P, max(ceil(fraction·P), min_bytes)).
int mp_sample_select(const uint64_t *sizes, size_t nfiles, double fraction, uint64_t min_bytes,
                     int full, mp_sample *out, mp_err *e);
// Same, with windows of `window` bytes (e.g. 32 MiB to measure transport). The windows
// come out in mp_window.len as uint32_t, so window <= 2^31.
int mp_sample_select_ex(const uint64_t *sizes, size_t nfiles, uint32_t window, double fraction,
                        uint64_t min_bytes, int full, mp_sample *out, mp_err *e);
void mp_sample_free(mp_sample *s);

#endif
