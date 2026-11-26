// SPDX-License-Identifier: GPL-3.0-or-later
// Install a .p5z: temporary folder outside the loaders' paths, files verified
// with their SHA-256 and forced to disk, runtime ready, and renamed to the destination at the end.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mp_hash.h"
#include "mp_parallel.h"
#include "mp_idx3.h"
#include "mp_inst_internal.h"
#include "mp_p5z_internal.h"
#include "mp_platform.h"

#define RUNTIME_PATH "fakelib/libSceAmpr.sprx"
#define RUNTIME_MAX (64u << 20)

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

static int has_marker(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 8 <= n; i++)
        if (p[i] == 'A' && !memcmp(p + i, "AMPRZPK1", 8)) return 1;
    return 0;
}

typedef struct {
    uint8_t *buf;
    size_t n, cap;
} membuf;

static int to_mem(void *ctx, const void *p, size_t n, mp_err *e) {
    membuf *m = ctx;
    if (m->n + n > m->cap) {
        mp_err_set(e, "file larger than expected");
        return -1;
    }
    memcpy(m->buf + m->n, p, n);
    m->n += n;
    return 0;
}

static int read_to_mem(mp_p5z *p, size_t i, uint8_t **out, size_t *len, mp_err *e) {
    membuf m = {malloc(p->files[i].size ? (size_t)p->files[i].size : 1), 0, (size_t)p->files[i].size};
    if (!m.buf) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    if (p5z_read_file(p, i, to_mem, &m, e) != 0) {
        free(m.buf);
        return -1;
    }
    *out = m.buf;
    *len = m.n;
    return 0;
}

void mp_inst_result_free(mp_inst_result *r) {
    mp_inst_existing_free(r->existing, r->nexisting);
    r->existing = NULL;
    r->nexisting = 0;
}

static void set_result(mp_inst_result *r, int status, const char *code) {
    r->status = status;
    r->code = code;
}

static int dest_ok(const mp_inst_env *env, const char *root) {
    if (!root || mp_path_kind(root) != 2) return 0;
    if (!strcmp(root, env->internal)) return 1;
    const char *d[16];
    size_t n = mp_inst_drives(env, d, 16);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(root, d[i])) return 1;
    return 0;
}

// ---- parallel writing ----------------------------------------------------------------

typedef struct {
    mp_p5z *p;
    const mp_inst_env *env;
    const char *tmp;
    const size_t *todo;
    size_t ntodo, next;
    mp_mutex *mu;
    uint64_t written, total, base;   // base: what was already done before resuming
    int stop, fail, fail_io;
    mp_err err;
    mpi_state *st;
} job;

typedef struct {
    job *j;
    mp_wfile *w;
    int sink_failed;
} wctx;

static int sink_write(void *ctx, const void *p, size_t n, mp_err *e) {
    wctx *c = ctx;
    job *j = c->j;
    if (mp_wfile_write(c->w, p, n, e) != 0) {
        c->sink_failed = 1;
        return -1;
    }
    int rc = 0;
    mp_mutex_lock(j->mu);
    j->written += n;
    if (j->env->progress) j->env->progress(j->env->ctx, "copiando", j->base + j->written, j->total);
    if (j->stop || j->fail) rc = -1;
    else if (j->env->io_check) {
        int k = j->env->io_check(j->env->ctx, j->written, e);
        if (k == 1) {
            j->stop = 1;
            rc = -1;
        } else if (k != 0) {
            rc = -1;
        }
    }
    mp_mutex_unlock(j->mu);
    if (rc) c->sink_failed = 1;
    return rc;
}

static void fail_job(job *j, const mp_err *e, int io) {
    mp_mutex_lock(j->mu);
    if (!j->fail && !j->stop) {
        j->fail = 1;
        j->fail_io = io;
        j->err = *e;
    }
    mp_mutex_unlock(j->mu);
}

static void write_one(job *j, size_t i) {
    const p5z_file *f = &j->p->files[i];
    mp_err e = {{0}};
    char *path = mpi_join(j->tmp, f->path);
    wctx c = {j, NULL, 0};
    if (path) mp_remove_file(path);   // what a cut-off left half-done
    int rc = path ? mp_wfile_create(path, &c.w, &e) : -1;
    if (!path) mp_err_set(&e, "out of memory");
    int io = rc != 0;
    if (rc == 0) {
        rc = p5z_read_file(j->p, i, sink_write, &c, &e);
        io = c.sink_failed;
    }
    if (rc == 0) {
        rc = mp_wfile_sync(c.w, &e);
        io = 1;
    }
    if (c.w && mp_wfile_close(c.w, rc ? NULL : &e) != 0 && rc == 0) {
        rc = -1;
        io = 1;
    }
    if (rc == 0 && f->mtime && mp_set_mtime(path, f->mtime, &e) != 0) {
        rc = -1;
        io = 1;
    }
    free(path);
    if (rc == 0) {
        mp_mutex_lock(j->mu);
        if (j->st) rc = mpi_state_add(j->st, f->path, &e);
        mp_mutex_unlock(j->mu);
        io = 1;
    }
    if (rc) {
        mp_mutex_lock(j->mu);
        int stopped = j->stop;
        mp_mutex_unlock(j->mu);
        if (!stopped) fail_job(j, &e, io);
    }
}

static void worker(void *arg) {
    job *j = arg;
    for (;;) {
        mp_mutex_lock(j->mu);
        size_t k = j->next;
        int quit = j->stop || j->fail || k >= j->ntodo;
        if (!quit) j->next++;
        mp_mutex_unlock(j->mu);
        if (quit) return;
        write_one(j, j->todo[k]);
    }
}

// Creates the folders of all the files before distributing them among threads.
static int make_parents(const char *tmp, mp_p5z *p, const size_t *todo, size_t n, mp_err *e) {
    char last[1100] = "";
    for (size_t k = 0; k < n; k++) {
        const char *rel = p->files[todo[k]].path;
        const char *s = strrchr(rel, '/');
        if (!s) continue;
        char d[1100];
        int len = snprintf(d, sizeof d, "%s/%.*s", tmp, (int)(s - rel), rel);
        if (len < 0 || (size_t)len >= sizeof d) {
            mp_err_set(e, "path too long: %s", rel);
            return -1;
        }
        if (!strcmp(d, last)) continue;
        if (mp_make_dirs(d, e) != 0) return -1;
        snprintf(last, sizeof last, "%s", d);
    }
    return 0;
}

static int run_job(job *j, unsigned threads) {
    if (threads < 1) threads = 1;
    if (threads > 32) threads = 32;
    mp_thread *t[32];
    unsigned started = 0;
    for (unsigned k = 1; k < threads; k++)
        if (mp_thread_start(&t[started], worker, j) == 0) started++;
    worker(j);
    for (unsigned k = 0; k < started; k++) mp_thread_join(t[k]);
    return j->fail ? -1 : 0;
}

// ---- runtime --------------------------------------------------------------------------------

typedef struct {
    int replace;          // put the installer's
    size_t rt_index;      // the .p5z's runtime (SIZE_MAX if there is none)
    size_t idx3_index;    // the .p5z's ampr_emu.index (SIZE_MAX if there is none)
    int has_packs;
} rt_plan;

static int plan_runtime(const mp_inst_env *env, mp_p5z *p, rt_plan *rp, mp_err *e) {
    rp->replace = 0;
    rp->rt_index = rp->idx3_index = SIZE_MAX;
    rp->has_packs = 0;
    for (size_t i = 0; i < p->nfiles; i++) {
        const p5z_file *f = &p->files[i];
        if (f->role == MP_ROLE_ORIGINAL) continue;
        if (f->role == MP_ROLE_ZPK_INDEX) rp->has_packs = 1;
        if (f->role == MP_ROLE_IDX3) rp->idx3_index = i;
        if (f->role == MP_ROLE_RUNTIME || ieq(f->path, RUNTIME_PATH)) rp->rt_index = i;
    }
    if (!rp->has_packs) return 0;   // without packs, the runtime is not needed
    int zstd = 0;
    if (rp->rt_index != SIZE_MAX && p->files[rp->rt_index].size <= RUNTIME_MAX) {
        uint8_t *b = NULL;
        size_t n = 0;
        if (read_to_mem(p, rp->rt_index, &b, &n, e) != 0) return -1;
        zstd = has_marker(b, n);
        free(b);
    }
    if (zstd) return 0;
    if (!env->runtime.data || !env->runtime.len) {
        mp_err_set(e, "the .p5z has no runtime that reads zstd packs (ampr-emu-ng) and this installer "
                      "carries none: repack with --runtime");
        return 1;
    }
    if (rp->idx3_index == SIZE_MAX) {
        mp_err_set(e, "the .p5z has no ampr_emu.index");
        return -1;
    }
    rp->replace = 1;
    if (env->notify)
        env->notify(env->ctx, rp->rt_index == SIZE_MAX
                                  ? "The .p5z has no runtime: the installer's ampr-emu-ng is being put in"
                                  : "The .p5z's runtime does not read zstd packs: it is being replaced with the installer's ampr-emu-ng");
    return 0;
}

static int write_buffer(const char *dir, const char *rel, const uint8_t *p, size_t n, int64_t mtime, mp_err *e) {
    char *path = mpi_join(dir, rel);
    if (!path) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    char *s = strrchr(path, '/');
    *s = 0;
    int rc = mp_make_dirs(path, e);
    *s = '/';
    mp_wfile *w = NULL;
    mp_remove_file(path);
    if (rc == 0) rc = mp_wfile_create(path, &w, e);
    if (rc == 0) rc = mp_wfile_write(w, p, n, e);
    if (rc == 0) rc = mp_wfile_sync(w, e);
    if (w && mp_wfile_close(w, rc ? NULL : e) != 0) rc = -1;
    if (rc == 0 && mtime) rc = mp_set_mtime(path, mtime, e);
    free(path);
    return rc;
}

static int install_runtime(const mp_inst_env *env, mp_p5z *p, const rt_plan *rp, const char *tmp, mp_err *e) {
    uint8_t *idx = NULL, *fixed = NULL;
    size_t n = 0, fn = 0;
    if (read_to_mem(p, rp->idx3_index, &idx, &n, e) != 0) return -1;
    int rc = mp_idx3_set_row(idx, n, RUNTIME_PATH, env->runtime.len, env->runtime.mtime, &fixed, &fn, e);
    free(idx);
    if (rc == 0) rc = write_buffer(tmp, "ampr_emu.index", fixed, fn, p->files[rp->idx3_index].mtime, e);
    free(fixed);
    if (rc == 0) rc = write_buffer(tmp, RUNTIME_PATH, env->runtime.data, env->runtime.len, env->runtime.mtime, e);
    return rc;
}

// ---- resume: re-read what was marked as done ------------------------------------------------

typedef struct {
    mp_p5z *p;
    const char *tmp;
    const size_t *cand;   // indices in p->files
    uint8_t *ok;
} recheck_ctx;

static int recheck_one(void *ctx, size_t k, unsigned tid) {
    recheck_ctx *c = ctx;
    (void)tid;
    const p5z_file *f = &c->p->files[c->cand[k]];
    char *path = mpi_join(c->tmp, f->path);
    uint8_t sha[32];
    uint64_t size = 0;
    uint64_t skip = f->role == MP_ROLE_VOLUME ? MP_ZPK_VOL_HEADER : 0;
    c->ok[k] = path && mp_path_kind(path) == 1 && mpi_file_sha(path, skip, sha, &size, NULL) == 0 &&
               size == f->size && !memcmp(sha, f->sha, 32);
    free(path);
    return 0;
}

static const mp_p5z *sort_p5z;

static int cmp_by_path(const void *a, const void *b) {
    return strcmp(sort_p5z->files[*(const size_t *)a].path, sort_p5z->files[*(const size_t *)b].path);
}

static int recheck_done(const mp_inst_env *env, mp_p5z *p, const char *tmp, char **done, size_t ndone,
                        uint8_t *skip, uint64_t *base, mp_err *e) {
    size_t *byp = malloc(p->nfiles * sizeof *byp + 1), n = 0;
    size_t *cand = malloc(ndone * sizeof *cand + 1), nc = 0;
    uint8_t *ok = calloc(ndone + 1, 1);
    if (!byp || !cand || !ok) {
        free(byp);
        free(cand);
        free(ok);
        mp_err_set(e, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < p->nfiles; i++)
        if (p->files[i].role != MP_ROLE_ORIGINAL) byp[n++] = i;
    sort_p5z = p;   // only used here, in one thread
    qsort(byp, n, sizeof *byp, cmp_by_path);
    for (size_t d = 0; d < ndone; d++) {
        size_t lo = 0, hi = n;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            int c = strcmp(p->files[byp[mid]].path, done[d]);
            if (c == 0) {
                cand[nc++] = byp[mid];
                break;
            }
            if (c < 0) lo = mid + 1;
            else hi = mid;
        }
    }
    if (env->progress) env->progress(env->ctx, "comprobando", 0, nc);
    recheck_ctx c = {p, tmp, cand, ok};
    mp_progress prog;
    mp_progress_init(&prog, "comprobando", nc, 0);
    mp_parallel_run(env->threads ? env->threads : 1, nc, recheck_one, &c, &prog);
    mp_progress_free(&prog);
    for (size_t k = 0; k < nc; k++)
        if (ok[k]) {
            skip[cand[k]] = 1;
            *base += p->files[cand[k]].size;
        }
    free(byp);
    free(cand);
    free(ok);
    return 0;
}

// ---- put in place ---------------------------------------------------------------------------

static int same_path(const char *a, const char *b) { return !strcmp(a, b) || mp_same_file(a, b); }

int mpi_place(const mp_inst_env *env, const char *item, const char *final_path, const mp_inst_existing *ex,
              size_t nex, mp_err *e) {
    char dir[1200];
    snprintf(dir, sizeof dir, "%s", final_path);
    char *s = strrchr(dir, '/');
    if (s) *s = 0;
    if (mp_make_dirs(dir, e) != 0) return -1;
    char old[1300], mark[1320];
    snprintf(old, sizeof old, "%s.old", item);
    snprintf(mark, sizeof mark, "%s.dest", old);
    int moved_old = 0, rc = 0;
    if (mp_path_kind(final_path) != 0) {   // the destination is taken: only if confirmed
        int confirmed = 0;
        for (size_t i = 0; i < nex && !confirmed; i++) confirmed = same_path(ex[i].path, final_path);
        if (!confirmed) {
            mp_err_set(e, "%s appeared during the install and nobody confirmed replacing it: not touched", final_path);
            return -1;
        }
        mp_remove_tree(old, NULL);
        // If it is cut off here, mp_inst_recover puts the old one back in place.
        rc = mp_write_file(mark, final_path, strlen(final_path), e);
        if (rc == 0) rc = mp_rename(final_path, old, e);
        moved_old = rc == 0;
    }
    if (rc == 0) rc = mp_rename(item, final_path, e);
    if (rc != 0 && moved_old) mp_rename(old, final_path, NULL);   // what was there comes back
    if (rc == 0) {
        mp_err e2 = {{0}};
        if (moved_old && mp_remove_tree(old, &e2) != 0 && env->notify) env->notify(env->ctx, e2.msg);
        for (size_t i = 0; i < nex; i++) {
            if (same_path(ex[i].path, final_path)) continue;
            if (mp_remove_tree(ex[i].path, &e2) != 0 && env->notify) env->notify(env->ctx, e2.msg);
        }
    }
    mp_remove_file(mark);
    return rc;
}

// <x>.old.dest marks that a cut-off left in the temporary folders (written by mpi_place).
typedef struct {
    const char *root;
} rec_ctx;

static int rec_cb(void *ctx, const char *rel, int is_dir, uint64_t size, int64_t mtime) {
    rec_ctx *c = ctx;
    (void)size;
    (void)mtime;
    size_t n = strlen(rel);
    if (is_dir || n < 9 || strcmp(rel + n - 9, ".old.dest") != 0) return 0;
    char mark[1400], old[1400], *target = NULL;
    size_t tl = 0;
    snprintf(mark, sizeof mark, "%s/%s", c->root, rel);
    snprintf(old, sizeof old, "%.*s", (int)(strlen(mark) - 5), mark);
    if (mpi_read_small(mark, 4096, &target, &tl, NULL) == 0) {
        if (mp_path_kind(target) == 0 && mp_path_kind(old) != 0) {
            char parent[1200];
            snprintf(parent, sizeof parent, "%s", target);
            char *s = strrchr(parent, '/');
            if (s) *s = 0;
            mp_make_dirs(parent, NULL);
            mp_rename(old, target, NULL);
        } else if (mp_path_kind(target) != 0 && mp_path_kind(old) != 0) {
            mp_remove_tree(old, NULL);   // the new one is properly in place: the old one is surplus
        }
    }
    free(target);
    if (mp_path_kind(old) == 0) mp_remove_file(mark);
    return 0;
}

void mp_inst_recover(const mp_inst_env *env) {
    char root[1200];
    rec_ctx c = {root};
    snprintf(root, sizeof root, "%s/maxpack/tmp", env->internal);
    if (mp_path_kind(root) == 2) mp_dir_walk(root, rec_cb, &c, NULL);
    const char *d[16];
    size_t n = mp_inst_drives(env, d, 16);
    for (size_t i = 0; i < n; i++) {
        snprintf(root, sizeof root, "%s/maxpack-tmp", d[i]);
        if (mp_path_kind(root) == 2) mp_dir_walk(root, rec_cb, &c, NULL);
    }
}

// ---- plan: what would be done ------------------------------------------------------------------

void mp_inst_plan_free(mp_inst_plan *pl) {
    free(pl->f.path);
    mp_inst_existing_free(pl->ex, pl->nex);
    pl->f.path = NULL;
    pl->ex = NULL;
    pl->nex = 0;
}

static void drop_existing(mp_inst_plan *pl, size_t i) {
    free(pl->ex[i].path);
    pl->ex[i] = pl->ex[--pl->nex];
}

int mp_inst_plan_make(const mp_inst_env *env, const mp_inst_req *req, mp_inst_plan *pl, const char **code,
                      mp_err *e) {
    memset(pl, 0, sizeof *pl);
    *code = NULL;
    if (mp_inst_probe_any(req->p5z_path, &pl->f, e) != 0) {
        *code = mpi_has_ext(req->p5z_path, ".p5z") ? "bad_p5z" : "bad_source";
        return -1;
    }
    if (!dest_ok(env, req->dest_root)) {
        mp_err_set(e, "the destination drive is not available: %s", req->dest_root ? req->dest_root : "(none)");
        *code = "bad_dest";
        mp_inst_plan_free(pl);
        return -1;
    }
    int internal = !strcmp(req->dest_root, env->internal);
    const char *src_name = strrchr(pl->f.path, '/');
    src_name = src_name ? src_name + 1 : pl->f.path;
    if (pl->f.kind == MP_SRC_IMAGE)
        snprintf(pl->final_path, sizeof pl->final_path, "%s/homebrew/%s", req->dest_root, src_name);
    else
        snprintf(pl->final_path, sizeof pl->final_path, "%s/homebrew/%s", req->dest_root, pl->f.title_id);
    snprintf(pl->tmp_root, sizeof pl->tmp_root, internal ? "%s/maxpack/tmp" : "%s/maxpack-tmp", req->dest_root);
    snprintf(pl->tmp, sizeof pl->tmp, "%s/%s", pl->tmp_root, pl->f.title_id);

    // Copies that already exist. They never count: the source, what contains it, and what contains the destination
    // or the temporary folders (deleting it would take the new game or other games with it).
    if (mp_inst_find_existing(env, pl->f.title_id, req->known, req->nknown, &pl->ex, &pl->nex, e) != 0) {
        *code = "io_error";
        mp_inst_plan_free(pl);
        return -1;
    }
    for (size_t i = 0; i < pl->nex;) {
        const char *p = pl->ex[i].path;
        int is_final = same_path(p, pl->final_path);
        int bad = same_path(p, pl->f.path) || mp_path_is_inside(pl->f.path, p) ||
                  (!is_final && mp_path_is_inside(pl->final_path, p)) || mp_path_is_inside(pl->tmp_root, p);
        if (bad) drop_existing(pl, i);
        else i++;
    }
    // A taken destination always counts as a copy (even if it is not recognized as this game).
    int listed = 0;
    for (size_t i = 0; i < pl->nex; i++) listed |= same_path(pl->ex[i].path, pl->final_path);
    if (!listed && mp_path_kind(pl->final_path) != 0) {
        mp_inst_existing *nx = realloc(pl->ex, (pl->nex + 1) * sizeof *nx);
        char *dup = mp_strdup(pl->final_path);
        if (!nx || !dup) {
            if (nx) pl->ex = nx;
            free(dup);
            mp_err_set(e, "out of memory");
            *code = "io_error";
            mp_inst_plan_free(pl);
            return -1;
        }
        pl->ex = nx;
        memset(&pl->ex[pl->nex], 0, sizeof pl->ex[pl->nex]);
        pl->ex[pl->nex].path = dup;
        pl->ex[pl->nex].is_file = mp_path_kind(pl->final_path) == 1;
        pl->ex[pl->nex].size = mpi_tree_size(pl->final_path);
        pl->nex++;
    }
    for (size_t i = 0; i < pl->nex; i++) {
        int ok = 0;
        for (size_t k = 0; k < req->nreplace && !ok; k++) ok = same_path(pl->ex[i].path, req->replace[k]);
        if (!ok) {
            pl->status = MP_INST_NEEDS_CONFIRM;
            return 0;
        }
    }
    // Space: what this game's temporary folder already takes up at this destination is reused or deleted.
    uint64_t avail = 0;
    int frc = env->free_space ? env->free_space(env->ctx, req->dest_root, &avail)
                              : mp_disk_free(req->dest_root, &avail, e);
    if (frc != 0) {
        *code = "io_error";
        mp_inst_plan_free(pl);
        return -1;
    }
    pl->need = pl->f.install_size + env->space_margin;
    pl->avail = avail + (mp_path_kind(pl->tmp) != 0 ? mpi_tree_size(pl->tmp) : 0);
    pl->status = pl->need > pl->avail ? MP_INST_NO_SPACE : MP_INST_DONE;
    return 0;
}

// ---- install --------------------------------------------------------------------------------

int mp_inst_install(const mp_inst_env *env, const mp_inst_req *req, mp_inst_result *r, mp_err *e) {
    memset(r, 0, sizeof *r);
    mp_inst_plan pl;
    const char *code = NULL;
    if (mp_inst_plan_make(env, req, &pl, &code, e) != 0) {
        set_result(r, MP_INST_FAILED, code);
        return -1;
    }
    snprintf(r->title_id, sizeof r->title_id, "%s", pl.f.title_id);
    snprintf(r->final_dir, sizeof r->final_dir, "%s", pl.final_path);
    r->need = pl.need;
    r->avail = pl.avail;
    if (pl.status == MP_INST_NEEDS_CONFIRM) {
        free(pl.f.path);
        r->existing = pl.ex;
        r->nexisting = pl.nex;
        set_result(r, MP_INST_NEEDS_CONFIRM, "needs_confirm");
        return 0;
    }
    if (pl.status == MP_INST_NO_SPACE) {
        mp_inst_plan_free(&pl);
        set_result(r, MP_INST_NO_SPACE, "no_space");
        return 0;
    }
    // Confirmed copies (ex) are deleted only at the end, with the new one already verified in place.
    mp_inst_found f = pl.f;
    mp_inst_existing *ex = pl.ex;
    size_t nex = pl.nex;
    char *final_dir = pl.final_path, *tmp = pl.tmp;

    if (f.kind != MP_SRC_P5Z) {   // as is (spec 5.4 bis)
        int crc = mpi_copy_install(env, req, &f, tmp, final_dir, ex, nex, r, e);
        free(f.path);
        mp_inst_existing_free(ex, nex);
        return crc;
    }

    mp_p5z *p = NULL;
    if (mp_p5z_open(req->p5z_path, &p, e) != 0) {
        free(f.path);
        mp_inst_existing_free(ex, nex);
        set_result(r, MP_INST_FAILED, "bad_p5z");
        return -1;
    }
    // a .p5z that grew has its packs as only ampr-emu-ng's p5z mode reads them: as a folder they would not mount
    if (mp_p5z_version(p) == 2) {
        mp_p5z_close(p);
        free(f.path);
        mp_inst_existing_free(ex, nex);
        mp_err_set(e, "this .p5z grew (DLC or an update were added to it): play it from the .p5z, or make a compact "
                      "copy of it on the PC (maxpack compact) to install it");
        set_result(r, MP_INST_FAILED, "grown_p5z");
        return -1;
    }
    rt_plan rp;
    int prc = plan_runtime(env, p, &rp, e);
    if (prc != 0) {
        mp_p5z_close(p);
        free(f.path);
        mp_inst_existing_free(ex, nex);
        set_result(r, MP_INST_FAILED, prc == 1 ? "no_runtime" : "bad_p5z");
        return -1;
    }

    // State: resume what belongs to the same .p5z or start from scratch.
    size_t *todo = malloc((p->nfiles + 1) * sizeof *todo), ntodo = 0;
    uint8_t *skip = calloc(p->nfiles + 1, 1);
    int rc = todo && skip ? 0 : -1;
    if (rc) mp_err_set(e, "out of memory");
    mpi_state *st = NULL;
    int resumed = 0;
    char **done = NULL;
    size_t ndone = 0;
    uint64_t base = 0;
    if (rc == 0)
        rc = mpi_state_begin(env, f.title_id, f.archive_id, req->p5z_path, req->dest_root, tmp, &resumed, &done,
                             &ndone, &st, e);
    if (rc == 0) rc = mp_make_dirs(tmp, e);
    if (rc == 0 && resumed && ndone) rc = recheck_done(env, p, tmp, done, ndone, skip, &base, e);
    mpi_done_free(done, ndone);
    for (size_t i = 0; rc == 0 && i < p->nfiles; i++) {
        const p5z_file *pf = &p->files[i];
        if (pf->role == MP_ROLE_ORIGINAL || skip[i]) continue;
        if (rp.replace && (i == rp.rt_index || i == rp.idx3_index)) continue;
        todo[ntodo++] = i;
    }
    free(skip);
    if (rc == 0) rc = make_parents(tmp, p, todo, ntodo, e);
    const char *fail_code = "io_error";
    int stopped = 0;
    if (rc == 0) {
        job j;
        memset(&j, 0, sizeof j);
        j.p = p;
        j.env = env;
        j.tmp = tmp;
        j.todo = todo;
        j.ntodo = ntodo;
        j.total = f.install_size;
        j.base = base;
        j.st = st;
        j.mu = mp_mutex_new();
        if (!j.mu) {
            mp_err_set(e, "out of memory");
            rc = -1;
        } else {
            rc = run_job(&j, env->threads);
            stopped = j.stop;
            if (rc) {
                *e = j.err;
                if (!j.fail_io) fail_code = "bad_p5z";
            }
            mp_mutex_free(j.mu);
        }
    }
    if (rc == 0 && !stopped && rp.replace) rc = install_runtime(env, p, &rp, tmp, e);
    // If the .p5z is deleted afterwards, this folder will be the only copy: it is re-read in full.
    if (rc == 0 && !stopped && req->delete_p5z) {
        if (env->progress) env->progress(env->ctx, "comprobando", 0, f.install_size);
        long bad = mp_inst_check_dir(p, tmp, rp.replace, env->threads, e);
        if (bad != 0) {
            if (bad > 0)
                mp_err_set(e, "the copy does not match the .p5z in %ld files: the .p5z is not deleted", bad);
            rc = -1;
            fail_code = "io_error";
        }
    }
    mp_inst_record rec;
    memset(&rec, 0, sizeof rec);
    if (rc == 0 && !stopped) {
        if (rp.replace) {
            mp_sha256 s;
            mp_sha256_init(&s);
            mp_sha256_update(&s, env->runtime.data, env->runtime.len);
            mp_sha256_final(&s, rec.runtime_sha);
        } else if (rp.rt_index != SIZE_MAX) {
            memcpy(rec.runtime_sha, p->files[rp.rt_index].sha, 32);
        }
    }
    mp_p5z_close(p);
    free(todo);
    mpi_state_close(st);
    if (rc == 0 && stopped) {
        free(f.path);
        mp_inst_existing_free(ex, nex);
        set_result(r, MP_INST_STOPPED, "stopped");
        return 0;
    }
    if (rc != 0) {
        free(f.path);
        mp_inst_existing_free(ex, nex);
        set_result(r, MP_INST_FAILED, fail_code);
        return -1;
    }

    // To the destination, and registry.
    if (env->progress) env->progress(env->ctx, "terminando", f.install_size, f.install_size);
    char hb[1100];
    snprintf(hb, sizeof hb, "%s/homebrew", req->dest_root);
    rc = mp_make_dirs(hb, e);
    if (rc == 0) rc = mpi_place(env, tmp, final_dir, ex, nex, e);
    if (rc == 0) {
        snprintf(rec.title_id, sizeof rec.title_id, "%s", f.title_id);
        snprintf(rec.version, sizeof rec.version, "%s", f.version);
        snprintf(rec.name, sizeof rec.name, "%s", f.name);
        snprintf(rec.dir, sizeof rec.dir, "%s", final_dir);
        snprintf(rec.p5z, sizeof rec.p5z, "%s", req->p5z_path);
        memcpy(rec.archive_id, f.archive_id, 16);
        rec.kind = MP_SRC_P5Z;
        rec.packed = 1;
        rec.runtime_replaced = rp.replace;
        rec.smp_pending = 1;
        rec.installed_at = (int64_t)time(NULL);
        rc = mp_inst_record_save(env, &rec, e);
    }
    if (rc == 0) mpi_state_remove(env, f.title_id);
    if (rc == 0 && req->delete_p5z && mp_remove_file(req->p5z_path) != 0 && env->notify)
        env->notify(env->ctx, "Could not delete the .p5z");
    r->runtime_replaced = rp.replace;
    if (rc != 0) {
        free(f.path);
        mp_inst_existing_free(ex, nex);
        set_result(r, MP_INST_FAILED, "io_error");
        return -1;
    }
    if (env->notify) {
        char msg[300];
        snprintf(msg, sizeof msg, "%s installed", f.name[0] ? f.name : f.title_id);
        env->notify(env->ctx, msg);
    }
    free(f.path);
    mp_inst_existing_free(ex, nex);
    set_result(r, MP_INST_DONE, "done");
    return 0;
}
