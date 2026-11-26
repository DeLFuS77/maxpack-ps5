// SPDX-License-Identifier: GPL-3.0-or-later
// Contiguous runs of a file inside an image.
#ifndef MP_EXTENT_H
#define MP_EXTENT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t file_off, img_off, len;
} mp_extent;   // sorted by file_off and without gaps

// Finds the run that contains file_off. Returns its index (and in *img_off the position in the
// image, in *run the bytes left in that run) or -1 if file_off falls outside.
long mp_extent_find(const mp_extent *ex, size_t n, uint64_t file_off, uint64_t *img_off,
                    uint64_t *run);

#endif
