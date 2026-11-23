// SPDX-License-Identifier: GPL-3.0-or-later
// Experimentos del formato v2: agrupar archivos pequeños (M5) y diccionario entrenado (M6).
#ifndef MP_V2EXP_H
#define MP_V2EXP_H

#include <stddef.h>
#include <stdint.h>

#include "mp_measure.h"
#include "mp_parallel.h"
#include "mp_sample.h"
#include "mp_source.h"

#define MP_SMALL_MAX 65535u          // "archivos < 64 KiB"
#define MP_DICT_SIZE (110u * 1024u)

// Validación cruzada: dict[p] se entrena solo con unidades de paridad p (y se aplica a las de
// paridad 1 - p). Si ZDICT no puede entrenar (pocos datos), dict[p] queda en NULL: no es error.
// Bloques: unidades = bloques de 256 KiB de las ventanas de la muestra; paridad = ordinal & 1;
// como mucho 16 MiB por paridad, repartidos por toda la muestra.
int mp_dict_train_blocks(mp_source *src, const uint32_t *packable, const mp_sample *s,
                         mp_dict_pair *out, mp_err *e);
// Pequeños: unidades = archivos enteros; paridad = posición en `small` & 1; 8 MiB por paridad.
int mp_dict_train_small(mp_source *src, const uint32_t *small, size_t n, mp_dict_pair *out,
                        mp_err *e);
void mp_dict_pair_free(mp_dict_pair *p);

typedef struct {
    uint64_t files, raw;
    uint64_t individual;        // un frame zstd 19 por archivo (lo que hace v1)
    uint64_t grouped;           // concatenados en orden de ruta y cortados en bloques de 256 KiB
    uint64_t individual_dict;   // un frame por archivo con dict[1 - paridad]
    int dicts;                  // diccionarios disponibles (0-2)
} mp_small_result;

// small: índices de origen de los archivos pequeños empaquetables, en orden de ruta.
int mp_small_run(mp_source *src, const uint32_t *small, size_t n, const mp_dict_pair *dicts,
                 unsigned threads, mp_progress *prog, mp_small_result *out, mp_err *e);

#endif
