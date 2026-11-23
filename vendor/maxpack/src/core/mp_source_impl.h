// SPDX-License-Identifier: GPL-3.0-or-later
// Interior de mp_source: lo comparten los distintos tipos de origen.
#ifndef MP_SOURCE_IMPL_H
#define MP_SOURCE_IMPL_H

#include "mp_source.h"

struct mp_source {
    mp_source_kind kind;
    mp_entry *entries;
    size_t n, ndirs;
    int (*read)(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e);
    void (*close)(mp_source *s);   // libera impl
    void *impl;
};

int mp_dir_open(const char *path, mp_source **out, mp_err *e);
int mp_exfat_open(const char *path, mp_source **out, mp_err *e);

// Ordena las entradas por strcmp de la ruta.
void mp_source_sort(mp_source *s);

#endif
