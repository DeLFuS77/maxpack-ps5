// SPDX-License-Identifier: GPL-3.0-or-later
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "mp_platform.h"
#include "mp_source_impl.h"

static int cmp_entry(const void *a, const void *b) {
    return strcmp(((const mp_entry *)a)->path, ((const mp_entry *)b)->path);
}

void mp_source_sort(mp_source *s) {
    if (s->n > 1) qsort(s->entries, s->n, sizeof *s->entries, cmp_entry);
}

int mp_source_open(const char *path, mp_source **out, mp_err *e) {
    *out = NULL;
    int kind = mp_path_kind(path);
    if (kind == 0) {
        mp_err_set(e, "no existe: %s", path);
        return -1;
    }
    if (kind == 2) return mp_dir_open(path, out, e);

    unsigned char h[16] = {0};
    size_t got = 0;
    mp_file *f = NULL;
    if (mp_file_open_read(path, &f, e) != 0) return -1;
    int rc = mp_file_pread(f, h, sizeof h, 0, &got, e);
    mp_file_close(f);
    if (rc != 0) return -1;
    if (got >= 11 && memcmp(h + 3, "EXFAT   ", 8) == 0) return mp_exfat_open(path, out, e);
    if (got >= 4 && memcmp(h, "\x7F" "FIH", 4) == 0) {
        mp_err_set(e, "%s: los PKG todavía no están soportados (llegarán en la fase 4); "
                      "usa una carpeta o una imagen .exfat", path);
        return -1;
    }
    mp_err_set(e, "%s: formato no reconocido: de momento se admite una carpeta o una imagen .exfat",
               path);
    return -1;
}

mp_source_kind mp_source_kind_of(const mp_source *s) { return s->kind; }
size_t mp_source_count(const mp_source *s) { return s->n; }
const mp_entry *mp_source_entry(const mp_source *s, size_t i) { return i < s->n ? &s->entries[i] : NULL; }
size_t mp_source_dir_count(const mp_source *s) { return s->ndirs; }

long mp_source_find(const mp_source *s, const char *path) {
    size_t lo = 0, hi = s->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(s->entries[mid].path, path);
        if (c == 0) return (long)mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

int mp_source_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e) {
    if (i >= s->n) {
        mp_err_set(e, "índice de archivo fuera de rango: %zu", i);
        return -1;
    }
    const mp_entry *en = &s->entries[i];
    if (off > en->size || len > en->size - off) {
        mp_err_set(e, "lectura fuera de %s (%" PRIu64 " + %zu > %" PRIu64 ")", en->path, off, len,
                   en->size);
        return -1;
    }
    if (len == 0) return 0;
    return s->read(s, i, off, buf, len, e);
}

void mp_source_close(mp_source *s) {
    if (!s) return;
    if (s->close) s->close(s);
    for (size_t i = 0; i < s->n; i++) free(s->entries[i].path);
    free(s->entries);
    free(s);
}
