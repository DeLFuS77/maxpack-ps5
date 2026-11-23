// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_extent.h"

long mp_extent_find(const mp_extent *ex, size_t n, uint64_t file_off, uint64_t *img_off,
                    uint64_t *run) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ex[mid].file_off + ex[mid].len <= file_off)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == n || file_off < ex[lo].file_off) return -1;
    uint64_t d = file_off - ex[lo].file_off;
    *img_off = ex[lo].img_off + d;
    *run = ex[lo].len - d;
    return (long)lo;
}
