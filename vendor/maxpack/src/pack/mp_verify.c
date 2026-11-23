// SPDX-License-Identifier: GPL-3.0-or-later
// verify: comprueba .p5z y carpetas empaquetadas archivo a archivo, en paralelo.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_idx3.h"
#include "mp_p5z_internal.h"

#define PIECE (4u << 20)
#define MAX_LISTED 20

enum { CHK_STORED, CHK_PACKED_P5Z, CHK_PACKED_DIR, CHK_LOOSE_DIR };

typedef struct {
    int kind;
    size_t a;          // índice en files (stored), en el manifiesto o en el zpk
    size_t src;        // índice en el origen (--against), o SIZE_MAX
    const char *path;  // ruta para los mensajes
    int bad;
    char why[256];
} check;

typedef struct {
    mp_p5z *p;
    mp_zpk *z;
    mp_source *src;     // --against
    const char *dir;    // carpeta verificada
    check *c;
    mp_progress *prog;
} vctx;

static int null_sink(void *ctx, const void *p, size_t n, mp_err *e) {
    (void)p;
    if (mp_cancel_check(e) != 0) return -1;
    mp_progress_add(ctx, n);
    return 0;
}

static int sha_source(mp_source *s, size_t i, uint8_t d[32], uint8_t *buf, mp_err *e) {
    mp_sha256 h;
    mp_sha256_init(&h);
    uint64_t size = mp_source_entry(s, i)->size;
    for (uint64_t off = 0; off < size; off += PIECE) {
        size_t k = size - off < PIECE ? (size_t)(size - off) : PIECE;
        if (mp_cancel_check(e) != 0 || mp_source_read(s, i, off, buf, k, e) != 0) return -1;
        mp_sha256_update(&h, buf, k);
    }
    mp_sha256_final(&h, d);
    return 0;
}

static int sha_zpk(mp_zpk *z, size_t i, uint8_t d[32], uint8_t *buf, mp_progress *prog, mp_err *e) {
    mp_sha256 h;
    mp_sha256_init(&h);
    uint64_t size = mp_zpk_size(z, i);
    for (uint64_t off = 0; off < size; off += PIECE) {
        size_t k = size - off < PIECE ? (size_t)(size - off) : PIECE;
        if (mp_cancel_check(e) != 0 || mp_zpk_read_file(z, i, off, buf, k, e) != 0) return -1;
        mp_sha256_update(&h, buf, k);
        mp_progress_add(prog, k);
    }
    mp_sha256_final(&h, d);
    return 0;
}

static int sha_path(const char *path, uint8_t d[32], uint8_t *buf, mp_err *e) {
    mp_file *f = NULL;
    uint64_t size = 0;
    if (mp_file_open_read(path, &f, e) != 0 || mp_file_size(f, &size, e) != 0) {
        mp_file_close(f);
        return -1;
    }
    mp_sha256 h;
    mp_sha256_init(&h);
    int rc = 0;
    for (uint64_t off = 0; off < size && !rc; off += PIECE) {
        size_t k = size - off < PIECE ? (size_t)(size - off) : PIECE, got = 0;
        rc = mp_cancel_check(e);
        if (!rc) rc = mp_file_pread(f, buf, k, off, &got, e);
        if (!rc && got != k) {
            mp_err_set(e, "lectura incompleta");
            rc = -1;
        }
        if (!rc) mp_sha256_update(&h, buf, k);
    }
    mp_file_close(f);
    mp_sha256_final(&h, d);
    return rc;
}

static int check_work(void *vc, size_t i, unsigned tid) {
    vctx *v = vc;
    (void)tid;
    check *c = &v->c[i];
    if (mp_cancelled()) return 0;   // la cancelación se informa al acabar run_checks
    mp_err e = {{0}};
    uint8_t got[32], want[32];
    uint8_t *buf = malloc(PIECE);
    int rc = buf ? 0 : -1;
    if (!buf) mp_err_set(&e, "sin memoria");
    if (!rc && c->kind == CHK_STORED) {
        rc = p5z_read_file(v->p, c->a, null_sink, v->prog, &e);   // ya comprueba el SHA-256
    } else if (!rc && c->kind == CHK_PACKED_P5Z) {
        const mp_manifest_entry *m = &v->p->man.v[c->a];
        long found = mp_zpk_find(v->z, m->path);
        size_t zi = found < 0 ? mp_zpk_count(v->z) : (size_t)found;
        if (zi == mp_zpk_count(v->z)) {
            mp_err_set(&e, "no está en los packs");
            rc = -1;
        } else if ((rc = sha_zpk(v->z, zi, got, buf, v->prog, &e)) == 0 && memcmp(got, m->sha, 32)) {
            mp_err_set(&e, "el SHA-256 no coincide con el del dump original");
            rc = -1;
        }
    } else if (!rc && c->kind == CHK_PACKED_DIR) {
        rc = sha_zpk(v->z, c->a, got, buf, v->prog, &e);
        if (!rc && c->src != SIZE_MAX) {
            rc = sha_source(v->src, c->src, want, buf, &e);
            if (!rc && memcmp(got, want, 32)) {
                mp_err_set(&e, "distinto del origen");
                rc = -1;
            }
        }
    } else if (!rc && c->kind == CHK_LOOSE_DIR) {
        size_t a = strlen(v->dir), b = strlen(c->path);
        char *p = malloc(a + b + 2);
        if (p) {
            memcpy(p, v->dir, a);
            p[a] = '/';
            memcpy(p + a + 1, c->path, b + 1);
        }
        rc = p ? sha_path(p, got, buf, &e) : -1;
        free(p);
        if (!rc) rc = sha_source(v->src, c->src, want, buf, &e);
        if (!rc && memcmp(got, want, 32)) {
            mp_err_set(&e, "distinto del origen");
            rc = -1;
        }
        mp_progress_add(v->prog, mp_source_entry(v->src, c->src)->size);
    }
    if (rc) {
        c->bad = 1;
        snprintf(c->why, sizeof c->why, "%s", e.msg[0] ? e.msg : "error");
    }
    mp_progress_maybe_print(v->prog);
    free(buf);
    return 0;   // un fallo no detiene la verificación del resto
}

static void report(const check *c, size_t n, mp_verify_result *r) {
    size_t listed = 0;
    for (size_t i = 0; i < n; i++) {
        if (!c[i].bad) {
            r->files_ok++;
            continue;
        }
        r->files_bad++;
        size_t pl = strlen(c[i].path);
        const char *why = c[i].why;   // los mensajes de los lectores ya empiezan por la ruta
        if (!strncmp(why, c[i].path, pl) && why[pl] == ':') why += pl + (why[pl + 1] == ' ' ? 2 : 1);
        if (listed++ < MAX_LISTED) fprintf(stderr, "FALLO: %s: %s\n", c[i].path, why);
    }
    if (listed > MAX_LISTED) fprintf(stderr, "... y %zu fallos más\n", listed - MAX_LISTED);
}

static int run_checks(vctx *v, size_t n, unsigned threads, int progress, uint64_t total, mp_verify_result *r,
                      mp_err *e) {
    mp_progress prog;
    mp_progress_init(&prog, "Verificando", total, progress);
    v->prog = &prog;
    mp_parallel_run(threads ? threads : 1, n, check_work, v, NULL);
    if (mp_cancel_check(e) != 0) {
        mp_progress_free(&prog);
        return -1;
    }
    if (progress) fprintf(stderr, "\rVerificando: 100.0 %%\n");
    r->bytes_checked = prog.done;
    mp_progress_free(&prog);
    report(v->c, n, r);
    return 0;
}

int mp_verify_p5z(const char *path, unsigned threads, int progress, mp_verify_result *r, mp_err *e) {
    memset(r, 0, sizeof *r);
    mp_p5z *p = NULL;
    if (mp_p5z_open(path, &p, e) != 0) return -1;
    vctx v = {p, NULL, NULL, NULL, NULL, NULL};
    size_t n = 0, cap = p->nfiles + p->man.n + 1;
    v.c = calloc(cap, sizeof *v.c);
    uint64_t total = 0;
    int has_packs = 0;
    for (size_t i = 0; i < p->man.n; i++) has_packs |= (p->man.v[i].flags & MP_MANIFEST_PACKED) != 0;
    if (!v.c) {
        mp_p5z_close(p);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    mp_err ze = {{0}};
    int packs_ok = !has_packs || mp_p5z_zpk(p, &v.z, &ze) == 0;
    for (size_t i = 0; i < p->nfiles; i++) {
        v.c[n].kind = CHK_STORED;
        v.c[n].a = i;
        v.c[n].path = p->files[i].path;
        total += p->files[i].size;
        n++;
    }
    if (!packs_ok) {   // los packs no se pueden ni abrir: un único fallo que lo explica
        v.c[n].bad = 1;
        v.c[n].path = "ampr_zstd.index";
        snprintf(v.c[n].why, sizeof v.c[n].why, "%s", ze.msg);
        n++;
    } else {
        for (size_t i = 0; i < p->man.n; i++)
            if (p->man.v[i].flags & MP_MANIFEST_PACKED) {
                v.c[n].kind = CHK_PACKED_P5Z;
                v.c[n].a = i;
                v.c[n].path = p->man.v[i].path;
                total += p->man.v[i].size;
                n++;
            }
    }
    size_t todo = packs_ok ? n : n - 1;
    if (run_checks(&v, todo, threads, progress, total, r, e) != 0) {
        free(v.c);
        mp_zpk_close(v.z);
        mp_p5z_close(p);
        return -1;
    }
    if (!packs_ok) report(&v.c[n - 1], 1, r);
    free(v.c);
    mp_zpk_close(v.z);
    mp_p5z_close(p);
    return 0;
}

static int ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

int mp_verify_folder(const char *dir, const char *against, unsigned threads, int progress,
                     mp_verify_result *r, mp_err *e) {
    memset(r, 0, sizeof *r);
    size_t a = strlen(dir);
    char *ipath = malloc(a + 32), *xpath = malloc(a + 32);
    if (!ipath || !xpath) {
        free(ipath);
        free(xpath);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    snprintf(ipath, a + 32, "%s/ampr_zstd.index", dir);
    snprintf(xpath, a + 32, "%s/ampr_emu.index", dir);
    vctx v = {NULL, NULL, NULL, dir, NULL, NULL};
    mp_zpk_volsrc vs = {0};
    mp_idx3_entry *rows = NULL;
    size_t nrows = 0, n = 0;
    uint8_t *zidx = NULL, *xidx = NULL;
    int rc = -1;
    // Índices: los dos tienen que existir y ser válidos.
    mp_file *f = NULL;
    uint64_t zl = 0, xl = 0;
    size_t got = 0;
    if (mp_file_open_read(ipath, &f, e) != 0 || mp_file_size(f, &zl, e) != 0 || !(zidx = malloc((size_t)zl + 1)) ||
        mp_file_pread(f, zidx, (size_t)zl, 0, &got, e) != 0) {
        if (!e->msg[0]) mp_err_set(e, "no se puede leer %s", ipath);
        mp_file_close(f);
        goto done;
    }
    mp_file_close(f);
    f = NULL;
    if (mp_zpk_volsrc_dir(dir, &vs, e) != 0 || mp_zpk_open(zidx, (size_t)zl, &vs, &v.z, e) != 0) goto done;
    if (mp_file_open_read(xpath, &f, e) != 0 || mp_file_size(f, &xl, e) != 0 || !(xidx = malloc((size_t)xl + 1)) ||
        mp_file_pread(f, xidx, (size_t)xl, 0, &got, e) != 0 || mp_idx3_parse(xidx, (size_t)xl, &rows, &nrows, e) != 0) {
        mp_file_close(f);
        goto done;
    }
    mp_file_close(f);
    if (against && mp_source_open(against, &v.src, e) != 0) goto done;
    size_t nsrc = v.src ? mp_source_count(v.src) : 0;
    v.c = calloc(mp_zpk_count(v.z) + nsrc + 1, sizeof *v.c);
    if (!v.c) {
        mp_err_set(e, "sin memoria");
        goto done;
    }
    uint64_t total = 0;
    // Cada empaquetado: en ampr_emu.index con el mismo tamaño (si no, el runtime no monta nada).
    for (size_t i = 0; i < mp_zpk_count(v.z); i++) {
        check *c = &v.c[n++];
        c->kind = CHK_PACKED_DIR;
        c->a = i;
        c->src = SIZE_MAX;
        c->path = mp_zpk_path(v.z, i);
        total += mp_zpk_size(v.z, i);
        long row = mp_idx3_find(rows, nrows, c->path);   // filas ordenadas: búsqueda binaria
        int in_idx3 = row >= 0 && rows[row].size == mp_zpk_size(v.z, i);
        if (!in_idx3) {
            c->bad = 1;
            snprintf(c->why, sizeof c->why, "no está en ampr_emu.index con el mismo tamaño");
        }
        if (v.src) {
            long si = mp_source_find(v.src, c->path);
            if (si >= 0) c->src = (size_t)si;
        }
        if (v.src && c->src == SIZE_MAX && !c->bad) {
            c->bad = 1;
            snprintf(c->why, sizeof c->why, "no está en el origen");
        }
    }
    // Con --against, cada archivo del origen que no esté empaquetado tiene que estar suelto igual.
    for (size_t s = 0; s < nsrc; s++) {
        const char *p = mp_source_entry(v.src, s)->path;
        if (mp_zpk_find(v.z, p) >= 0) continue;   // empaquetado: ya se comprueba arriba
        if (ieq(p, "ampr_emu.index")) continue;   // se regenera siempre
        size_t pl = strlen(p);
        char *full = malloc(a + pl + 2);
        if (full) {
            memcpy(full, dir, a);
            full[a] = '/';
            memcpy(full + a + 1, p, pl + 1);
        }
        int exists = full && mp_path_kind(full) == 1;
        free(full);
        check *c = &v.c[n++];
        c->kind = CHK_LOOSE_DIR;
        c->src = s;
        c->path = p;
        if (!exists) {
            c->bad = 1;
            snprintf(c->why, sizeof c->why, "falta en la carpeta");
        } else if (ieq(p, "fakelib/libSceAmpr.sprx")) {
            n--;   // puede haberlo reemplazado --runtime: no cuenta como fallo
            continue;
        }
        total += mp_source_entry(v.src, s)->size;
    }
    // Se comprueban solo los que no han fallado ya por la estructura.
    check *todo = malloc((n ? n : 1) * sizeof *todo);
    size_t nt = 0;
    if (!todo) {
        mp_err_set(e, "sin memoria");
        goto done;
    }
    for (size_t i = 0; i < n; i++)
        if (!v.c[i].bad) todo[nt++] = v.c[i];
    check *all = v.c;
    v.c = todo;
    int crc = run_checks(&v, nt, threads, progress, total, r, e);
    v.c = all;
    if (crc != 0) {
        free(todo);
        goto done;   // rc sigue en -1
    }
    for (size_t i = 0; i < n; i++)
        if (all[i].bad) report(&all[i], 1, r);
    free(todo);
    rc = 0;
done:
    free(v.c);
    mp_zpk_close(v.z);
    mp_zpk_volsrc_free(&vs);
    mp_idx3_entries_free(rows, nrows);
    mp_source_close(v.src);
    free(zidx);
    free(xidx);
    free(ipath);
    free(xpath);
    return rc;
}
