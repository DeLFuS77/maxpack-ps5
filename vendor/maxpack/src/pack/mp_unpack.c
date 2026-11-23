// SPDX-License-Identifier: GPL-3.0-or-later
// unpack: carpeta para la consola (--folder) o dump original bit a bit (--original).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_p5z_internal.h"

#define PIECE (4u << 20)

static int prepare_out(const char *dir, mp_err *e) {
    int kind = mp_path_kind(dir);
    if (kind == 1) {
        mp_err_set(e, "la salida es un archivo: %s", dir);
        return -1;
    }
    if (kind == 2 && mp_dir_is_empty(dir) != 1) {
        mp_err_set(e, "la carpeta de salida no está vacía: %s", dir);
        return -1;
    }
    return kind == 0 ? mp_make_dirs(dir, e) : 0;
}

static char *out_path(const char *dir, const char *rel, mp_err *e) {
    size_t a = strlen(dir), b = strlen(rel);
    char *p = malloc(a + b + 2);
    if (!p) {
        mp_err_set(e, "sin memoria");
        return NULL;
    }
    memcpy(p, dir, a);
    p[a] = '/';
    memcpy(p + a + 1, rel, b + 1);
    char *slash = strrchr(p, '/');
    if (slash > p + a) {
        *slash = 0;
        int rc = mp_make_dirs(p, e);
        *slash = '/';
        if (rc != 0) {
            free(p);
            return NULL;
        }
    }
    return p;
}

static int to_wfile(void *ctx, const void *p, size_t n, mp_err *e) {
    if (mp_cancel_check(e) != 0) return -1;
    return mp_wfile_write(ctx, p, n, e);
}

static int write_stored(mp_p5z *p, size_t fi, const char *dir, const char *rel, int64_t mtime,
                        mp_progress *prog, mp_err *e) {
    char *path = out_path(dir, rel, e);
    mp_wfile *w = NULL;
    if (!path) return -1;
    int rc = mp_wfile_create(path, &w, e);
    if (rc == 0) rc = p5z_read_file(p, fi, to_wfile, w, e);
    if (w && mp_wfile_close(w, e) != 0) rc = -1;
    if (rc == 0 && mtime) rc = mp_set_mtime(path, mtime, e);
    mp_progress_add(prog, p->files[fi].size);
    mp_progress_maybe_print(prog);
    free(path);
    return rc;
}

int mp_unpack_folder(mp_p5z *p, const char *out_dir, unsigned threads, int progress, mp_err *e) {
    (void)threads;
    if (prepare_out(out_dir, e) != 0) return -1;
    uint64_t total = 0;
    for (size_t i = 0; i < p->nfiles; i++)
        if (p->files[i].role != MP_ROLE_ORIGINAL) total += p->files[i].size;
    mp_progress prog;
    mp_progress_init(&prog, "Extrayendo la carpeta", total, progress);
    int rc = 0;
    for (size_t i = 0; i < p->nfiles && !rc; i++)
        if (p->files[i].role != MP_ROLE_ORIGINAL)
            rc = write_stored(p, i, out_dir, p->files[i].path, p->files[i].mtime, &prog, e);
    if (!rc) rc = mp_cancel_check(e);
    if (!rc && progress) fprintf(stderr, "\rExtrayendo la carpeta: 100.0 %%\n");
    mp_progress_free(&prog);
    return rc;
}

int mp_unpack_original(mp_p5z *p, const char *out_dir, unsigned threads, int progress, mp_err *e) {
    (void)threads;
    if (prepare_out(out_dir, e) != 0) return -1;
    mp_zpk *z = NULL;
    int has_packs = 0;
    for (size_t i = 0; i < p->man.n; i++) has_packs |= (p->man.v[i].flags & MP_MANIFEST_PACKED) != 0;
    if (has_packs && mp_p5z_zpk(p, &z, e) != 0) return -1;
    uint64_t total = 0;
    for (size_t i = 0; i < p->man.n; i++) total += p->man.v[i].size;
    mp_progress prog;
    mp_progress_init(&prog, "Reconstruyendo el original", total, progress);
    uint8_t *buf = malloc(PIECE);
    int rc = buf ? 0 : -1;
    if (!buf) mp_err_set(e, "sin memoria");
    size_t zi = 0;   // el manifiesto y los packs van en orden distinto: búsqueda por ruta
    for (size_t i = 0; i < p->man.n && !rc; i++) {
        const mp_manifest_entry *m = &p->man.v[i];
        if (mp_cancel_check(e) != 0) {
            rc = -1;
            break;
        }
        if (!(m->flags & MP_MANIFEST_PACKED)) {
            rc = write_stored(p, p->man_file[i], out_dir, m->path, m->mtime, &prog, e);
            continue;
        }
        long found = mp_zpk_find(z, m->path);   // búsqueda binaria: O(log n) por archivo
        zi = found < 0 ? mp_zpk_count(z) : (size_t)found;
        if (zi == mp_zpk_count(z) || mp_zpk_size(z, zi) != m->size) {
            mp_err_set(e, "%s no está en los packs del .p5z", m->path);
            rc = -1;
            break;
        }
        char *path = out_path(out_dir, m->path, e);
        mp_wfile *w = NULL;
        if (!path || mp_wfile_create(path, &w, e) != 0) {
            free(path);
            rc = -1;
            break;
        }
        mp_sha256 s;
        mp_sha256_init(&s);
        for (uint64_t off = 0; !rc && off < m->size; off += PIECE) {
            size_t k = m->size - off < PIECE ? (size_t)(m->size - off) : PIECE;
            rc = mp_cancel_check(e);
            if (!rc) rc = mp_zpk_read_file(z, zi, off, buf, k, e);
            if (!rc) rc = mp_wfile_write(w, buf, k, e);
            if (!rc) mp_sha256_update(&s, buf, k);
            mp_progress_add(&prog, k);
            mp_progress_maybe_print(&prog);
        }
        if (mp_wfile_close(w, e) != 0) rc = -1;
        uint8_t d[32];
        mp_sha256_final(&s, d);
        if (!rc && memcmp(d, m->sha, 32) != 0) {
            mp_err_set(e, "%s: el SHA-256 no coincide con el del dump original", m->path);
            rc = -1;
        }
        if (!rc) rc = mp_set_mtime(path, m->mtime, e);
        free(path);
    }
    if (!rc) rc = mp_cancel_check(e);
    if (!rc && progress) fprintf(stderr, "\rReconstruyendo el original: 100.0 %%\n");
    mp_progress_free(&prog);
    free(buf);
    mp_zpk_close(z);
    return rc;
}
