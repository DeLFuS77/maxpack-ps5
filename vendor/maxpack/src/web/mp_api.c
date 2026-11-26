// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_inst_internal.h"
#include "mp_json.h"
#include "mp_p5z_internal.h"
#include "mp_platform.h"
#include "mp_version.h"

#define QUEUE_MAX 32
#define ICON_MAX (8u << 20)

enum { JOB_INSTALL = 1, JOB_VERIFY, JOB_UNINSTALL };

typedef struct {
    int kind;
    char title[16], name[128];
    char p5z[1024], dest[1024];
    char **replace;
    size_t nreplace;
    int delete_p5z;
} job_t;

struct mp_api {
    mp_inst_env env;    // with the API's callbacks
    mp_inst_env user;   // those of whoever created it
    mp_smp_cfg smp;
    unsigned smp_retry_ms;
    mp_mutex *mu;       // shared state
    mp_mutex *run_mu;   // one job or one ShadowMountPlus sync at a time
    mp_inst_found *found;
    size_t nfound;
    job_t queue[QUEUE_MAX];
    size_t qn;
    job_t cur;
    int busy, pause_req;
    char stage[32];
    uint64_t done, total;
    int has_last, last_counts;
    uint64_t last_ok, last_bad;
    mp_inst_existing *last_ex;   // with needs_confirm: what needs confirming
    size_t last_nex;
    char last_src[1024], last_dest[1024];
    char last_kind[16], last_title[16], last_code[32], last_detail[600];
    int smp_alive;
    char smp_version[32];
    volatile int quit;
    void (*quit_cb)(void *);   // POST /api/quit
    void *quit_ctx;
    mp_thread *worker, *smp_thread;
};

static void job_free(job_t *j) {
    for (size_t i = 0; i < j->nreplace; i++) free(j->replace[i]);
    free(j->replace);
    memset(j, 0, sizeof *j);
}

// ---- engine callbacks -----------------------------------------------------------------------

static int api_io(void *ctx, uint64_t written, mp_err *e) {
    mp_api *a = ctx;
    if (a->user.io_check) {
        int k = a->user.io_check(a->user.ctx, written, e);
        if (k) return k;
    }
    mp_mutex_lock(a->mu);
    int p = a->pause_req;
    mp_mutex_unlock(a->mu);
    return p ? 1 : 0;
}

static void api_progress(void *ctx, const char *stage, uint64_t done, uint64_t total) {
    mp_api *a = ctx;
    mp_mutex_lock(a->mu);
    snprintf(a->stage, sizeof a->stage, "%s", stage);
    a->done = done;
    a->total = total;
    mp_mutex_unlock(a->mu);
    if (a->user.progress) a->user.progress(a->user.ctx, stage, done, total);
}

static int api_free_space(void *ctx, const char *path, uint64_t *out) {
    mp_api *a = ctx;
    return a->user.free_space(a->user.ctx, path, out);
}

static void api_notify(void *ctx, const char *msg) {
    mp_api *a = ctx;
    if (a->user.notify) a->user.notify(a->user.ctx, msg);
}

// ---- allowed paths -----------------------------------------------------------------------

static int dotdot(const char *p) {
    return strstr(p, "/../") || !strncmp(p, "../", 3) || (strlen(p) >= 3 && !strcmp(p + strlen(p) - 3, "/..")) ||
           strchr(p, '\\') != NULL;
}

static int inside_roots(const mp_api *a, const char *path) {
    if (mp_path_is_inside(path, a->env.internal)) return 1;
    const char *d[16];
    size_t n = mp_inst_drives(&a->env, d, 16);
    for (size_t i = 0; i < n; i++)
        if (mp_path_is_inside(path, d[i])) return 1;
    return 0;
}

static int is_image(const char *p) {
    return mpi_has_ext(p, ".exfat") || mpi_has_ext(p, ".ffpkg") || mpi_has_ext(p, ".ffpfs") || mpi_has_ext(p, ".ffpfsc");
}

static int game_dir(const char *path) {
    char *p = mpi_join(path, "sce_sys/param.json");
    int ok = p && mp_path_kind(p) == 1;
    free(p);
    return ok;
}

static int dest_ok(const mp_api *a, const char *dest);

// What can be installed: a .p5z, an image or a game folder, inside /data or a
// drive (and never a root).
static int source_ok(const mp_api *a, const char *path) {
    if (!path || !*path || dotdot(path) || !inside_roots(a, path) || dest_ok(a, path)) return 0;
    int k = mp_path_kind(path);
    if (k == 1) return mpi_has_ext(path, ".p5z") || is_image(path);
    return k == 2 && game_dir(path);
}

static int dest_ok(const mp_api *a, const char *dest) {
    if (!dest || mp_path_kind(dest) != 2) return 0;
    if (!strcmp(dest, a->env.internal)) return 1;
    const char *d[16];
    size_t n = mp_inst_drives(&a->env, d, 16);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(dest, d[i])) return 1;
    return 0;
}

// Paths of title_id that ShadowMountPlus knows, only if they are inside the roots (and are not one).
static void smp_known(mp_api *a, const char *title, char ***out, size_t *n) {
    *out = NULL;
    *n = 0;
    mp_mutex_lock(a->mu);
    int alive = a->smp_alive;
    mp_mutex_unlock(a->mu);
    if (!alive) return;
    mp_smp_game *g = NULL;
    size_t ng = 0;
    if (mp_smp_games(&a->smp, &g, &ng, NULL) != 0) return;
    char **v = calloc(ng + 1, sizeof *v);
    size_t k = 0;
    for (size_t i = 0; v && i < ng; i++) {
        if (strcmp(g[i].title_id, title) != 0 || !g[i].path[0] || dotdot(g[i].path) || !inside_roots(a, g[i].path))
            continue;
        if (!strcmp(g[i].path, a->env.internal) || dest_ok(a, g[i].path)) continue;   // never a root
        v[k] = mp_strdup(g[i].path);
        if (v[k]) k++;
    }
    mp_smp_games_free(g, ng);
    *out = v;
    *n = k;
}

static void free_strs(char **v, size_t n) {
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

// ---- jobs -----------------------------------------------------------------------------------

static void set_last(mp_api *a, const char *kind, const char *title, const char *code, const char *detail) {
    mp_mutex_lock(a->mu);
    a->has_last = 1;
    snprintf(a->last_kind, sizeof a->last_kind, "%s", kind);
    snprintf(a->last_title, sizeof a->last_title, "%s", title);
    snprintf(a->last_code, sizeof a->last_code, "%s", code);
    snprintf(a->last_detail, sizeof a->last_detail, "%s", detail ? detail : "");
    a->last_counts = 0;
    mp_inst_existing_free(a->last_ex, a->last_nex);
    a->last_ex = NULL;
    a->last_nex = 0;
    a->last_src[0] = a->last_dest[0] = 0;
    mp_mutex_unlock(a->mu);
}

static void register_smp(mp_api *a, const char *title) {
    mp_inst_record rec;
    if (mp_inst_record_load(&a->env, title, &rec, NULL) != 0) return;
    char why[256];
    int rc = mp_smp_register(&a->smp, title, rec.dir, rec.packed, why, sizeof why);
    rec.smp_pending = rc != 0;
    snprintf(rec.smp_why, sizeof rec.smp_why, "%s", rc ? why : "");
    mp_inst_record_save(&a->env, &rec, NULL);
}

static void run_install(mp_api *a, job_t *j) {
    char **known = NULL;
    size_t nknown = 0;
    smp_known(a, j->title, &known, &nknown);
    mp_inst_req q;
    memset(&q, 0, sizeof q);
    q.p5z_path = j->p5z;
    q.dest_root = j->dest;
    q.replace = (const char *const *)j->replace;
    q.nreplace = j->nreplace;
    q.known = (const char *const *)known;
    q.nknown = nknown;
    q.delete_p5z = j->delete_p5z;
    mp_inst_result r;
    mp_err e = {{0}};
    mp_inst_install(&a->env, &q, &r, &e);
    free_strs(known, nknown);
    char detail[600];
    if (r.status == MP_INST_NO_SPACE)
        snprintf(detail, sizeof detail, "%llu bytes missing", (unsigned long long)(r.need - r.avail));
    else
        snprintf(detail, sizeof detail, "%s", e.msg);
    if (r.status == MP_INST_DONE) {
        api_progress(a, "registrando", r.need, r.need);
        register_smp(a, r.title_id);
    }
    else if (r.status == MP_INST_FAILED && a->user.notify) {
        char msg[800];
        snprintf(msg, sizeof msg, "Could not install %s: %s", j->name[0] ? j->name : j->title, e.msg);
        a->user.notify(a->user.ctx, msg);
    }
    set_last(a, "install", j->title, r.code ? r.code : "io_error", detail);
    if (r.status == MP_INST_NEEDS_CONFIRM) {   // something changed since it was queued: the page asks again
        mp_mutex_lock(a->mu);
        a->last_ex = r.existing;
        a->last_nex = r.nexisting;
        r.existing = NULL;
        r.nexisting = 0;
        snprintf(a->last_src, sizeof a->last_src, "%s", j->p5z);
        snprintf(a->last_dest, sizeof a->last_dest, "%s", j->dest);
        mp_mutex_unlock(a->mu);
    }
    mp_inst_result_free(&r);
}

static void run_verify(mp_api *a, job_t *j) {
    mp_inst_verify_result r;
    mp_err e = {{0}};
    if (mp_inst_verify(&a->env, j->title, &r, &e) != 0) {
        set_last(a, "verify", j->title, "io_error", e.msg);
        return;
    }
    char detail[200];
    snprintf(detail, sizeof detail, "%llu OK, %llu failed%s", (unsigned long long)r.files_ok,
             (unsigned long long)r.files_bad, r.p5z_checked ? "" : " (without the .p5z to compare the loose files)");
    set_last(a, "verify", j->title, r.files_bad ? "verify_bad" : "done", detail);
    mp_mutex_lock(a->mu);
    a->last_counts = 1;
    a->last_ok = r.files_ok;
    a->last_bad = r.files_bad;
    mp_mutex_unlock(a->mu);
    if (a->user.notify) {
        char msg[300];
        snprintf(msg, sizeof msg, "%s: %s", j->title, detail);
        a->user.notify(a->user.ctx, msg);
    }
}

static void run_uninstall(mp_api *a, job_t *j) {
    mp_inst_record rec;
    mp_err e = {{0}};
    if (mp_inst_record_load(&a->env, j->title, &rec, &e) != 0 || mp_inst_uninstall(&a->env, j->title, &e) != 0) {
        set_last(a, "uninstall", j->title, "io_error", e.msg);
        return;
    }
    char why[256];
    mp_smp_forget(&a->smp, rec.dir, why, sizeof why);
    set_last(a, "uninstall", j->title, "done", "");
    if (a->user.notify) {
        char msg[300];
        snprintf(msg, sizeof msg, "%s uninstalled", rec.name[0] ? rec.name : j->title);
        a->user.notify(a->user.ctx, msg);
    }
}

static void worker(void *arg) {
    mp_api *a = arg;
    while (!a->quit) {
        mp_mutex_lock(a->mu);
        if (a->qn == 0) {
            mp_mutex_unlock(a->mu);
            mp_sleep_ms(20);
            continue;
        }
        a->cur = a->queue[0];
        memmove(a->queue, a->queue + 1, (a->qn - 1) * sizeof *a->queue);
        a->qn--;
        a->busy = 1;
        a->pause_req = 0;
        a->stage[0] = 0;
        a->done = a->total = 0;
        mp_mutex_unlock(a->mu);
        mp_mutex_lock(a->run_mu);
        if (a->cur.kind == JOB_INSTALL) run_install(a, &a->cur);
        else if (a->cur.kind == JOB_VERIFY) run_verify(a, &a->cur);
        else run_uninstall(a, &a->cur);
        mp_mutex_unlock(a->run_mu);
        mp_mutex_lock(a->mu);
        job_free(&a->cur);
        a->busy = 0;
        a->pause_req = 0;
        mp_mutex_unlock(a->mu);
    }
}

static void smp_loop(void *arg) {
    mp_api *a = arg;
    while (!a->quit) {
        char ver[32];
        int alive = mp_smp_alive(&a->smp, ver, sizeof ver);
        mp_mutex_lock(a->mu);
        a->smp_alive = alive;
        snprintf(a->smp_version, sizeof a->smp_version, "%s", alive ? ver : "");
        mp_mutex_unlock(a->mu);
        if (alive) {
            mp_mutex_lock(a->run_mu);
            mp_inst_env quiet = a->env;
            quiet.notify = NULL;
            mp_inst_smp_sync(&quiet, &a->smp, NULL);
            mp_mutex_unlock(a->run_mu);
        }
        for (unsigned t = 0; t < a->smp_retry_ms && !a->quit; t += 10) mp_sleep_ms(10);
    }
}

// ---- respuestas -----------------------------------------------------------------------------

static void respond(mp_http_resp *rs, int status, mp_jbuf *b) {
    rs->status = status;
    rs->content_type = "application/json; charset=utf-8";
    if (b->oom) {
        mp_jbuf_free(b);
        rs->status = 500;
        rs->body = mp_strdup("{\"ok\":false,\"code\":\"oom\"}");
        rs->body_len = rs->body ? strlen(rs->body) : 0;
        return;
    }
    rs->body = b->p;
    rs->body_len = b->n;
    b->p = NULL;
}

static void respond_err(mp_http_resp *rs, int status, const char *code, const char *detail) {
    mp_jbuf b = {0};
    mp_jbuf_raw(&b, "{\"ok\":false,\"code\":");
    mp_jbuf_str(&b, code);
    mp_jbuf_raw(&b, ",\"detail\":");
    mp_jbuf_str(&b, detail ? detail : "");
    mp_jbuf_raw(&b, "}");
    respond(rs, status, &b);
}

static void respond_ok(mp_http_resp *rs) {
    mp_jbuf b = {0};
    mp_jbuf_raw(&b, "{\"ok\":true}");
    respond(rs, 200, &b);
}

static void key(mp_jbuf *b, const char *k, int first) {
    mp_jbuf_raw(b, first ? "" : ",");
    mp_jbuf_str(b, k);
    mp_jbuf_raw(b, ":");
}

static const char *kind_name(int kind) {
    return kind == MP_SRC_FOLDER ? "folder" : kind == MP_SRC_IMAGE ? "image" : "p5z";
}

static void put_found(mp_jbuf *b, const mp_inst_found *f) {
    mp_jbuf_raw(b, "{");
    key(b, "path", 1);
    mp_jbuf_str(b, f->path);
    key(b, "kind", 0);
    mp_jbuf_str(b, kind_name(f->kind));
    key(b, "packed", 0);
    mp_jbuf_raw(b, f->packed ? "true" : "false");
    key(b, "title_id", 0);
    mp_jbuf_str(b, f->title_id);
    key(b, "version", 0);
    mp_jbuf_str(b, f->version);
    key(b, "name", 0);
    mp_jbuf_str(b, f->name);
    key(b, "p5z_size", 0);
    mp_jbuf_u64(b, f->p5z_size);
    key(b, "install_size", 0);
    mp_jbuf_u64(b, f->install_size);
    key(b, "original_size", 0);
    mp_jbuf_u64(b, f->original_size);
    mp_jbuf_raw(b, "}");
}

static uint64_t free_of(const mp_api *a, const char *root) {
    uint64_t f = 0;
    if (a->env.free_space) a->env.free_space(a->env.ctx, root, &f);
    else mp_disk_free(root, &f, NULL);
    return f;
}

static void put_drive(mp_jbuf *b, const mp_api *a, const char *root, int internal, int first) {
    mp_jbuf_raw(b, first ? "{" : ",{");
    key(b, "root", 1);
    mp_jbuf_str(b, root);
    key(b, "internal", 0);
    mp_jbuf_raw(b, internal ? "true" : "false");
    key(b, "free", 0);
    mp_jbuf_u64(b, free_of(a, root));
    mp_jbuf_raw(b, "}");
}

static void put_existing(mp_jbuf *b, const mp_inst_existing *ex, size_t n) {
    mp_jbuf_raw(b, "[");
    for (size_t i = 0; i < n; i++) {
        mp_jbuf_raw(b, i ? ",{" : "{");
        key(b, "path", 1);
        mp_jbuf_str(b, ex[i].path);
        key(b, "size", 0);
        mp_jbuf_u64(b, ex[i].size);
        key(b, "from_maxpack", 0);
        mp_jbuf_raw(b, ex[i].from_maxpack ? "true" : "false");
        mp_jbuf_raw(b, "}");
    }
    mp_jbuf_raw(b, "]");
}

static void handle_state(mp_api *a, mp_http_resp *rs) {
    mp_jbuf b = {0};
    mp_jbuf_raw(&b, "{\"ok\":true");
    key(&b, "version", 0);
    mp_jbuf_str(&b, MP_VERSION);
    key(&b, "drives", 0);
    mp_jbuf_raw(&b, "[");
    put_drive(&b, a, a->env.internal, 1, 1);
    const char *d[16];
    size_t nd = mp_inst_drives(&a->env, d, 16);
    for (size_t i = 0; i < nd; i++) put_drive(&b, a, d[i], 0, 0);
    mp_jbuf_raw(&b, "]");
    mp_inst_env quiet = a->env;
    quiet.notify = NULL;
    mp_inst_record *rec = NULL;
    size_t nrec = 0;
    mp_inst_list(&quiet, &rec, &nrec, NULL);
    key(&b, "installed", 0);
    mp_jbuf_raw(&b, "[");
    for (size_t i = 0; i < nrec; i++) {
        mp_jbuf_raw(&b, i ? ",{" : "{");
        key(&b, "title_id", 1);
        mp_jbuf_str(&b, rec[i].title_id);
        key(&b, "name", 0);
        mp_jbuf_str(&b, rec[i].name);
        key(&b, "version", 0);
        mp_jbuf_str(&b, rec[i].version);
        key(&b, "dir", 0);
        mp_jbuf_str(&b, rec[i].dir);
        key(&b, "kind", 0);
        mp_jbuf_str(&b, kind_name(rec[i].kind));
        key(&b, "smp_pending", 0);
        mp_jbuf_raw(&b, rec[i].smp_pending ? "true" : "false");
        key(&b, "smp_why", 0);
        mp_jbuf_str(&b, rec[i].smp_why);
        mp_jbuf_raw(&b, "}");
    }
    mp_jbuf_raw(&b, "]");
    free(rec);
    mp_inst_pending_item *pend = NULL;
    size_t npend = 0;
    mp_inst_pending(&quiet, &pend, &npend, NULL);
    mp_mutex_lock(a->mu);
    char busy_title[16] = "";
    if (a->busy) snprintf(busy_title, sizeof busy_title, "%s", a->cur.title);
    mp_mutex_unlock(a->mu);
    key(&b, "pending", 0);
    mp_jbuf_raw(&b, "[");
    for (size_t i = 0, first = 1; i < npend; i++) {
        if (!strcmp(pend[i].title_id, busy_title)) continue;   // the one in progress is not "half-done"
        mp_jbuf_raw(&b, first ? "{" : ",{");
        first = 0;
        key(&b, "title_id", 1);
        mp_jbuf_str(&b, pend[i].title_id);
        key(&b, "source", 0);
        mp_jbuf_str(&b, pend[i].source);
        key(&b, "dest", 0);
        mp_jbuf_str(&b, pend[i].dest);
        key(&b, "bytes", 0);
        mp_jbuf_u64(&b, pend[i].bytes);
        mp_jbuf_raw(&b, "}");
    }
    mp_jbuf_raw(&b, "]");
    free(pend);
    mp_mutex_lock(a->mu);
    key(&b, "found", 0);
    mp_jbuf_raw(&b, "[");
    for (size_t i = 0; i < a->nfound; i++) {
        if (i) mp_jbuf_raw(&b, ",");
        put_found(&b, &a->found[i]);
    }
    mp_jbuf_raw(&b, "]");
    key(&b, "job", 0);
    if (a->busy) {
        static const char *kinds[] = {"", "install", "verify", "uninstall"};
        mp_jbuf_raw(&b, "{");
        key(&b, "kind", 1);
        mp_jbuf_str(&b, kinds[a->cur.kind]);
        key(&b, "title_id", 0);
        mp_jbuf_str(&b, a->cur.title);
        key(&b, "name", 0);
        mp_jbuf_str(&b, a->cur.name);
        key(&b, "stage", 0);
        mp_jbuf_str(&b, a->stage);
        key(&b, "done", 0);
        mp_jbuf_u64(&b, a->done);
        key(&b, "total", 0);
        mp_jbuf_u64(&b, a->total);
        key(&b, "paused", 0);
        mp_jbuf_raw(&b, a->pause_req ? "true" : "false");
        mp_jbuf_raw(&b, "}");
    } else {
        mp_jbuf_raw(&b, "null");
    }
    key(&b, "queue", 0);
    mp_jbuf_u64(&b, a->qn);
    key(&b, "last", 0);
    if (a->has_last) {
        mp_jbuf_raw(&b, "{");
        key(&b, "kind", 1);
        mp_jbuf_str(&b, a->last_kind);
        key(&b, "title_id", 0);
        mp_jbuf_str(&b, a->last_title);
        key(&b, "code", 0);
        mp_jbuf_str(&b, a->last_code);
        key(&b, "detail", 0);
        mp_jbuf_str(&b, a->last_detail);
        if (a->last_ex) {
            key(&b, "existing", 0);
            put_existing(&b, a->last_ex, a->last_nex);
            key(&b, "source", 0);
            mp_jbuf_str(&b, a->last_src);
            key(&b, "dest", 0);
            mp_jbuf_str(&b, a->last_dest);
        }
        if (a->last_counts) {
            key(&b, "files_ok", 0);
            mp_jbuf_u64(&b, a->last_ok);
            key(&b, "files_bad", 0);
            mp_jbuf_u64(&b, a->last_bad);
        }
        mp_jbuf_raw(&b, "}");
    } else {
        mp_jbuf_raw(&b, "null");
    }
    key(&b, "smp", 0);
    mp_jbuf_raw(&b, "{");
    key(&b, "alive", 1);
    mp_jbuf_raw(&b, a->smp_alive ? "true" : "false");
    key(&b, "version", 0);
    mp_jbuf_str(&b, a->smp_version);
    mp_jbuf_raw(&b, "}");
    mp_mutex_unlock(a->mu);
    mp_jbuf_raw(&b, "}");
    respond(rs, 200, &b);
}

static void rescan(mp_api *a) {
    mp_inst_env quiet = a->env;
    quiet.notify = NULL;
    mp_inst_found *v = NULL;
    size_t n = 0;
    if (mp_inst_scan(&quiet, &v, &n, NULL) != 0) return;
    mp_mutex_lock(a->mu);
    mp_inst_found_free(a->found, a->nfound);
    a->found = v;
    a->nfound = n;
    mp_mutex_unlock(a->mu);
}

static int title_busy(mp_api *a, const char *title) {
    int busy = a->busy && !strcmp(a->cur.title, title);
    for (size_t i = 0; i < a->qn && !busy; i++) busy = !strcmp(a->queue[i].title, title);
    return busy;
}

// Queues j (takes over its memory). 0 if it fits and the title is not busy.
static const char *enqueue(mp_api *a, job_t *j) {
    const char *err = NULL;
    mp_mutex_lock(a->mu);
    if (title_busy(a, j->title) || a->qn >= QUEUE_MAX) err = "busy";
    else a->queue[a->qn++] = *j;
    mp_mutex_unlock(a->mu);
    if (err) job_free(j);
    return err;
}

static void handle_probe(mp_api *a, const mp_json *in, mp_http_resp *rs) {
    const char *path = mp_json_str(mp_json_get(in, "path"));
    if (!source_ok(a, path)) {
        respond_err(rs, 200, "bad_path", "path not allowed");
        return;
    }
    mp_inst_found f;
    mp_err e = {{0}};
    if (mp_inst_probe_any(path, &f, &e) != 0) {
        respond_err(rs, 200, mpi_has_ext(path, ".p5z") ? "bad_p5z" : "bad_source", e.msg);
        return;
    }
    mp_jbuf b = {0};
    mp_jbuf_raw(&b, "{\"ok\":true,\"found\":");
    put_found(&b, &f);
    mp_jbuf_raw(&b, "}");
    free(f.path);
    respond(rs, 200, &b);
}

static void handle_install(mp_api *a, const mp_json *in, mp_http_resp *rs) {
    const char *path = mp_json_str(mp_json_get(in, "p5z")), *dest = mp_json_str(mp_json_get(in, "dest"));
    if (!source_ok(a, path)) {
        respond_err(rs, 200, "bad_path", "path not allowed");
        return;
    }
    if (!dest_ok(a, dest)) {
        respond_err(rs, 200, "bad_dest", "destination not available");
        return;
    }
    mp_inst_found f;
    mp_err e = {{0}};
    if (mp_inst_probe_any(path, &f, &e) != 0) {
        respond_err(rs, 200, mpi_has_ext(path, ".p5z") ? "bad_p5z" : "bad_source", e.msg);
        return;
    }
    mp_mutex_lock(a->mu);
    int busy = title_busy(a, f.title_id);
    mp_mutex_unlock(a->mu);
    if (busy) {
        free(f.path);
        respond_err(rs, 200, "busy", "that game is already being installed or is queued");
        return;
    }
    job_t j;
    memset(&j, 0, sizeof j);
    j.kind = JOB_INSTALL;
    snprintf(j.title, sizeof j.title, "%s", f.title_id);
    snprintf(j.name, sizeof j.name, "%s", f.name);
    snprintf(j.p5z, sizeof j.p5z, "%s", path);
    snprintf(j.dest, sizeof j.dest, "%s", dest);
    int del = 0;
    mp_json_bool(mp_json_get(in, "delete_p5z"), &del);
    j.delete_p5z = del && f.kind == MP_SRC_P5Z;
    const mp_json *rep = mp_json_get(in, "replace");
    size_t nr = mp_json_len(rep);
    j.replace = calloc(nr + 1, sizeof *j.replace);
    for (size_t i = 0; j.replace && i < nr; i++) {
        const char *s = mp_json_str(mp_json_at(rep, i));
        if (s && (j.replace[j.nreplace] = mp_strdup(s)) != NULL) j.nreplace++;
    }
    // The same decision the engine will make (mp_inst_plan_make), to answer right away.
    char **known = NULL;
    size_t nknown = 0;
    smp_known(a, f.title_id, &known, &nknown);
    free(f.path);
    mp_inst_req q;
    memset(&q, 0, sizeof q);
    q.p5z_path = j.p5z;
    q.dest_root = j.dest;
    q.replace = (const char *const *)j.replace;
    q.nreplace = j.nreplace;
    q.known = (const char *const *)known;
    q.nknown = nknown;
    mp_inst_plan pl;
    const char *code = NULL;
    int prc = mp_inst_plan_make(&a->env, &q, &pl, &code, &e);
    free_strs(known, nknown);
    if (prc != 0) {
        job_free(&j);
        respond_err(rs, 200, code ? code : "io_error", e.msg);
        return;
    }
    if (pl.status == MP_INST_NEEDS_CONFIRM) {
        mp_jbuf b = {0};
        mp_jbuf_raw(&b, "{\"ok\":false,\"code\":\"needs_confirm\",\"existing\":");
        put_existing(&b, pl.ex, pl.nex);
        mp_jbuf_raw(&b, "}");
        mp_inst_plan_free(&pl);
        job_free(&j);
        respond(rs, 200, &b);
        return;
    }
    if (pl.status == MP_INST_NO_SPACE) {
        mp_jbuf b = {0};
        mp_jbuf_raw(&b, "{\"ok\":false,\"code\":\"no_space\",\"need\":");
        mp_jbuf_u64(&b, pl.need);
        mp_jbuf_raw(&b, ",\"avail\":");
        mp_jbuf_u64(&b, pl.avail);
        mp_jbuf_raw(&b, "}");
        mp_inst_plan_free(&pl);
        job_free(&j);
        respond(rs, 200, &b);
        return;
    }
    mp_inst_plan_free(&pl);
    const char *err = enqueue(a, &j);
    if (err) respond_err(rs, 200, err, "that game is already being installed or the queue is full");
    else respond_ok(rs);
}

static void handle_title_job(mp_api *a, const mp_json *in, mp_http_resp *rs, int kind) {
    const char *title = mp_json_str(mp_json_get(in, "title_id"));
    mp_inst_record rec;
    if (!title || !mp_inst_title_ok(title) || mp_inst_record_load(&a->env, title, &rec, NULL) != 0) {
        respond_err(rs, 200, "not_found", "that game is not installed with MaxPack");
        return;
    }
    mp_mutex_lock(a->mu);
    int busy = title_busy(a, title);
    mp_mutex_unlock(a->mu);
    if (busy) {
        respond_err(rs, 200, "busy", "that game is being installed or is queued");
        return;
    }
    int confirm = 0;
    mp_json_bool(mp_json_get(in, "confirm"), &confirm);
    if (kind == JOB_UNINSTALL && !confirm) {
        respond_err(rs, 200, "needs_confirm", rec.dir);
        return;
    }
    job_t j;
    memset(&j, 0, sizeof j);
    j.kind = kind;
    snprintf(j.title, sizeof j.title, "%s", title);
    snprintf(j.name, sizeof j.name, "%s", rec.name);
    const char *err = enqueue(a, &j);
    if (err) respond_err(rs, 200, err, "that game is busy or the queue is full");
    else respond_ok(rs);
}

// Discard a half-done install: deletes its state and its temporary folder.
static void handle_discard(mp_api *a, const mp_json *in, mp_http_resp *rs) {
    const char *title = mp_json_str(mp_json_get(in, "title_id"));
    if (!title || !mp_inst_title_ok(title)) {
        respond_err(rs, 200, "not_found", "there is no half-done install of that game");
        return;
    }
    mp_mutex_lock(a->mu);
    int busy = title_busy(a, title);
    mp_mutex_unlock(a->mu);
    if (busy) {
        respond_err(rs, 200, "busy", "that game is being installed or is queued");
        return;
    }
    mp_err e = {{0}};
    mp_mutex_lock(a->run_mu);
    int rc = mp_inst_discard(&a->env, title, &e);
    mp_mutex_unlock(a->run_mu);
    if (rc != 0) respond_err(rs, 200, "not_found", e.msg);
    else respond_ok(rs);
}

typedef struct {
    uint8_t *buf;
    size_t n, cap;
} icon_buf;

static int icon_sink(void *ctx, const void *p, size_t n, mp_err *e) {
    icon_buf *b = ctx;
    if (b->n + n > b->cap) {
        mp_err_set(e, "icon too large");
        return -1;
    }
    memcpy(b->buf + b->n, p, n);
    b->n += n;
    return 0;
}

static void handle_icon(mp_api *a, const mp_http_req *rq, mp_http_resp *rs) {
    char path[1100];
    rs->status = 404;
    rs->content_type = "text/plain";
    if (mp_http_query_get(rq->query, "p5z", path, sizeof path) != 0 || !source_ok(a, path)) return;
    if (mp_path_kind(path) == 2) {   // game folder: its sce_sys/icon0.png
        char *icon = mpi_join(path, "sce_sys/icon0.png"), *buf = NULL;
        size_t n = 0;
        if (icon && mp_path_kind(icon) == 1 && mpi_read_small(icon, ICON_MAX, &buf, &n, NULL) == 0) {
            rs->status = 200;
            rs->content_type = "image/png";
            rs->body = buf;
            rs->body_len = n;
        }
        free(icon);
        return;
    }
    if (!mpi_has_ext(path, ".p5z")) return;
    mp_p5z *p = NULL;
    if (mp_p5z_open(path, &p, NULL) != 0) return;
    for (size_t i = 0; i < p->nfiles; i++) {
        const p5z_file *f = &p->files[i];
        if (f->role == MP_ROLE_ORIGINAL || strcmp(f->path, "sce_sys/icon0.png") != 0 || f->size > ICON_MAX) continue;
        icon_buf b = {malloc((size_t)f->size + 1), 0, (size_t)f->size};
        if (b.buf && p5z_read_file(p, i, icon_sink, &b, NULL) == 0) {
            rs->status = 200;
            rs->content_type = "image/png";
            rs->body = (char *)b.buf;
            rs->body_len = b.n;
        } else {
            free(b.buf);
        }
        break;
    }
    mp_p5z_close(p);
}

// Host must be an IP, [IPv6] or localhost (with a port or without): so that an arbitrary website
// cannot read the API with a name that points to the console (DNS rebinding).
static int host_ok(const char *h) {
    if (!h || !*h) return 0;
    if (h[0] == '[') return strchr(h, ']') != NULL;
    size_t n = strcspn(h, ":");
    if (n == 9 && !strncmp(h, "localhost", 9)) return 1;
    int dots = 0;
    for (size_t i = 0; i < n; i++) {
        if (h[i] == '.') dots++;
        else if (h[i] < '0' || h[i] > '9') return 0;
    }
    return dots == 3;
}

// A POST from another website without a preflight can only carry text/plain, form or multipart: requiring JSON
// makes the browser ask for permission first (and this server does not give it).
static int json_ok(const char *ct) {
    const char *want = "application/json";
    if (!ct) return 0;
    for (size_t i = 0; want[i]; i++) {
        char c = ct[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != want[i]) return 0;
    }
    return ct[16] == 0 || ct[16] == ';' || ct[16] == ' ';
}

void mp_api_handle(void *ctx, const mp_http_req *rq, mp_http_resp *rs) {
    mp_api *a = ctx;
    if (!strncmp(rq->path, "/api/", 5) &&
        (!host_ok(rq->host) || (!strcmp(rq->method, "POST") && !json_ok(rq->content_type)))) {
        respond_err(rs, 403, "forbidden", "request rejected: only MaxPack's page is accepted");
        return;
    }
    int get = !strcmp(rq->method, "GET"), post = !strcmp(rq->method, "POST");
    if (get && !strcmp(rq->path, "/api/state")) {
        handle_state(a, rs);
        return;
    }
    if (get && !strcmp(rq->path, "/api/icon")) {
        handle_icon(a, rq, rs);
        return;
    }
    if (!post || strncmp(rq->path, "/api/", 5) != 0) {
        respond_err(rs, 404, "not_found", rq->path);
        return;
    }
    if (!strcmp(rq->path, "/api/quit")) {
        // Only from this same console: another copy of MaxPack asks for the port to be left free.
        if (!rq->loopback) {
            respond_err(rs, 403, "forbidden", "only accepted from the console itself");
            return;
        }
        if (!a->quit_cb) {
            respond_err(rs, 403, "forbidden", "nobody to notify");
            return;
        }
        respond_ok(rs);
        a->quit_cb(a->quit_ctx);
        return;
    }
    mp_json *in = mp_json_parse(rq->body_len ? rq->body : "{}", rq->body_len ? rq->body_len : 2, NULL);
    if (!in) {
        respond_err(rs, 400, "bad_json", "the body is not valid JSON");
        return;
    }
    const char *r = rq->path + 5;
    if (!strcmp(r, "scan")) {
        rescan(a);
        respond_ok(rs);
    } else if (!strcmp(r, "probe")) {
        handle_probe(a, in, rs);
    } else if (!strcmp(r, "install")) {
        handle_install(a, in, rs);
    } else if (!strcmp(r, "pause")) {
        mp_mutex_lock(a->mu);
        if (a->busy) a->pause_req = 1;
        mp_mutex_unlock(a->mu);
        respond_ok(rs);
    } else if (!strcmp(r, "verify")) {
        handle_title_job(a, in, rs, JOB_VERIFY);
    } else if (!strcmp(r, "uninstall")) {
        handle_title_job(a, in, rs, JOB_UNINSTALL);
    } else if (!strcmp(r, "discard")) {
        handle_discard(a, in, rs);
    } else {
        respond_err(rs, 404, "not_found", rq->path);
    }
    mp_json_free(in);
}

// ---- vida -----------------------------------------------------------------------------------

mp_api *mp_api_new(const mp_inst_env *env, const mp_smp_cfg *smp, unsigned smp_retry_ms, mp_err *e) {
    mp_api *a = calloc(1, sizeof *a);
    if (!a) {
        mp_err_set(e, "out of memory");
        return NULL;
    }
    a->user = *env;
    a->env = *env;
    a->env.ctx = a;
    a->env.io_check = api_io;
    a->env.progress = api_progress;
    a->env.notify = api_notify;
    if (env->free_space) a->env.free_space = api_free_space;   // with the ctx of whoever created it
    a->smp = *smp;
    if (smp->fakelib_mode == smp->mode_buf) a->smp.fakelib_mode = a->smp.mode_buf;
    a->smp_retry_ms = smp_retry_ms ? smp_retry_ms : 60000;
    a->mu = mp_mutex_new();
    a->run_mu = mp_mutex_new();
    if (!a->mu || !a->run_mu) {
        mp_api_free(a);
        mp_err_set(e, "out of memory");
        return NULL;
    }
    mp_inst_recover(&a->env);   // a cut-off in the middle of a replace leaves the old one set aside
    rescan(a);
    if (mp_thread_start(&a->worker, worker, a) != 0 || mp_thread_start(&a->smp_thread, smp_loop, a) != 0) {
        mp_api_free(a);
        mp_err_set(e, "could not create the threads");
        return NULL;
    }
    return a;
}

int mp_api_wait_idle(mp_api *a, unsigned timeout_ms) {
    for (unsigned t = 0;; t += 10) {
        mp_mutex_lock(a->mu);
        int idle = !a->busy && a->qn == 0;
        mp_mutex_unlock(a->mu);
        if (idle) return 0;
        if (t >= timeout_ms) return -1;
        mp_sleep_ms(10);
    }
}

void mp_api_set_quit(mp_api *a, mp_api_quit_cb cb, void *ctx) {
    a->quit_cb = cb;
    a->quit_ctx = ctx;
}

void mp_api_free(mp_api *a) {
    if (!a) return;
    a->quit = 1;
    if (a->worker) mp_thread_join(a->worker);
    if (a->smp_thread) mp_thread_join(a->smp_thread);
    for (size_t i = 0; i < a->qn; i++) job_free(&a->queue[i]);
    mp_inst_found_free(a->found, a->nfound);
    mp_inst_existing_free(a->last_ex, a->last_nex);
    if (a->mu) mp_mutex_free(a->mu);
    if (a->run_mu) mp_mutex_free(a->run_mu);
    free(a);
}
