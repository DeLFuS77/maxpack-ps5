// SPDX-License-Identifier: GPL-3.0-or-later
// ampr_emu.index (AMPRIDX3): la lista de todos los archivos del juego que usa el runtime para dar
// un id a cada ruta. Formato idéntico a write_index de build_ampr_index.py (drakmor/ampr_emu).
#ifndef MP_IDX3_H
#define MP_IDX3_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

typedef struct {
    const char *path;   // relativa al juego, sin "/app0/"
    uint64_t size;
    int64_t mtime;
} mp_idx3_row;

// Valida las rutas y que no haya dos iguales salvo mayúsculas, y construye el archivo (malloc).
int mp_idx3_build(const mp_idx3_row *rows, size_t n, uint8_t **out, size_t *out_len, mp_err *e);

typedef struct {
    char *app0_path;   // "/app0/..."
    uint64_t size;
    int64_t mtime;
} mp_idx3_entry;

// Comprueba el archivo como Index::check de ampr-emu-ng y devuelve sus filas.
int mp_idx3_parse(const uint8_t *d, size_t n, mp_idx3_entry **out, size_t *count, mp_err *e);
void mp_idx3_entries_free(mp_idx3_entry *v, size_t n);
// Fila de la ruta relativa rel ("/app0/" + rel, sin distinguir mayúsculas), o -1. Las filas de un
// índice válido van ordenadas por esa clave (mp_idx3_parse lo comprueba).
long mp_idx3_find(const mp_idx3_entry *v, size_t n, const char *rel);
// Copia del índice (malloc) con la fila 'rel' cambiada, o añadida, a size/mtime. Valida como
// mp_idx3_parse. Es lo que hace pack --runtime con la fila de fakelib/libSceAmpr.sprx.
int mp_idx3_set_row(const uint8_t *idx, size_t n, const char *rel, uint64_t size, int64_t mtime,
                    uint8_t **out, size_t *out_len, mp_err *e);

// Valida una ruta relativa para los dos índices (AMPRZPK1 y AMPRIDX3). 0 si vale.
int mp_pack_path_check(const char *rel, mp_err *e);

#endif
