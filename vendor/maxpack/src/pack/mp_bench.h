// SPDX-License-Identifier: GPL-3.0-or-later
// maxpack measure: how fast this PC packs, with no game. Generated data (pieces of noise, text,
// structured records and zeros, as a game has) is packed by the pack's own pipeline (mp_zpk_write)
// into nothing at a few thread counts, and the disk of a folder (the destination) is measured by
// writing a temporary file there and reading it back. That file is the only thing written, and it is
// deleted.
#ifndef MP_BENCH_H
#define MP_BENCH_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

#define MP_BENCH_MAX_TRIALS 8
#define MP_BENCH_TIE 0.05   // a count less than 5 % slower than the fastest is as good

typedef struct {
    unsigned threads;
    uint64_t bytes, ns;
    uint64_t rate;   // bytes a second
} mp_bench_trial;

typedef struct {
    int level;                              // 19
    const char *dir;                        // the folder whose disk is measured, or NULL
    unsigned cpu_threads;                   // mp_cpu_count()
    unsigned counts[MP_BENCH_MAX_TRIALS];   // the thread counts to try, increasing; with ncounts 0,
    size_t ncounts;                         // 25, 50, 75 and 100 % of cpu_threads
    double trial_seconds;                   // about how long each count is packing (3)
    double disk_seconds;                    // the most the disk is written, and then read (3 each)
    uint64_t disk_max;                      // the most written (1 GiB)
    int progress;                           // progress on stderr
    void (*on_trial)(void *ctx, const mp_bench_trial *t);   // each count once tried (may be NULL)
    void *ctx;
} mp_bench_opts;

typedef struct {
    unsigned cpu_threads;
    int level;
    size_t ntrials;
    mp_bench_trial trials[MP_BENCH_MAX_TRIALS];
    unsigned best;      // the threads to use (mp_bench_choose)
    unsigned fastest;   // the count that went fastest
    int disk;           // 1 measured, 0 no folder given, -1 it could not be (disk_error says why)
    char disk_error[256];
    uint64_t written, write_rate, read, read_rate;   // bytes, and bytes a second
    uint64_t elapsed_ns;
} mp_bench_report;

void mp_bench_opts_default(mp_bench_opts *o);
// 25, 50, 75 and 100 % of cpu threads (rounded up, each once): 8, 16, 24 and 32 of 32.
size_t mp_bench_counts(unsigned cpu, unsigned out[MP_BENCH_MAX_TRIALS]);
// The fewest threads less than MP_BENCH_TIE slower than the fastest of the trials (0 with none).
unsigned mp_bench_choose(const mp_bench_trial *t, size_t n);
// n bytes like a game's, the same for each seed: pieces of 16 KiB to 2 MiB, 40 % noise, 25 % text,
// 25 % vertex-like records and 10 % zeros (the mix of tools/bench_pack.py).
void mp_bench_fill(uint8_t *buf, size_t n, uint64_t seed);
// 0 with the report (a disk that could not be measured is in it); -1 on a cancel or an error of the
// CPU's part.
int mp_bench_run(const mp_bench_opts *o, mp_bench_report *r, mp_err *e);

#endif
