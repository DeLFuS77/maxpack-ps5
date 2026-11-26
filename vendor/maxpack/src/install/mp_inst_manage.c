// SPDX-License-Identifier: GPL-3.0-or-later
// What was installed with MaxPack: list, Verify and Uninstall.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_inst_internal.h"
#include "mp_json.h"
#include "mp_p5z.h"
#include "mp_platform.h"

typedef struct {
    char (*ids)[16];
    size_t n, cap;
    int oom;
} id_list;

static int id_cb(void *ctx, const char *name, int is_dir, uint64_t size) {
    id_list *l = ctx;
    (void)size;
    size_t len = strlen(name);
    if (is_dir || len != 14 || strcmp(name + 9, ".json") != 0) return 0;   // <TITLE_ID>.json
    if (l->n == l->cap) {
        size_t cap = l->cap ? 2 * l->cap : 16;
        char (*v)[16] = realloc(l->ids, cap * sizeof *v);
        if (!v) {
            l->oom = 1;
            return -1;
        }
        l->ids = v;
        l->cap = cap;
    }
    memcpy(l->ids[l->n], name, 9);
    l->ids[l->n][9] = 0;
    if (mp_inst_title_ok(l->ids[l->n])) l->n++;
    return 0;
}

static int cmp_id(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int mp_inst_list(const mp_inst_env *env, mp_inst_record **out, size_t *n, mp_err *e) {
    *out = NULL;
    *n = 0;
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/maxpack/installed", env->internal);
    if (mp_path_kind(dir) != 2) return 0;
    id_list l = {0};
    if (mp_dir_list(dir, id_cb, &l, e) != 0 && l.oom) {
        free(l.ids);
        mp_err_set(e, "out of memory");
        return -1;
    }
    if (l.n > 1) qsort(l.ids, l.n, sizeof *l.ids, cmp_id);
    mp_inst_record *v = calloc(l.n + 1, sizeof *v);
    if (!v) {
        free(l.ids);
        mp_err_set(e, "out of memory");
        return -1;
    }
    size_t k = 0;
    for (size_t i = 0; i < l.n; i++) {
        mp_err le = {{0}};
        if (mp_inst_record_load(env, l.ids[i], &v[k], &le) == 0) {
            k++;
        } else if (env->notify) {
            char msg[700];
            snprintf(msg, sizeof msg, "Skipping a damaged record (%s.json): %s", l.ids[i], le.msg);
            env->notify(env->ctx, msg);
        }
    }
    free(l.ids);
    *out = v;
    *n = k;
    return 0;
}

// What was installed as is: each file against the SHA-256 saved when it was copied.
static int verify_copy(const mp_inst_env *env, const mp_inst_record *rec, mp_inst_verify_result *r, mp_err *e) {
    char path[1200], *buf = NULL;
    size_t n = 0;
    snprintf(path, sizeof path, "%s/maxpack/installed/%s.files.json", env->internal, rec->title_id);
    if (mpi_read_small(path, (size_t)1 << 30, &buf, &n, e) != 0) return -1;
    mp_json *j = mp_json_parse(buf, n, NULL);
    free(buf);
    const mp_json *files = mp_json_get(j, "files");
    if (!j || !files) {
        mp_json_free(j);
        mp_err_set(e, "damaged file list: %s", path);
        return -1;
    }
    for (size_t i = 0; i < mp_json_len(files); i++) {
        const mp_json *it = mp_json_at(files, i);
        const char *rel = mp_json_str(mp_json_get(it, "path"));
        uint8_t want[32], got[32];
        uint64_t size = 0, wsize = 0;
        mp_json_u64(mp_json_get(it, "size"), &wsize);
        int ok = rel && mpi_unhex(mp_json_str(mp_json_get(it, "sha")), want, 32) == 0;
        char *p = ok ? (rec->kind == MP_SRC_IMAGE ? mp_strdup(rec->dir) : mpi_join(rec->dir, rel)) : NULL;
        ok = p && mpi_file_sha(p, 0, got, &size, NULL) == 0 && size == wsize && !memcmp(got, want, 32);
        free(p);
        if (ok) r->files_ok++;
        else r->files_bad++;
    }
    mp_json_free(j);
    return 0;
}

int mp_inst_verify(const mp_inst_env *env, const char *title_id, mp_inst_verify_result *r, mp_err *e) {
    memset(r, 0, sizeof *r);
    mp_inst_record rec;
    if (mp_inst_record_load(env, title_id, &rec, e) != 0) return -1;
    if (rec.kind != MP_SRC_P5Z) return verify_copy(env, &rec, r, e);
    if (mp_path_kind(rec.dir) != 2) {
        mp_err_set(e, "the game's folder is missing: %s", rec.dir);
        return -1;
    }
    mp_verify_result vr;
    if (mp_verify_folder(rec.dir, NULL, env->threads ? env->threads : 1, 0, &vr, e) != 0) return -1;
    r->files_ok = vr.files_ok;
    r->files_bad = vr.files_bad;
    mp_inst_found f;
    if (rec.p5z[0] && mp_path_kind(rec.p5z) == 1 && mp_inst_probe(rec.p5z, &f, NULL) == 0) {
        int same = !memcmp(f.archive_id, rec.archive_id, 16);
        free(f.path);
        mp_p5z *p = NULL;
        if (same && mp_p5z_open(rec.p5z, &p, NULL) == 0) {
            long bad = mp_inst_check_dir(p, rec.dir, rec.runtime_replaced, env->threads, e);
            mp_p5z_close(p);
            if (bad < 0) return -1;
            r->files_bad += (uint64_t)bad;
            r->p5z_checked = 1;
        }
    }
    if (rec.runtime_replaced) {   // the runtime the installer put in
        char *rt = mpi_join(rec.dir, "fakelib/libSceAmpr.sprx");
        uint8_t sha[32];
        int ok = rt && mpi_file_sha(rt, 0, sha, NULL, NULL) == 0 && !memcmp(sha, rec.runtime_sha, 32);
        free(rt);
        if (ok) r->files_ok++;
        else r->files_bad++;
    }
    return 0;
}

int mp_inst_uninstall(const mp_inst_env *env, const char *title_id, mp_err *e) {
    mp_inst_record rec;
    if (!mp_inst_title_ok(title_id)) {
        mp_err_set(e, "invalid TITLE_ID: %s", title_id);
        return -1;
    }
    if (mp_inst_record_load(env, title_id, &rec, e) != 0) {
        mp_err_set(e, "%s is not installed with MaxPack: not touched", title_id);
        return -1;
    }
    if (mp_remove_tree(rec.dir, e) != 0) return -1;
    char path[1200];
    snprintf(path, sizeof path, "%s/maxpack/installed/%s.files.json", env->internal, title_id);
    mp_remove_file(path);
    mpi_record_path(env, title_id, path, sizeof path);
    if (mp_remove_file(path) != 0) {
        mp_err_set(e, "could not delete the record %s", path);
        return -1;
    }
    return 0;
}
