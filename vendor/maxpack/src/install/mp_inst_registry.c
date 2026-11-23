// SPDX-License-Identifier: GPL-3.0-or-later
// Registro de lo instalado con MaxPack: <internal>/maxpack/installed/<TITLE_ID>.json.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_inst_internal.h"
#include "mp_json.h"
#include "mp_platform.h"

static void put_str(mp_jbuf *b, const char *key, const char *v, int first) {
    mp_jbuf_raw(b, first ? "{" : ",");
    mp_jbuf_str(b, key);
    mp_jbuf_raw(b, ":");
    mp_jbuf_str(b, v);
}

static void put_int(mp_jbuf *b, const char *key, int64_t v) {
    mp_jbuf_raw(b, ",");
    mp_jbuf_str(b, key);
    mp_jbuf_raw(b, ":");
    mp_jbuf_i64(b, v);
}

int mp_inst_record_save(const mp_inst_env *env, const mp_inst_record *r, mp_err *e) {
    char path[1200];
    if (mpi_record_path(env, r->title_id, path, sizeof path) != 0) {
        mp_err_set(e, "TITLE_ID no válido: %s", r->title_id);
        return -1;
    }
    char aid[33], rsha[65];
    mpi_hex(r->archive_id, 16, aid);
    mpi_hex(r->runtime_sha, 32, rsha);
    mp_jbuf b = {0};
    put_str(&b, "title_id", r->title_id, 1);
    put_str(&b, "version", r->version, 0);
    put_str(&b, "name", r->name, 0);
    put_str(&b, "dir", r->dir, 0);
    put_str(&b, "p5z", r->p5z, 0);
    put_str(&b, "archive_id", aid, 0);
    put_str(&b, "runtime_sha", rsha, 0);
    put_int(&b, "kind", r->kind);
    put_int(&b, "packed", r->packed);
    put_int(&b, "runtime_replaced", r->runtime_replaced);
    put_int(&b, "smp_pending", r->smp_pending);
    put_str(&b, "smp_why", r->smp_why, 0);
    put_int(&b, "installed_at", r->installed_at);
    mp_jbuf_raw(&b, "}\n");
    if (b.oom) {
        mp_jbuf_free(&b);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/maxpack/installed", env->internal);
    int rc = mp_make_dirs(dir, e);
    if (rc == 0) rc = mpi_write_replace(path, b.p, b.n, e);
    mp_jbuf_free(&b);
    return rc;
}

static void get_str(const mp_json *j, const char *key, char *dst, size_t cap) {
    const char *s = mp_json_str(mp_json_get(j, key));
    snprintf(dst, cap, "%s", s ? s : "");
}

static int get_int(const mp_json *j, const char *key) {
    int64_t v = 0;
    return mp_json_i64(mp_json_get(j, key), &v) == 0 && v != 0;
}

int mp_inst_record_load(const mp_inst_env *env, const char *title_id, mp_inst_record *r, mp_err *e) {
    memset(r, 0, sizeof *r);
    char path[1200];
    if (mpi_record_path(env, title_id, path, sizeof path) != 0) {
        mp_err_set(e, "TITLE_ID no válido: %s", title_id);
        return -1;
    }
    if (mp_path_kind(path) != 1) {
        mp_err_set(e, "no hay registro de %s", title_id);
        return -1;
    }
    char *buf = NULL;
    size_t n = 0;
    if (mpi_read_small(path, 1u << 20, &buf, &n, e) != 0) return -1;
    mp_json *j = mp_json_parse(buf, n, NULL);
    free(buf);
    const char *id = mp_json_str(mp_json_get(j, "title_id"));
    if (!j || !id || strcmp(id, title_id) != 0) {
        mp_json_free(j);
        mp_err_set(e, "registro dañado: %s", path);
        return -1;
    }
    get_str(j, "title_id", r->title_id, sizeof r->title_id);
    get_str(j, "version", r->version, sizeof r->version);
    get_str(j, "name", r->name, sizeof r->name);
    get_str(j, "dir", r->dir, sizeof r->dir);
    get_str(j, "p5z", r->p5z, sizeof r->p5z);
    get_str(j, "smp_why", r->smp_why, sizeof r->smp_why);
    mpi_unhex(mp_json_str(mp_json_get(j, "archive_id")), r->archive_id, 16);
    mpi_unhex(mp_json_str(mp_json_get(j, "runtime_sha")), r->runtime_sha, 32);
    int64_t kind = 0;
    mp_json_i64(mp_json_get(j, "kind"), &kind);
    r->kind = (int)kind;
    // Sin el campo (registros anteriores), un .p5z siempre lleva packs.
    r->packed = mp_json_get(j, "packed") ? get_int(j, "packed") : r->kind == MP_SRC_P5Z;
    r->runtime_replaced = get_int(j, "runtime_replaced");
    r->smp_pending = get_int(j, "smp_pending");
    mp_json_i64(mp_json_get(j, "installed_at"), &r->installed_at);
    mp_json_free(j);
    return 0;
}
