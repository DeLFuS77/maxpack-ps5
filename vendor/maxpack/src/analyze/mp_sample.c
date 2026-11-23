// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_sample.h"

#include <math.h>
#include <stdlib.h>

#define MP_SAMPLE_SEED 0x4D4158504B303030ULL   // "MAXPK000"

uint64_t mp_splitmix64(uint64_t x) {
    uint64_t z = x + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

int mp_sample_select(const uint64_t *sizes, size_t nfiles, double fraction, uint64_t min_bytes,
                     int full, mp_sample *out, mp_err *e) {
    return mp_sample_select_ex(sizes, nfiles, MP_WINDOW, fraction, min_bytes, full, out, e);
}

int mp_sample_select_ex(const uint64_t *sizes, size_t nfiles, uint32_t window, double fraction,
                        uint64_t min_bytes, int full, mp_sample *out, mp_err *e) {
    out->w = NULL;
    out->n = 0;
    out->sample_bytes = 0;
    uint64_t total = 0;
    for (size_t i = 0; i < nfiles; i++) total += sizes[i];
    out->total_bytes = total;
    double rate = 1.0;
    if (!full && total > 0) {
        double want = ceil(fraction * (double)total);
        if (want < (double)min_bytes) want = (double)min_bytes;
        if (want < (double)total) rate = want / (double)total;
    }
    out->rate = rate;
    // Umbral sobre 53 bits: la ventana k entra si (splitmix(k ^ semilla) >> 11) < rate · 2^53.
    uint64_t thr = (uint64_t)(rate * 9007199254740992.0);
    size_t cap = 0;
    uint64_t ordinal = 0;
    for (size_t f = 0; f < nfiles; f++) {
        for (uint64_t off = 0; off < sizes[f]; off += window, ordinal++) {
            if (rate < 1.0 && (mp_splitmix64(ordinal ^ MP_SAMPLE_SEED) >> 11) >= thr) continue;
            if (out->n == cap) {
                cap = cap ? cap * 2 : 1024;
                mp_window *w = realloc(out->w, cap * sizeof *w);
                if (!w) {
                    mp_sample_free(out);
                    mp_err_set(e, "sin memoria");
                    return -1;
                }
                out->w = w;
            }
            uint64_t left = sizes[f] - off;
            mp_window *w = &out->w[out->n++];
            w->file = (uint32_t)f;
            w->off = off;
            w->len = left < window ? (uint32_t)left : window;
            w->ordinal = ordinal;
            out->sample_bytes += w->len;
        }
    }
    return 0;
}

void mp_sample_free(mp_sample *s) {
    free(s->w);
    s->w = NULL;
    s->n = 0;
}
