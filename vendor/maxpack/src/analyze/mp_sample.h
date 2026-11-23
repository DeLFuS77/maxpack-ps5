// SPDX-License-Identifier: GPL-3.0-or-later
// Muestra determinista de ventanas de 1 MiB sobre los archivos empaquetables.
#ifndef MP_SAMPLE_H
#define MP_SAMPLE_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

#define MP_WINDOW ((uint32_t)1 << 20)   // alineada al desplazamiento 0 de cada archivo

typedef struct {
    uint32_t file;     // índice en la lista de empaquetables
    uint64_t off;
    uint32_t len;
    uint64_t ordinal;  // índice global entre todas las ventanas, en orden de archivo
} mp_window;

typedef struct {
    mp_window *w;
    size_t n;
    uint64_t sample_bytes, total_bytes;
    double rate;
} mp_sample;

uint64_t mp_splitmix64(uint64_t x);
// sizes: tamaños de los empaquetables en orden de ruta. Objetivo = todo si full; si no,
// min(P, max(ceil(fraction·P), min_bytes)).
int mp_sample_select(const uint64_t *sizes, size_t nfiles, double fraction, uint64_t min_bytes,
                     int full, mp_sample *out, mp_err *e);
// Igual, con ventanas de `window` bytes (p. ej. 32 MiB para medir el transporte). Las ventanas
// salen en mp_window.len como uint32_t, así que window <= 2^31.
int mp_sample_select_ex(const uint64_t *sizes, size_t nfiles, uint32_t window, double fraction,
                        uint64_t min_bytes, int full, mp_sample *out, mp_err *e);
void mp_sample_free(mp_sample *s);

#endif
