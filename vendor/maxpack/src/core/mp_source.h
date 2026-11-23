// SPDX-License-Identifier: GPL-3.0-or-later
// Origen de un juego: una carpeta o una imagen exFAT, vistas como una lista de archivos.
#ifndef MP_SOURCE_H
#define MP_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

typedef struct {
    char *path;      // UTF-8, relativa, con '/', sin barra inicial
    uint64_t size;
    int64_t mtime;   // segundos Unix
} mp_entry;

typedef enum { MP_SRC_DIR = 1, MP_SRC_EXFAT = 2 } mp_source_kind;

typedef struct mp_source mp_source;

// Detecta el tipo: carpeta, imagen exFAT o (para rechazarlo con un mensaje claro) PKG.
int mp_source_open(const char *utf8_path, mp_source **out, mp_err *e);
mp_source_kind mp_source_kind_of(const mp_source *s);
size_t mp_source_count(const mp_source *s);
// Ordenadas por strcmp de los bytes de la ruta.
const mp_entry *mp_source_entry(const mp_source *s, size_t i);
size_t mp_source_dir_count(const mp_source *s);
// Índice de la entrada con esa ruta exacta (búsqueda binaria; las entradas van ordenadas), o -1.
long mp_source_find(const mp_source *s, const char *path);
// Lee exactamente len bytes; off + len debe ser <= size. Segura entre hilos.
int mp_source_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e);
void mp_source_close(mp_source *s);

#endif
