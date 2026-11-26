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
    mp_err_set(e, "cancelled");
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
        if (!p->err && p->next < p->n && mp_cancelled()) p->err = -1;   // cancelled: no more is handed out
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
    if (started == 0) {   // no threads: everything is done in this one
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

// ---- Ordered pipeline ------------------------------------------------------------------------

enum { SLOT_FREE, SLOT_QUEUED, SLOT_RUNNING, SLOT_DONE };

struct mp_pipe {
    mp_mutex *mu;
    mp_cond *work_cv;   // the threads wait: a slot was submitted, or stop
    mp_cond *done_cv;   // the consumer waits: the oldest slot finished, closed or aborted
    mp_cond *free_cv;   // the producer waits: a slot was released, or aborted
    size_t nslots;
    // Sequence numbers (slot = number % nslots): submitted, handed to a thread, released.
    uint64_t submitted, taken, consumed;
    unsigned char *state;
    int *rc;
    int closed, aborted, stop;
    unsigned started, tids;
    mp_thread **t;
    int (*work)(void *, size_t, unsigned);
    void *ctx;
};

static void pipe_worker(void *arg) {
    mp_pipe *p = arg;
    mp_mutex_lock(p->mu);
    unsigned tid = p->tids++;
    for (;;) {
        while (!p->stop && !p->aborted && p->taken == p->submitted) mp_cond_wait(p->work_cv, p->mu);
        if (p->stop || p->aborted) break;
        size_t s = (size_t)(p->taken++ % p->nslots);
        if (p->state[s] != SLOT_QUEUED) continue;   // it needed no work (or another thread has it)
        p->state[s] = SLOT_RUNNING;
        mp_mutex_unlock(p->mu);
        int rc = mp_cancelled() ? -1 : p->work(p->ctx, s, tid);
        mp_mutex_lock(p->mu);
        p->rc[s] = rc;
        p->state[s] = SLOT_DONE;
        if (s == (size_t)(p->consumed % p->nslots)) mp_cond_signal(p->done_cv);
    }
    mp_mutex_unlock(p->mu);
}

mp_pipe *mp_pipe_new(unsigned threads, size_t nslots, int (*work)(void *ctx, size_t slot, unsigned tid),
                     void *ctx) {
    if (nslots == 0) return NULL;
    if (threads == 0) threads = 1;
    mp_pipe *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->nslots = nslots;
    p->work = work;
    p->ctx = ctx;
    p->mu = mp_mutex_new();
    p->work_cv = mp_cond_new();
    p->done_cv = mp_cond_new();
    p->free_cv = mp_cond_new();
    p->state = calloc(nslots, 1);
    p->rc = calloc(nslots, sizeof *p->rc);
    p->t = calloc(threads, sizeof *p->t);
    if (!p->mu || !p->work_cv || !p->done_cv || !p->free_cv || !p->state || !p->rc || !p->t) {
        mp_pipe_free(p);
        return NULL;
    }
    for (unsigned i = 0; i < threads; i++)
        if (mp_thread_start(&p->t[p->started], pipe_worker, p) == 0) p->started++;
    return p;
}

long mp_pipe_acquire(mp_pipe *p) {
    mp_mutex_lock(p->mu);
    while (!p->aborted && p->submitted - p->consumed >= p->nslots) mp_cond_wait(p->free_cv, p->mu);
    long s = p->aborted ? -1 : (long)(p->submitted % p->nslots);
    mp_mutex_unlock(p->mu);
    return s;
}

void mp_pipe_submit(mp_pipe *p, size_t slot, int run) {
    int rc = 0, queued = run && p->started;
    if (run && !p->started) rc = mp_cancelled() ? -1 : p->work(p->ctx, slot, 0);   // no threads: here
    mp_mutex_lock(p->mu);
    p->rc[slot] = rc;
    p->state[slot] = queued ? SLOT_QUEUED : SLOT_DONE;
    p->submitted++;
    if (queued) mp_cond_signal(p->work_cv);
    else if (slot == (size_t)(p->consumed % p->nslots)) mp_cond_signal(p->done_cv);
    mp_mutex_unlock(p->mu);
}

void mp_pipe_close(mp_pipe *p) {
    mp_mutex_lock(p->mu);
    p->closed = 1;
    mp_cond_signal(p->done_cv);
    mp_mutex_unlock(p->mu);
}

long mp_pipe_next(mp_pipe *p, int wait, int *rc) {
    long s = -1;
    mp_mutex_lock(p->mu);
    for (;;) {
        if (p->aborted) break;
        size_t k = (size_t)(p->consumed % p->nslots);
        if (p->consumed < p->submitted && p->state[k] == SLOT_DONE) {
            s = (long)k;
            if (rc) *rc = p->rc[k];
            break;
        }
        if (!wait || (p->closed && p->consumed == p->submitted)) break;
        mp_cond_wait(p->done_cv, p->mu);
    }
    mp_mutex_unlock(p->mu);
    return s;
}

void mp_pipe_release(mp_pipe *p, size_t slot) {
    mp_mutex_lock(p->mu);
    p->state[slot] = SLOT_FREE;
    p->consumed++;
    mp_cond_signal(p->free_cv);
    mp_mutex_unlock(p->mu);
}

size_t mp_pipe_pending(mp_pipe *p) {
    mp_mutex_lock(p->mu);
    size_t n = (size_t)(p->submitted - p->consumed);
    mp_mutex_unlock(p->mu);
    return n;
}

void mp_pipe_abort(mp_pipe *p) {
    mp_mutex_lock(p->mu);
    p->aborted = 1;
    mp_cond_broadcast(p->work_cv);
    mp_cond_broadcast(p->done_cv);
    mp_cond_broadcast(p->free_cv);
    mp_mutex_unlock(p->mu);
}

void mp_pipe_free(mp_pipe *p) {
    if (!p) return;
    if (p->mu) {
        mp_mutex_lock(p->mu);
        p->stop = 1;
        if (p->work_cv) mp_cond_broadcast(p->work_cv);
        mp_mutex_unlock(p->mu);
    }
    for (unsigned i = 0; i < p->started; i++) mp_thread_join(p->t[i]);
    free(p->t);
    free(p->state);
    free(p->rc);
    mp_cond_free(p->work_cv);
    mp_cond_free(p->done_cv);
    mp_cond_free(p->free_cv);
    mp_mutex_free(p->mu);
    free(p);
}
