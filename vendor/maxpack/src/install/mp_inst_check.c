// SPDX-License-Identifier: GPL-3.0-or-later
// Releer una carpeta instalada y compararla con el SHA-256 de cada archivo del .p5z.
#include <stdlib.h>
#include <string.h>

#include "mp_inst_internal.h"
#include "mp_p5z_internal.h"
#include "mp_parallel.h"

typedef struct {
    mp_p5z *p;
    const char *dir;
    const size_t *idx;
    uint8_t *bad;
} check_ctx;

static int check_one(void *ctx, size_t k, unsigned tid) {
    check_ctx *c = ctx;
    (void)tid;
    const p5z_file *f = &c->p->files[c->idx[k]];
    char *path = mpi_join(c->dir, f->path);
    uint8_t sha[32];
    uint64_t size = 0;
    uint64_t skip = f->role == MP_ROLE_VOLUME ? MP_ZPK_VOL_HEADER : 0;
    int ok = path && mp_path_kind(path) == 1 && mpi_file_sha(path, skip, sha, &size, NULL) == 0 &&
             size == f->size && !memcmp(sha, f->sha, 32);
    c->bad[k] = !ok;
    free(path);
    return 0;
}

static int is_runtime_file(const p5z_file *f) {
    if (f->role == MP_ROLE_IDX3 || f->role == MP_ROLE_RUNTIME) return 1;
    const char *rt = "fakelib/libsceampr.sprx";
    size_t i = 0;
    for (; f->path[i] && rt[i]; i++) {
        char c = f->path[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != rt[i]) return 0;
    }
    return !f->path[i] && !rt[i];
}

long mp_inst_check_dir(mp_p5z *p, const char *dir, int skip_runtime, unsigned threads, mp_err *e) {
    size_t *idx = malloc((p->nfiles + 1) * sizeof *idx), n = 0;
    uint8_t *bad = calloc(p->nfiles + 1, 1);
    if (!idx || !bad) {
        free(idx);
        free(bad);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    for (size_t i = 0; i < p->nfiles; i++) {
        const p5z_file *f = &p->files[i];
        if (f->role == MP_ROLE_ORIGINAL || (skip_runtime && is_runtime_file(f))) continue;
        idx[n++] = i;
    }
    check_ctx c = {p, dir, idx, bad};
    mp_progress prog;
    mp_progress_init(&prog, "comprobando", n, 0);
    mp_parallel_run(threads ? threads : 1, n, check_one, &c, &prog);
    mp_progress_free(&prog);
    long nbad = 0;
    for (size_t k = 0; k < n; k++) nbad += bad[k];
    free(idx);
    free(bad);
    return nbad;
}
