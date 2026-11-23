// SPDX-License-Identifier: GPL-3.0-or-later
// Qué archivos deben quedar sueltos en la consola y cuáles se pueden empaquetar (diseño 6.3).
#ifndef MP_RULES_H
#define MP_RULES_H

#include <stdint.h>

#include "mp_err.h"

typedef enum { MP_PACKABLE = 0, MP_LOOSE = 1 } mp_class;

typedef struct mp_rules mp_rules;

mp_rules *mp_rules_new(void);
// Añade un patrón --keep-loose. Error si está vacío o contiene '\'.
int mp_rules_add_keep_loose(mp_rules *r, const char *glob, mp_err *e);
mp_class mp_rules_classify(const mp_rules *r, const char *rel_path, uint64_t size);
void mp_rules_free(mp_rules *r);

// '*' y '?' dentro de un segmento, '**' = cualquier número de segmentos (también cero).
// Sin distinguir mayúsculas en ASCII. Devuelve 1 si casa.
int mp_glob_match(const char *glob, const char *path);

#endif
