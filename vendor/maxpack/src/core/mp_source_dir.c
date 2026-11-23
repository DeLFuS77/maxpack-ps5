// SPDX-License-Identifier: GPL-3.0-or-later
// Origen carpeta: cada lectura abre el archivo, lee de forma posicional y lo cierra.
#include <stdlib.h>
#include <string.h>

#include "mp_platform.h"
#include "mp_source_impl.h"

typedef struct {
    char *root;
} dir_impl;

typedef struct {
    mp_entry *v;
    size_t n, cap, ndirs;
} collect;

static int collect_cb(void *ctx, const char *rel, int is_dir, uint64_t size, int64_t mtime) {
    collect *c = ctx;
    if (is_dir) {
        c->ndirs++;
        return 0;
    }
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 256;
        mp_entry *v = realloc(c->v, cap * sizeof *v);
        if (!v) return -2;
        c->v = v;
        c->cap = cap;
    }
    char *p = mp_strdup(rel);
    if (!p) return -2;
    c->v[c->n].path = p;
    c->v[c->n].size = size;
    c->v[c->n].mtime = mtime;
    c->n++;
    return 0;
}

static char *full_path(const dir_impl *d, const char *rel) {
    size_t a = strlen(d->root), b = strlen(rel);
    char *p = malloc(a + b + 2);
    if (!p) return NULL;
    memcpy(p, d->root, a);
    p[a] = '/';
    memcpy(p + a + 1, rel, b + 1);
    return p;
}

static int dir_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e) {
    const dir_impl *d = s->impl;
    char *p = full_path(d, s->entries[i].path);
    if (!p) {
        mp_err_set(e, "sin memoria");
        return -1;
    }
    mp_file *f = NULL;
    int rc = mp_file_open_read(p, &f, e);
    if (rc == 0) {
        size_t got = 0;
        rc = mp_file_pread(f, buf, len, off, &got, e);
        if (rc == 0 && got != len) {
            mp_err_set(e, "lectura incompleta de %s (¿ha cambiado el archivo?)", s->entries[i].path);
            rc = -1;
        }
        mp_file_close(f);
    }
    free(p);
    return rc;
}

static void dir_close(mp_source *s) {
    dir_impl *d = s->impl;
    if (d) free(d->root);
    free(d);
}

int mp_dir_open(const char *path, mp_source **out, mp_err *e) {
    *out = NULL;
    collect c = {0};
    int rc = mp_dir_walk(path, collect_cb, &c, e);
    if (rc == -2) mp_err_set(e, "sin memoria");
    mp_source *s = calloc(1, sizeof *s);
    dir_impl *d = calloc(1, sizeof *d);
    if (rc == 0 && (!s || !d || !(d->root = mp_strdup(path)))) {
        mp_err_set(e, "sin memoria");
        rc = -1;
    }
    if (rc != 0) {
        for (size_t i = 0; i < c.n; i++) free(c.v[i].path);
        free(c.v);
        if (d) free(d->root);
        free(d);
        free(s);
        return -1;
    }
    s->kind = MP_SRC_DIR;
    s->entries = c.v;
    s->n = c.n;
    s->ndirs = c.ndirs;
    s->read = dir_read;
    s->close = dir_close;
    s->impl = d;
    mp_source_sort(s);
    *out = s;
    return 0;
}
