// SPDX-License-Identifier: GPL-3.0-or-later
// Tramos contiguos de un archivo dentro de una imagen.
#ifndef MP_EXTENT_H
#define MP_EXTENT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t file_off, img_off, len;
} mp_extent;   // ordenados por file_off y sin huecos

// Busca el tramo que contiene file_off. Devuelve su índice (y en *img_off la posición en la
// imagen, en *run los bytes que quedan en ese tramo) o -1 si file_off cae fuera.
long mp_extent_find(const mp_extent *ex, size_t n, uint64_t file_off, uint64_t *img_off,
                    uint64_t *run);

#endif
