// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_inst_internal.h"
#include "mp_json.h"
#include "mp_param.h"
#include "mp_platform.h"

char *mpi_join(const char *a, const char *b) {
    size_t x = strlen(a), y = strlen(b);
    char *p = malloc(x + y + 2);
    if (!p) return NULL;
    memcpy(p, a, x);
    p[x] = '/';
    memcpy(p + x + 1, b, y + 1);
    return p;
}

int mpi_read_small(const char *path, size_t max, char **out, size_t *len, mp_err *e) {
    *out = NULL;
    *len = 0;
    mp_file *f = NULL;
    uint64_t size = 0;
    if (mp_file_open_read(path, &f, e) != 0) return -1;
    if (mp_file_size(f, &size, e) != 0) {
        mp_file_close(f);
        return -1;
    }
    if (size > max) {
        mp_file_close(f);
        mp_err_set(e, "%s es demasiado grande", path);
        return -1;
    }
    char *b = malloc((size_t)size + 1);
    size_t got = 0;
    int rc = b ? mp_file_pread(f, b, (size_t)size, 0, &got, e) : -1;
    mp_file_close(f);
    if (!b) mp_err_set(e, "sin memoria");
    if (rc == 0 && got != size) {
        mp_err_set(e, "no se pudo leer entero %s", path);
        rc = -1;
    }
    if (rc) {
        free(b);
        return -1;
    }
    b[size] = 0;
    *out = b;
    *len = (size_t)size;
    return 0;
}

int mpi_write_replace(const char *path, const void *p, size_t n, mp_err *e) {
    char *tmp = malloc(strlen(path) + 5);
    if (!tmp) {
        mp_err_set(e, "sin memoria");
        return -1;
    }
    sprintf(tmp, "%s.tmp", path);
    mp_remove_file(tmp);
    mp_wfile *w = NULL;
    int rc = mp_wfile_create(tmp, &w, e);
    if (rc == 0) rc = mp_wfile_write(w, p, n, e);
    if (rc == 0) rc = mp_wfile_sync(w, e);
    if (w && mp_wfile_close(w, rc ? NULL : e) != 0) rc = -1;
    if (rc == 0) {
        mp_remove_file(path);
        rc = mp_rename(tmp, path, e);
    }
    if (rc) mp_remove_file(tmp);
    free(tmp);
    return rc;
}

void mpi_hex(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int mpi_unhex(const char *s, uint8_t *b, size_t n) {
    if (!s || strlen(s) != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) {
        int h = hexval(s[2 * i]), l = hexval(s[2 * i + 1]);
        if (h < 0 || l < 0) return -1;
        b[i] = (uint8_t)(h << 4 | l);
    }
    return 0;
}

int mpi_param_parse(const char *json, size_t n, char id[16], char ver[16], char name[128]) {
    return mp_param_read(json, n, id, ver, name);
}

void mpi_dir_title(const char *dir, char id[16]) {
    id[0] = 0;
    char *p = mpi_join(dir, "sce_sys/param.json"), *buf = NULL;
    size_t n = 0;
    char ver[16], name[128];
    if (p && mp_path_kind(p) == 1 && mpi_read_small(p, 1u << 20, &buf, &n, NULL) == 0)
        mpi_param_parse(buf, n, id, ver, name);
    free(buf);
    free(p);
}

static int size_cb(void *ctx, const char *rel, int is_dir, uint64_t size, int64_t mtime) {
    (void)rel;
    (void)mtime;
    if (!is_dir) *(uint64_t *)ctx += size;
    return 0;
}

uint64_t mpi_tree_size(const char *path) {
    uint64_t t = 0;
    int k = mp_path_kind(path);
    if (k == 1) mp_stat(path, &t, NULL);
    else if (k == 2) mp_dir_walk(path, size_cb, &t, NULL);
    return t;
}

int mpi_has_ext(const char *name, const char *ext) {
    size_t a = strlen(name), b = strlen(ext);
    if (a <= b) return 0;
    for (size_t i = 0; i < b; i++) {
        char c = name[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != ext[i]) return 0;
    }
    return 1;
}

int mpi_file_sha(const char *path, uint64_t skip, uint8_t sha[32], uint64_t *size, mp_err *e) {
    mp_file *f = NULL;
    uint64_t total = 0;
    if (mp_file_open_read(path, &f, e) != 0) return -1;
    if (mp_file_size(f, &total, e) != 0) {
        mp_file_close(f);
        return -1;
    }
    enum { PIECE = 4u << 20 };
    uint8_t *buf = malloc(PIECE);
    if (!buf) {
        mp_file_close(f);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    mp_sha256 s;
    mp_sha256_init(&s);
    int rc = 0;
    for (uint64_t off = skip < total ? skip : total; off < total && rc == 0;) {
        size_t k = total - off < PIECE ? (size_t)(total - off) : PIECE, got = 0;
        rc = mp_file_pread(f, buf, k, off, &got, e);
        if (rc == 0 && got != k) {
            mp_err_set(e, "%s: lectura incompleta", path);
            rc = -1;
        }
        if (rc == 0) mp_sha256_update(&s, buf, k);
        off += k;
    }
    free(buf);
    mp_file_close(f);
    if (rc == 0) {
        mp_sha256_final(&s, sha);
        if (size) *size = total;
    }
    return rc;
}

void mpi_tmp_path(const mp_inst_env *env, const char *dest_root, const char *title_id, char *out, size_t cap) {
    int internal = !strcmp(dest_root, env->internal);
    snprintf(out, cap, internal ? "%s/maxpack/tmp/%s" : "%s/maxpack-tmp/%s", dest_root, title_id);
}

static int same_path(const char *a, const char *b) { return !strcmp(a, b) || mp_same_file(a, b); }

static int reserved_under(const char *base, const char *path, int internal) {
    static const char *const subs[] = {"homebrew", "etaHEN", "etaHEN/games"};
    if (same_path(path, base)) return 1;
    for (size_t i = 0; i < sizeof subs / sizeof *subs; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", base, subs[i]);
        if (same_path(path, p)) return 1;
    }
    char m[1200];
    snprintf(m, sizeof m, internal ? "%s/maxpack" : "%s/maxpack-tmp", base);
    return same_path(path, m);
}

int mpi_reserved_path(const mp_inst_env *env, const char *path) {
    if (reserved_under(env->internal, path, 1)) return 1;
    const char *d[16];
    size_t n = mp_inst_drives(env, d, 16);
    for (size_t i = 0; i < n; i++)
        if (reserved_under(d[i], path, 0)) return 1;
    return 0;
}

int mpi_record_path(const mp_inst_env *env, const char *title_id, char *out, size_t cap) {
    if (!mp_inst_title_ok(title_id)) return -1;
    int k = snprintf(out, cap, "%s/maxpack/installed/%s.json", env->internal, title_id);
    return k > 0 && (size_t)k < cap ? 0 : -1;
}
