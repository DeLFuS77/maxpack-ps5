// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_parallel.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define HOOK_INTERVAL_NS 100000000ull

static mp_progress_hook g_hook;
static volatile long g_cancel;

void mp_progress_set_hook(const mp_progress_hook *h) {
    if (h) g_hook = *h;
    else g_hook = (mp_progress_hook){NULL, NULL};
}

#ifdef _WIN32
void mp_cancel_request(void) { InterlockedExchange(&g_cancel, 1); }
int mp_cancelled(void) { return InterlockedCompareExchange(&g_cancel, 0, 0) != 0; }
void mp_cancel_reset(void) { InterlockedExchange(&g_cancel, 0); }
#else
void mp_cancel_request(void) { __atomic_store_n(&g_cancel, 1, __ATOMIC_SEQ_CST); }
int mp_cancelled(void) { return __atomic_load_n(&g_cancel, __ATOMIC_SEQ_CST) != 0; }
void mp_cancel_reset(void) { __atomic_store_n(&g_cancel, 0, __ATOMIC_SEQ_CST); }
#endif

int mp_cancel_check(mp_err *e) {
    if (!mp_cancelled()) return 0;
    mp_err_set(e, "cancelado");
    return -1;
}

void mp_progress_init(mp_progress *p, const char *phase, uint64_t total, int print) {
    p->mu = mp_mutex_new();
    p->done = 0;
    p->total = total;
    p->phase = phase;
    p->print = print;
    p->last_ns = 0;
    p->hook = g_hook;
    p->hook_ns = 0;
    p->hook_final = 0;
}

void mp_progress_maybe_print(mp_progress *p) {
    if (!p || !p->print || !p->mu) return;
    uint64_t now = mp_now_ns();
    if (now - p->last_ns < 1000000000ull) return;
    p->last_ns = now;
    mp_mutex_lock(p->mu);
    double pct = p->total ? 100.0 * (double)p->done / (double)p->total : 100.0;
    mp_mutex_unlock(p->mu);
    fprintf(stderr, "\r%s: %5.1f %%", p->phase, pct);
    fflush(stderr);
}

void mp_progress_add(mp_progress *p, uint64_t n) {
    if (!p || !p->mu) return;
    mp_mutex_lock(p->mu);
    p->done += n;
    if (p->hook.cb) {
        int final = p->done >= p->total && !p->hook_final;
        uint64_t now = mp_now_ns();
        if (final || !p->hook_ns || now - p->hook_ns >= HOOK_INTERVAL_NS) {
            if (final) p->hook_final = 1;
            p->hook_ns = now ? now : 1;
            p->hook.cb(p->hook.ctx, p->phase, p->done, p->total);
        }
    }
    mp_mutex_unlock(p->mu);
}

void mp_progress_free(mp_progress *p) {
    if (!p) return;
    mp_mutex_free(p->mu);
    p->mu = NULL;
}

typedef struct {
    mp_mutex *mu;
    size_t next, n;
    unsigned tids, finished;
    int err;
    int (*work)(void *, size_t, unsigned);
    void *ctx;
} pool;

static void worker(void *arg) {
    pool *p = arg;
    mp_mutex_lock(p->mu);
    unsigned tid = p->tids++;
    mp_mutex_unlock(p->mu);
    for (;;) {
        mp_mutex_lock(p->mu);
        if (!p->err && p->next < p->n && mp_cancelled()) p->err = -1;   // cancelado: no se reparte más
        if (p->err || p->next >= p->n) {
            mp_mutex_unlock(p->mu);
            break;
        }
        size_t item = p->next++;
        mp_mutex_unlock(p->mu);
        int rc = p->work(p->ctx, item, tid);
        if (rc) {
            mp_mutex_lock(p->mu);
            if (!p->err) p->err = rc;
            mp_mutex_unlock(p->mu);
        }
    }
    mp_mutex_lock(p->mu);
    p->finished++;
    mp_mutex_unlock(p->mu);
}

static void print_progress(mp_progress *prog, int final) {
    mp_mutex_lock(prog->mu);
    double pct = prog->total ? 100.0 * (double)prog->done / (double)prog->total : 100.0;
    mp_mutex_unlock(prog->mu);
    if (final) pct = 100.0;
    fprintf(stderr, "\r%s: %5.1f %%%s", prog->phase, pct, final ? "\n" : "");
    fflush(stderr);
}

int mp_parallel_run(unsigned threads, size_t n, int (*work)(void *ctx, size_t item, unsigned tid),
                    void *ctx, mp_progress *prog) {
    if (n == 0) return 0;
    if (threads == 0) threads = 1;
    if (threads > n) threads = (unsigned)n;
    pool p = {mp_mutex_new(), 0, n, 0, 0, 0, work, ctx};
    if (!p.mu) return -1;
    mp_thread **t = calloc(threads, sizeof *t);
    unsigned started = 0;
    for (unsigned i = 0; t && i < threads; i++)
        if (mp_thread_start(&t[i], worker, &p) == 0) started++;
    if (started == 0) {   // sin hilos: se hace todo en este
        worker(&p);
    } else {
        int print = prog && prog->print && prog->mu;
        uint64_t last = mp_now_ns();
        for (;;) {
            mp_mutex_lock(p.mu);
            int done = p.finished == started;
            mp_mutex_unlock(p.mu);
            if (done) break;
            mp_sleep_ms(50);
            if (print && mp_now_ns() - last >= 1000000000ull) {
                print_progress(prog, 0);
                last = mp_now_ns();
            }
        }
        for (unsigned i = 0; i < threads; i++)
            if (t[i]) mp_thread_join(t[i]);
    }
    if (prog && prog->print && prog->mu && !p.err) print_progress(prog, 1);
    free(t);
    int err = p.err;
    mp_mutex_free(p.mu);
    return err;
}
