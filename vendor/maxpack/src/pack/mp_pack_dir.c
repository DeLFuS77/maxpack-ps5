// SPDX-License-Identifier: GPL-3.0-or-later
// Folder output: what gets copied to the console (replaced originals are not written).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_pack.h"
#include "mp_platform.h"

typedef struct {
    char *dir;
    mp_wfile *cur;
    char *cur_path;
    int64_t cur_mtime;
    int skip, made;
    mp_zpk_sink zs;   // the folder's volume sink (freed in destroy)
} dirout;

static char *join(const char *dir, const char *rel) {
    size_t a = strlen(dir), b = strlen(rel);
    char *p = malloc(a + b + 2);
    if (!p) return NULL;
    memcpy(p, dir, a);
    p[a] = '/';
    memcpy(p + a + 1, rel, b + 1);
    return p;
}

// The folder is created on the first write: if the source is rejected, nothing is left.
static int ensure_dir(dirout *d, mp_err *e) {
    if (d->made) return 0;
    if (mp_make_dirs(d->dir, e) != 0) return -1;
    d->made = 1;
    return 0;
}

static int dz_begin(void *ctx, uint32_t vol, mp_err *e) {
    dirout *d = ctx;
    if (ensure_dir(d, e) != 0) return -1;
    return d->zs.vol_begin(d->zs.ctx, vol, e);
}
static int dz_write(void *ctx, const void *p, size_t n, mp_err *e) {
    dirout *d = ctx;
    return d->zs.vol_write(d->zs.ctx, p, n, e);
}
static int dz_end(void *ctx, uint32_t vol, mp_err *e) {
    dirout *d = ctx;
    return d->zs.vol_end(d->zs.ctx, vol, e);
}
static int dz_header(void *ctx, uint32_t vol, const uint8_t hdr[64], mp_err *e) {
    dirout *d = ctx;
    return d->zs.vol_header(d->zs.ctx, vol, hdr, e);
}

static int d_begin(void *ctx, const char *rel, int role, uint64_t size, int64_t mtime, mp_err *e) {
    dirout *d = ctx;
    (void)size;
    if (role == MP_ROLE_ORIGINAL) {
        d->skip = 1;
        return 0;
    }
    if (ensure_dir(d, e) != 0) return -1;
    d->cur_path = join(d->dir, rel);
    if (!d->cur_path) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    char *slash = strrchr(d->cur_path, '/');
    if (slash && slash > d->cur_path + strlen(d->dir)) {
        *slash = 0;
        int rc = mp_make_dirs(d->cur_path, e);
        *slash = '/';
        if (rc != 0) return -1;
    }
    d->cur_mtime = mtime;
    return mp_wfile_create(d->cur_path, &d->cur, e);
}

static int d_write(void *ctx, const void *p, size_t n, mp_err *e) {
    dirout *d = ctx;
    return d->skip ? 0 : mp_wfile_write(d->cur, p, n, e);
}

static int d_end(void *ctx, const uint8_t sha[32], mp_err *e) {
    dirout *d = ctx;
    (void)sha;
    if (d->skip) {
        d->skip = 0;
        return 0;
    }
    int rc = mp_wfile_close(d->cur, e);
    d->cur = NULL;
    if (rc == 0) rc = mp_set_mtime(d->cur_path, d->cur_mtime, e);
    free(d->cur_path);
    d->cur_path = NULL;
    return rc;
}

static int d_finish(void *ctx, const mp_pack_manifest *m, mp_err *e) {
    (void)ctx;
    (void)m;
    (void)e;
    return 0;
}

static void d_destroy(void *ctx) {
    dirout *d = ctx;
    if (!d) return;
    if (d->cur) mp_wfile_close(d->cur, NULL);
    mp_zpk_sink_free(&d->zs);
    free(d->cur_path);
    free(d->dir);
    free(d);
}

int mp_pack_out_dir(const char *out_dir, mp_pack_out *out, mp_err *e) {
    memset(out, 0, sizeof *out);
    int kind = mp_path_kind(out_dir);
    if (kind == 1) {
        mp_err_set(e, "the output is a file, not a folder: %s", out_dir);
        return -1;
    }
    if (kind == 2 && mp_dir_is_empty(out_dir) != 1) {
        mp_err_set(e, "the output folder is not empty: %s", out_dir);
        return -1;
    }
    dirout *d = calloc(1, sizeof *d);
    if (!d || !(d->dir = mp_strdup(out_dir))) {
        free(d);
        mp_err_set(e, "out of memory");
        return -1;
    }
    d->made = kind == 2;
    if (mp_zpk_sink_dir(out_dir, &d->zs, e) != 0) {
        d_destroy(d);
        return -1;
    }
    out->zpk.vol_begin = dz_begin;
    out->zpk.vol_write = dz_write;
    out->zpk.vol_end = dz_end;
    out->zpk.vol_header = dz_header;
    out->zpk.ctx = d;
    out->file_begin = d_begin;
    out->file_write = d_write;
    out->file_end = d_end;
    out->finish = d_finish;
    out->destroy = d_destroy;
    out->ctx = d;
    return 0;
}
