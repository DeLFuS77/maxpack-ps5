// SPDX-License-Identifier: GPL-3.0-or-later
// Descripción de un juego (carpeta, imagen .exfat o .p5z) para una interfaz: ID, versión, nombre,
// tamaños, sus packs y si lleva un runtime que lea zstd. Es lo que muestra `maxpack info --json`.
#ifndef MP_DESCRIBE_H
#define MP_DESCRIBE_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

typedef struct {
    char kind[8];   // "p5z", "folder", "exfat"
    char title_id[16], version[16], name[128];
    uint64_t original_size;   // suma de los archivos del dump original
    uint64_t install_size;    // lo que ocupa al instalarlo (.p5z: lo que deja unpack --folder)
    uint64_t p5z_size;        // tamaño del .p5z (0 si no lo es)
    uint64_t files;           // archivos del dump original
    uint64_t packed_files;    // de ellos, los que van en los packs zstd (0 si no está empaquetado)
    uint64_t volumes;         // volúmenes de los packs, ampr_zstd-NNN.pak (0 si no está empaquetado)
    int has_runtime, has_zstd_runtime, packed;   // packed: ya trae ampr_zstd.index
} mp_describe;

// 0 si pudo describirlo; -1 con el motivo en e.
int mp_describe_path(const char *path, mp_describe *d, mp_err *e);
// sce_sys/icon0.png: 0 con los bytes en *png (reservados con malloc; los libera quien llama);
// -1 si no tiene icono o no se pudo leer (e dice por qué).
int mp_describe_icon(const char *path, uint8_t **png, size_t *len, mp_err *e);

#endif
