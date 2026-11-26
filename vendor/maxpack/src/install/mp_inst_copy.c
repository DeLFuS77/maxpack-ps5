// SPDX-License-Identifier: GPL-3.0-or-later
// Install a game folder or an image as is (spec 5.4 bis): byte-by-byte copy with each file's
// SHA-256 computed while reading it, fsync and the original date; resumable; a folder
// with zstd packs and no ampr-emu-ng receives the installer's runtime. An image is not touched.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mp_hash.h"
#include "mp_idx3.h"
#include "mp_inst_internal.h"
#include "mp_json.h"
#include "mp_parallel.h"
#include "mp_platform.h"

#define RUNTIME_PATH "fakelib/libSceAmpr.sprx"
#define RUNTIME_MAX (64u << 20)
#define PIECE (4u << 20)

typedef struct {
    char *rel;        // relative path at the destination
    char *src;        // full path at the source
    uint64_t size;
    int64_t mtime;
    uint8_t sha[32];
    int done;         // already there (resumed or copied)
    int skip;         // not copied: set by the installer (runtime)
} cfile;

typedef struct {
    cfile *v;
    size_t n, cap;
    const char *root;
    int bad_name;
} clist;

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

static int add_file(clist *l, const char *rel, const char *src, uint64_t size, int64_t mtime) {
    if (strpbrk(rel, "\t\n\r")) {
        l->bad_name = 1;
        return -1;
    }
    if (l->n == l->cap) {
        size_t cap = l->cap ? 2 * l->cap : 256;
        cfile *v = realloc(l->v, cap * sizeof *v);
        if (!v) return -1;
        l->v = v;
        l->cap = cap;
    }
    cfile *f = &l->v[l->n];
    memset(f, 0, sizeof *f);
    f->rel = mp_strdup(rel);
    f->src = src ? mp_strdup(src) : mpi_join(l->root, rel);
    if (!f->rel || !f->src) {
        free(f->rel);
        free(f->src);
        return -1;
    }
    f->size = size;
    f->mtime = mtime;
    l->n++;
    return 0;
}

static int walk_cb(void *ctx, const char *rel, int is_dir, uint64_t size, int64_t mtime) {
    return is_dir ? 0 : add_file(ctx, rel, NULL, size, mtime);
}

static void clist_free(clist *l) {
    for (size_t i = 0; i < l->n; i++) {
        free(l->v[i].rel);
        free(l->v[i].src);
    }
    free(l->v);
}

static long find_rel(const clist *l, const char *rel) {
    for (size_t i = 0; i < l->n; i++)
        if (ieq(l->v[i].rel, rel)) return (long)i;
    return -1;
}

// ---- parallel copy ----------------------------------------------------------------------

typedef struct {
    const mp_inst_env *env;
    clist *l;
    const char *tmp;
    size_t *todo, ntodo, next;
    mp_mutex *mu;
    uint64_t written, total, base;
    int stop, fail, fail_io;
    mp_err err;
    mpi_state *st;
} cjob;

static int report(cjob *j, size_t n, mp_err *e) {
    int rc = 0;
    mp_mutex_lock(j->mu);
    j->written += n;
    if (j->env->progress) j->env->progress(j->env->ctx, "copiando", j->base + j->written, j->total);
    if (j->stop || j->fail) {
        rc = -1;
    } else if (j->env->io_check) {
        int k = j->env->io_check(j->env->ctx, j->written, e);
        if (k == 1) j->stop = 1;
        if (k) rc = -1;
    }
    mp_mutex_unlock(j->mu);
    return rc;
}

static void copy_one(cjob *j, cfile *f, uint8_t *buf) {
    mp_err e = {{0}};
    char *dst = mpi_join(j->tmp, f->rel);
    mp_file *in = NULL;
    mp_wfile *w = NULL;
    int io = 0, rc = dst ? 0 : -1;
    if (!dst) mp_err_set(&e, "out of memory");
    if (rc == 0) {
        mp_remove_file(dst);
        rc = mp_file_open_read(f->src, &in, &e);
    }
    if (rc == 0) {
        rc = mp_wfile_create(dst, &w, &e);
        io = rc != 0;
    }
    mp_sha256 s;
    mp_sha256_init(&s);
    for (uint64_t off = 0; rc == 0 && off < f->size;) {
        size_t k = f->size - off < PIECE ? (size_t)(f->size - off) : PIECE, got = 0;
        rc = mp_file_pread(in, buf, k, off, &got, &e);
        if (rc == 0 && got != k) {
            mp_err_set(&e, "%s ended early (was the drive removed?)", f->src);
            rc = -1;
        }
        if (rc == 0) {
            mp_sha256_update(&s, buf, k);
            rc = mp_wfile_write(w, buf, k, &e);
            io = rc != 0;
        }
        if (rc == 0) {
            rc = report(j, k, &e);
            io = rc != 0;
        }
        off += k;
    }
    if (in) mp_file_close(in);
    if (rc == 0) {
        rc = mp_wfile_sync(w, &e);
        io = 1;
    }
    if (w && mp_wfile_close(w, rc ? NULL : &e) != 0 && rc == 0) {
        rc = -1;
        io = 1;
    }
    if (rc == 0 && f->mtime && mp_set_mtime(dst, f->mtime, &e) != 0) {
        rc = -1;
        io = 1;
    }
    free(dst);
    if (rc == 0) {
        mp_sha256_final(&s, f->sha);
        char line[1200], hex[65];
        mpi_hex(f->sha, 32, hex);
        snprintf(line, sizeof line, "%s\t%s", hex, f->rel);
        mp_mutex_lock(j->mu);
        if (j->st) rc = mpi_state_add(j->st, line, &e);
        if (rc == 0) f->done = 1;
        mp_mutex_unlock(j->mu);
        io = 1;
    }
    if (rc) {
        mp_mutex_lock(j->mu);
        if (!j->fail && !j->stop) {
            j->fail = 1;
            j->fail_io = io;
            j->err = e;
        }
        mp_mutex_unlock(j->mu);
    }
}

static void cworker(void *arg) {
    cjob *j = arg;
    uint8_t *buf = malloc(PIECE);
    if (!buf) {
        mp_mutex_lock(j->mu);
        if (!j->fail) {
            j->fail = 1;
            mp_err_set(&j->err, "out of memory");
        }
        mp_mutex_unlock(j->mu);
        return;
    }
    for (;;) {
        mp_mutex_lock(j->mu);
        size_t k = j->next;
        int quit = j->stop || j->fail || k >= j->ntodo;
        if (!quit) j->next++;
        mp_mutex_unlock(j->mu);
        if (quit) break;
        copy_one(j, &j->l->v[j->todo[k]], buf);
    }
    free(buf);
}

// ---- resume ---------------------------------------------------------------------------------

typedef struct {
    clist *l;
    const char *tmp;
    size_t *cand;
    uint8_t (*sha)[32];
    uint8_t *ok;
} rc_ctx;

static int recheck_one(void *ctx, size_t k, unsigned tid) {
    rc_ctx *c = ctx;
    (void)tid;
    cfile *f = &c->l->v[c->cand[k]];
    char *p = mpi_join(c->tmp, f->rel);
    uint8_t sha[32];
    uint64_t size = 0;
    c->ok[k] = p && mpi_file_sha(p, 0, sha, &size, NULL) == 0 && size == f->size && !memcmp(sha, c->sha[k], 32);
    free(p);
    return 0;
}

static void recheck(const mp_inst_env *env, clist *l, const char *tmp, char **done, size_t ndone, uint64_t *base) {
    size_t *cand = malloc(ndone * sizeof *cand + 1), nc = 0;
    uint8_t(*sha)[32] = malloc(ndone * sizeof *sha + 1);
    uint8_t *ok = calloc(ndone + 1, 1);
    if (!cand || !sha || !ok) goto out;
    for (size_t d = 0; d < ndone; d++) {
        char *tab = strchr(done[d], '\t');
        if (!tab || tab - done[d] != 64) continue;
        *tab = 0;
        long i = find_rel(l, tab + 1);
        if (i < 0 || l->v[i].skip || mpi_unhex(done[d], sha[nc], 32) != 0) continue;
        cand[nc++] = (size_t)i;
    }
    if (env->progress) env->progress(env->ctx, "comprobando", 0, nc);
    rc_ctx c = {l, tmp, cand, sha, ok};
    mp_progress prog;
    mp_progress_init(&prog, "comprobando", nc, 0);
    mp_parallel_run(env->threads ? env->threads : 1, nc, recheck_one, &c, &prog);
    mp_progress_free(&prog);
    for (size_t k = 0; k < nc; k++)
        if (ok[k]) {
            cfile *f = &l->v[cand[k]];
            f->done = 1;
            memcpy(f->sha, sha[k], 32);
            *base += f->size;
        }
out:
    free(cand);
    free(sha);
    free(ok);
}

// ---- runtime of a folder with packs -----------------------------------------------------------

static int has_marker(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 8 <= n; i++)
        if (p[i] == 'A' && !memcmp(p + i, "AMPRZPK1", 8)) return 1;
    return 0;
}

static int write_new(const char *dir, const char *rel, const void *p, size_t n, int64_t mtime, uint8_t sha[32],
                     mp_err *e) {
    char *path = mpi_join(dir, rel);
    if (!path) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    char *s = strrchr(path, '/');
    *s = 0;
    int rc = mp_make_dirs(path, e);
    *s = '/';
    mp_remove_file(path);
    mp_wfile *w = NULL;
    if (rc == 0) rc = mp_wfile_create(path, &w, e);
    if (rc == 0) rc = mp_wfile_write(w, p, n, e);
    if (rc == 0) rc = mp_wfile_sync(w, e);
    if (w && mp_wfile_close(w, rc ? NULL : e) != 0) rc = -1;
    if (rc == 0 && mtime) rc = mp_set_mtime(path, mtime, e);
    free(path);
    if (rc == 0) {
        mp_sha256 h;
        mp_sha256_init(&h);
        mp_sha256_update(&h, p, n);
        mp_sha256_final(&h, sha);
    }
    return rc;
}

// ---- list of SHA-256 hashes of what was installed -------------------------------------------

static int save_files(const mp_inst_env *env, const char *title, const clist *l, mp_err *e) {
    mp_jbuf b = {0};
    mp_jbuf_raw(&b, "{\"files\":[");
    for (size_t i = 0; i < l->n; i++) {
        char hex[65];
        mpi_hex(l->v[i].sha, 32, hex);
        mp_jbuf_raw(&b, i ? ",{\"path\":" : "{\"path\":");
        mp_jbuf_str(&b, l->v[i].rel);
        mp_jbuf_raw(&b, ",\"size\":");
        mp_jbuf_u64(&b, l->v[i].size);
        mp_jbuf_raw(&b, ",\"sha\":");
        mp_jbuf_str(&b, hex);
        mp_jbuf_raw(&b, "}");
    }
    mp_jbuf_raw(&b, "]}\n");
    char path[1200], dir[1200];
    snprintf(dir, sizeof dir, "%s/maxpack/installed", env->internal);
    snprintf(path, sizeof path, "%s/%s.files.json", dir, title);
    int rc = b.oom ? -1 : mp_make_dirs(dir, e);
    if (b.oom) mp_err_set(e, "out of memory");
    if (rc == 0) rc = mpi_write_replace(path, b.p, b.n, e);
    mp_jbuf_free(&b);
    return rc;
}

// ---- install as is --------------------------------------------------------------------------

int mpi_copy_install(const mp_inst_env *env, const mp_inst_req *req, const mp_inst_found *f, const char *tmp_dir,
                     const char *final_path, const mp_inst_existing *ex, size_t nex, mp_inst_result *r, mp_err *e) {
    const char *base = strrchr(f->path, '/');
    base = base ? base + 1 : f->path;
    int image = f->kind == MP_SRC_IMAGE;
    clist l = {NULL, 0, 0, f->path, 0};
    int rc = 0;
    if (image) rc = add_file(&l, base, f->path, f->install_size, 0);
    else rc = mp_dir_walk(f->path, walk_cb, &l, e);
    if (image) {
        int64_t mt = 0;
        if (rc == 0 && mp_stat(f->path, NULL, &mt) == 0) l.v[0].mtime = mt;
    }
    if (rc != 0) {
        if (l.bad_name) mp_err_set(e, "%s has a file name with a tab or a line break", f->path);
        else if (!e || !e->msg[0]) mp_err_set(e, "could not read %s", f->path);
        clist_free(&l);
        r->status = MP_INST_FAILED;
        r->code = l.bad_name ? "bad_source" : "io_error";
        return -1;
    }

    // Runtime: a folder with packs must carry ampr-emu-ng to be playable.
    int replace = 0;
    long rt = -1, idx3 = -1;
    if (!image && f->packed) {
        rt = find_rel(&l, RUNTIME_PATH);
        idx3 = find_rel(&l, "ampr_emu.index");
        int zstd = 0;
        if (rt >= 0 && l.v[rt].size <= RUNTIME_MAX) {
            char *buf = NULL;
            size_t n = 0;
            if (mpi_read_small(l.v[rt].src, RUNTIME_MAX, &buf, &n, NULL) == 0) zstd = has_marker((uint8_t *)buf, n);
            free(buf);
        }
        if (!zstd) {
            if (!env->runtime.data || !env->runtime.len) {
                mp_err_set(e, "the folder has zstd packs but no ampr-emu-ng, and this installer carries none");
                clist_free(&l);
                r->status = MP_INST_FAILED;
                r->code = "no_runtime";
                return -1;
            }
            if (idx3 < 0) {
                mp_err_set(e, "the folder has zstd packs but no ampr_emu.index");
                clist_free(&l);
                r->status = MP_INST_FAILED;
                r->code = "bad_source";
                return -1;
            }
            replace = 1;
            if (rt >= 0) l.v[rt].skip = 1;
            l.v[idx3].skip = 1;
            if (env->notify)
                env->notify(env->ctx, "The folder has no runtime that reads zstd packs: the installer's ampr-emu-ng is being put in");
        }
    }

    // State: a copy's identity is its source and its size.
    uint8_t aid[32];
    char ident[1300];
    snprintf(ident, sizeof ident, "MAXPACK-COPY\n%s\n%llu\n%zu", f->path, (unsigned long long)f->install_size, l.n);
    mp_sha256 h;
    mp_sha256_init(&h);
    mp_sha256_update(&h, ident, strlen(ident));
    mp_sha256_final(&h, aid);
    mpi_state *st = NULL;
    int resumed = 0;
    char **done = NULL;
    size_t ndone = 0;
    uint64_t done_bytes = 0;
    rc = mpi_state_begin(env, f->title_id, aid, f->path, req->dest_root, tmp_dir, &resumed, &done, &ndone, &st, e);
    if (rc == 0) rc = mp_make_dirs(tmp_dir, e);
    if (rc == 0 && resumed && ndone) recheck(env, &l, tmp_dir, done, ndone, &done_bytes);
    mpi_done_free(done, ndone);

    size_t *todo = malloc((l.n + 1) * sizeof *todo), ntodo = 0;
    if (!todo) {
        mp_err_set(e, "out of memory");
        rc = -1;
    }
    char last[1200] = "";
    for (size_t i = 0; rc == 0 && i < l.n; i++) {
        if (l.v[i].done || l.v[i].skip) continue;
        todo[ntodo++] = i;
        const char *s = strrchr(l.v[i].rel, '/');
        if (!s) continue;
        char d[1200];
        snprintf(d, sizeof d, "%s/%.*s", tmp_dir, (int)(s - l.v[i].rel), l.v[i].rel);
        if (strcmp(d, last) != 0) {
            rc = mp_make_dirs(d, e);
            snprintf(last, sizeof last, "%s", d);
        }
    }
    const char *code = "io_error";
    int stopped = 0;
    if (rc == 0) {
        cjob j;
        memset(&j, 0, sizeof j);
        j.env = env;
        j.l = &l;
        j.tmp = tmp_dir;
        j.todo = todo;
        j.ntodo = ntodo;
        j.total = f->install_size;
        j.base = done_bytes;
        j.st = st;
        j.mu = mp_mutex_new();
        if (!j.mu) {
            mp_err_set(e, "out of memory");
            rc = -1;
        } else {
            unsigned threads = env->threads ? env->threads : 1;
            if (threads > 32) threads = 32;
            mp_thread *t[32];
            unsigned started = 0;
            for (unsigned k = 1; k < threads; k++)
                if (mp_thread_start(&t[started], cworker, &j) == 0) started++;
            cworker(&j);
            for (unsigned k = 0; k < started; k++) mp_thread_join(t[k]);
            stopped = j.stop;
            if (j.fail) {
                rc = -1;
                *e = j.err;
            }
            mp_mutex_free(j.mu);
        }
    }
    free(todo);
    mpi_state_close(st);
    if (rc == 0 && !stopped && replace) {   // the installer's runtime and its row in ampr_emu.index
        char *idx = NULL, *fixed = NULL;
        size_t n = 0, fn = 0;
        rc = mpi_read_small(l.v[idx3].src, (size_t)1 << 30, &idx, &n, e);
        if (rc == 0)
            rc = mp_idx3_set_row((uint8_t *)idx, n, RUNTIME_PATH, env->runtime.len, env->runtime.mtime,
                                 (uint8_t **)&fixed, &fn, e);
        if (rc == 0) {
            rc = write_new(tmp_dir, "ampr_emu.index", fixed, fn, l.v[idx3].mtime, l.v[idx3].sha, e);
            l.v[idx3].size = fn;
        }
        free(idx);
        free(fixed);
        if (rc == 0) {
            uint8_t sha[32];
            rc = write_new(tmp_dir, RUNTIME_PATH, env->runtime.data, env->runtime.len, env->runtime.mtime, sha, e);
            if (rc == 0 && rt >= 0) {
                memcpy(l.v[rt].sha, sha, 32);
                l.v[rt].size = env->runtime.len;
            } else if (rc == 0 && add_file(&l, RUNTIME_PATH, "", env->runtime.len, env->runtime.mtime) == 0) {
                memcpy(l.v[l.n - 1].sha, sha, 32);
            }
        }
        if (rc) code = "io_error";
    }
    if (rc == 0 && stopped) {
        clist_free(&l);
        r->status = MP_INST_STOPPED;
        r->code = "stopped";
        return 0;
    }
    if (rc != 0) {
        clist_free(&l);
        r->status = MP_INST_FAILED;
        r->code = code;
        return -1;
    }

    // To the destination: the image is the only file in the temporary folder.
    if (env->progress) env->progress(env->ctx, "terminando", f->install_size, f->install_size);
    char item[1300];
    if (image) snprintf(item, sizeof item, "%s/%s", tmp_dir, base);
    else snprintf(item, sizeof item, "%s", tmp_dir);
    rc = save_files(env, f->title_id, &l, e);
    if (rc == 0) rc = mpi_place(env, item, final_path, ex, nex, e);
    if (rc == 0 && image) mp_remove_tree(tmp_dir, NULL);
    mp_inst_record rec;
    memset(&rec, 0, sizeof rec);
    if (rc == 0) {
        snprintf(rec.title_id, sizeof rec.title_id, "%s", f->title_id);
        snprintf(rec.version, sizeof rec.version, "%s", f->version);
        snprintf(rec.name, sizeof rec.name, "%s", f->name);
        snprintf(rec.dir, sizeof rec.dir, "%s", final_path);
        snprintf(rec.p5z, sizeof rec.p5z, "%s", f->path);
        rec.kind = f->kind;
        rec.packed = f->packed;
        rec.runtime_replaced = replace;
        rec.smp_pending = 1;
        rec.installed_at = (int64_t)time(NULL);
        rc = mp_inst_record_save(env, &rec, e);
    }
    clist_free(&l);
    if (rc != 0) {
        r->status = MP_INST_FAILED;
        r->code = "io_error";
        return -1;
    }
    mpi_state_remove(env, f->title_id);
    r->runtime_replaced = replace;
    if (env->notify) {
        char msg[300];
        snprintf(msg, sizeof msg, "%s installed", f->name[0] ? f->name : f->title_id);
        env->notify(env->ctx, msg);
    }
    r->status = MP_INST_DONE;
    r->code = "done";
    return 0;
}
