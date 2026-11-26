// SPDX-License-Identifier: GPL-3.0-or-later
// Distribution of items among threads and progress on stderr.
#ifndef MP_PARALLEL_H
#define MP_PARALLEL_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"
#include "mp_platform.h"

// Progress hook: mp_progress_add calls it at most every 100 ms and always when the
// phase ends (done == total). It is called with the phase's lock held, so no two calls happen at once.
typedef struct {
    void (*cb)(void *ctx, const char *phase, uint64_t done, uint64_t total);
    void *ctx;
} mp_progress_hook;

// One hook per process (NULL removes it). mp_progress_init copies it: set it before starting.
void mp_progress_set_hook(const mp_progress_hook *h);

// Cancellation: atomic flag, can be requested from any thread. mp_parallel_run stops handing out
// and pack, unpack and verify exit with -1 and the message "cancelled".
void mp_cancel_request(void);
int mp_cancelled(void);
void mp_cancel_reset(void);
// If a cancellation is pending, puts "cancelled" in e and returns -1; otherwise 0.
int mp_cancel_check(mp_err *e);

typedef struct mp_progress {
    mp_mutex *mu;
    uint64_t done, total;
    const char *phase;
    int print;
    uint64_t last_ns;   // last time it was printed (mp_progress_maybe_print)
    mp_progress_hook hook;
    uint64_t hook_ns;   // last call to the hook
    int hook_final;     // done == total has already been reported
} mp_progress;

void mp_progress_init(mp_progress *p, const char *phase, uint64_t total, int print);
void mp_progress_add(mp_progress *p, uint64_t n);   // p may be NULL
void mp_progress_free(mp_progress *p);
// For loops that do not use mp_parallel_run for everything: prints at most once per second.
void mp_progress_maybe_print(mp_progress *p);

// Processes items 0..n-1 with `threads` threads (tid in [0, threads)). If a work returns != 0,
// items stop being handed out and the first code is returned. With mp_cancelled() it also stops
// handing out and returns -1 if items were left undistributed. If prog->print, the main thread
// writes "\r<phase>: NN %" on stderr every second and at the end.
int mp_parallel_run(unsigned threads, size_t n, int (*work)(void *ctx, size_t item, unsigned tid),
                    void *ctx, mp_progress *prog);

// ---- Ordered pipeline ------------------------------------------------------------------------
// A ring of nslots slots that one producer fills in order, a pool of threads that lives as long as
// the pipe processes in any order, and one consumer takes back in the order they were filled. So
// reading the next items, processing the current ones and writing the previous ones overlap, and the
// output does not depend on the number of threads. The producer and the consumer may be two threads
// or the same one (then it must not wait for a free slot or for the next one with nothing pending).
typedef struct mp_pipe mp_pipe;

// work(ctx, slot, tid) runs on the pool's threads (tid in [0, threads)) and its return is the slot's
// code. NULL if it cannot be created. If no thread can be started, work runs inside mp_pipe_submit.
mp_pipe *mp_pipe_new(unsigned threads, size_t nslots, int (*work)(void *ctx, size_t slot, unsigned tid),
                     void *ctx);
// Producer: the next slot to fill (one at a time); waits while all of them are in use. -1 if aborted.
long mp_pipe_acquire(mp_pipe *p);
// Producer: the slot that mp_pipe_acquire gave is ready. With run = 0 it needs no work and goes
// straight to the consumer with code 0. A slot that would start after mp_cancelled() is not passed
// to work: its code is -1.
void mp_pipe_submit(mp_pipe *p, size_t slot, int run);
// Producer: no more slots will come.
void mp_pipe_close(mp_pipe *p);
// Consumer: the oldest slot submitted, once processed, with its code in *rc. With wait = 0 it does not
// wait: -1 if that slot is not ready (or there is none). With wait = 1, -1 means closed and every slot
// already taken, or aborted.
long mp_pipe_next(mp_pipe *p, int wait, int *rc);
// Consumer: gives back the slot that mp_pipe_next returned, for the producer to fill again.
void mp_pipe_release(mp_pipe *p, size_t slot);
// Slots submitted and not yet released.
size_t mp_pipe_pending(mp_pipe *p);
// Stops everything: the threads take no more slots and mp_pipe_acquire and mp_pipe_next return -1.
void mp_pipe_abort(mp_pipe *p);
// Waits for the slots being processed, joins the threads and frees the pipe (p may be NULL).
void mp_pipe_free(mp_pipe *p);

#endif
