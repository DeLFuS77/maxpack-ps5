// SPDX-License-Identifier: GPL-3.0-or-later
// Measuring this PC. The generated data is a pool of 64 MiB that a source of its own shows as files
// of 8 MiB, as many as a trial needs, so mp_zpk_write packs it as it packs a game: the read-ahead
// reads and hashes, a pool compresses, this thread "writes" (into nothing). A short first run with
// the most threads tells how much each trial needs to last about trial_seconds. Then a temporary file
// in the folder is written (synced every 64 MiB) and read back past the system's cache, each for at
// most disk_seconds, and deleted.
#include "mp_bench.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_platform.h"
#include "mp_sample.h"
#include "mp_source_impl.h"
#include "mp_zpk.h"

#define POOL (64u << 20)
#define FILE_BYTES (8u << 20)
#define DISK_PIECE (4u << 20)
#define DISK_SYNC (64u << 20)
#define WORDS 5000

void mp_bench_opts_default(mp_bench_opts *o) {
    memset(o, 0, sizeof *o);
    o->level = 19;
    o->cpu_threads = mp_cpu_count();
    o->trial_seconds = 3;
    o->disk_seconds = 3;
    o->disk_max = 1ull << 30;
    o->progress = 1;
}

size_t mp_bench_counts(unsigned cpu, unsigned out[MP_BENCH_MAX_TRIALS]) {
    if (cpu == 0) cpu = 1;
    size_t n = 0;
    for (unsigned q = 1; q <= 4; q++) {
        unsigned c = (cpu * q + 3) / 4;
        if (n == 0 || out[n - 1] != c) out[n++] = c;
    }
    return n;
}

unsigned mp_bench_choose(const mp_bench_trial *t, size_t n) {
    uint64_t top = 0;
    for (size_t i = 0; i < n; i++)
        if (t[i].rate > top) top = t[i].rate;
    unsigned best = 0;
    for (size_t i = 0; i < n; i++)
        if ((double)t[i].rate >= (1.0 - MP_BENCH_TIE) * (double)top && (best == 0 || t[i].threads < best))
            best = t[i].threads;
    return best;
}

// ---- the data ------------------------------------------------------------------------------------

static uint64_t rnd(uint64_t *s) {
    *s += 0x9E3779B97F4A7C15ull;
    return mp_splitmix64(*s);
}
static double rnd01(uint64_t *s) { return (double)(rnd(s) >> 11) * (1.0 / 9007199254740992.0); }

static void noise(uint8_t *p, size_t n, uint64_t *s) {
    for (size_t i = 0; i < n; i += 8) {
        uint64_t v = rnd(s);
        memcpy(p + i, &v, n - i < 8 ? n - i : 8);
    }
}

// Words of 2 to 11 letters, the frequent ones much more so (as in real text), with spaces, commas,
// line ends and " = ".
static int make_text(uint8_t *t, size_t n, uint64_t *s) {
    char *vocab = malloc((size_t)WORDS * 12);
    double *cum = malloc(WORDS * sizeof *cum);
    if (!vocab || !cum) {
        free(vocab);
        free(cum);
        return -1;
    }
    unsigned char len[WORDS];
    double sum = 0;
    for (unsigned w = 0; w < WORDS; w++) {
        len[w] = (unsigned char)(2 + rnd(s) % 10);
        for (unsigned c = 0; c < len[w]; c++) vocab[w * 12 + c] = (char)('a' + rnd(s) % 26);
        sum += 1.0 / (w + 1);
        cum[w] = sum;
    }
    static const char *const seps[] = {" ", " ", " ", " ", ", ", ".\n", "\n", " = ", "; "};
    size_t at = 0;
    while (at < n) {
        double u = rnd01(s) * sum;
        unsigned lo = 0, hi = WORDS - 1;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            if (cum[mid] > u) hi = mid;
            else lo = mid + 1;
        }
        const char *sep = seps[rnd(s) % (sizeof seps / sizeof *seps)];
        for (unsigned c = 0; c < len[lo] && at < n; c++) t[at++] = (uint8_t)vocab[lo * 12 + c];
        for (const char *p = sep; *p && at < n; p++) t[at++] = (uint8_t)*p;
    }
    free(vocab);
    free(cum);
    return 0;
}

// Vertex-like records of 8 floats: a position that wanders, a normal, texture coordinates, an index.
static void make_records(uint8_t *t, size_t n, uint64_t *s) {
    float x = 0, y = 0, z = 0;
    size_t i = 0;
    for (; (i + 1) * 32 <= n; i++) {
        x += (float)(rnd01(s) * 0.02 - 0.01);
        y += (float)(rnd01(s) * 0.02 - 0.01);
        z += (float)(rnd01(s) * 0.02 - 0.01);
        float rec[8] = {x, y, z, (float)(rnd01(s) * 2 - 1), (float)(rnd01(s) * 2 - 1), (float)(i % 997) / 997.0f,
                        (float)rnd01(s), (float)(i & 0xFFFF)};
        memcpy(t + i * 32, rec, 32);
    }
    memset(t + i * 32, 0, n - i * 32);
}

static void from_pool(uint8_t *p, size_t n, const uint8_t *pool, size_t pn, uint64_t *s) {
    while (n) {
        size_t off = (size_t)(rnd(s) % (pn - 4096)), k = pn - off < n ? pn - off : n;
        memcpy(p, pool + off, k);
        p += k;
        n -= k;
    }
}

void mp_bench_fill(uint8_t *buf, size_t n, uint64_t seed) {
    uint64_t s = seed * 0x2545F4914F6CDD1Dull + 1;
    size_t pn = n < (8u << 20) ? (n > 65536 ? n : 65536) : (8u << 20);
    uint8_t *text = malloc(pn), *rec = malloc(pn);
    if (text && make_text(text, pn, &s) != 0) {
        free(text);
        text = NULL;
    }
    if (rec) make_records(rec, pn, &s);
    for (size_t at = 0; at < n;) {
        size_t len = (16u << 10) + (size_t)(rnd(&s) % ((2u << 20) - (16u << 10) + 1));
        if (len > n - at) len = n - at;
        unsigned kind = (unsigned)(rnd(&s) % 100);
        if (kind >= 90) memset(buf + at, 0, len);
        else if (kind >= 65 && rec) from_pool(buf + at, len, rec, pn, &s);
        else if (kind >= 40 && kind < 65 && text) from_pool(buf + at, len, text, pn, &s);
        else noise(buf + at, len, &s);
        at += len;
    }
    free(text);
    free(rec);
}

// ---- packing it ----------------------------------------------------------------------------------

// File k of the source, at off, is the pool from (k * 8 MiB + off) on, round and round.
static int bench_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e) {
    (void)e;
    const uint8_t *pool = s->impl;
    uint64_t at = ((uint64_t)i * FILE_BYTES + off) % POOL;
    uint8_t *out = buf;
    while (len) {
        size_t k = POOL - at < len ? (size_t)(POOL - at) : len;
        memcpy(out, pool + at, k);
        out += k;
        len -= k;
        at = 0;
    }
    return 0;
}

static void bench_close(mp_source *s) { (void)s; }   // the pool is the caller's

static int null_vol(void *ctx, uint32_t vol, mp_err *e) {
    (void)ctx;
    (void)vol;
    (void)e;
    return 0;
}
static int null_write(void *ctx, const void *p, size_t n, mp_err *e) {
    (void)ctx;
    (void)p;
    (void)n;
    (void)e;
    return 0;
}
static int null_header(void *ctx, uint32_t vol, const uint8_t hdr[64], mp_err *e) {
    (void)ctx;
    (void)vol;
    (void)hdr;
    (void)e;
    return 0;
}

// Packs bytes of the pool as a game, with threads, into nothing: *ns is how long it took.
static int pack_once(const uint8_t *pool, uint64_t bytes, unsigned threads, int level, mp_progress *prog,
                     uint64_t *ns, mp_err *e) {
    size_t n = (size_t)((bytes + FILE_BYTES - 1) / FILE_BYTES);
    mp_source *s = calloc(1, sizeof *s);
    mp_entry *en = calloc(n ? n : 1, sizeof *en);
    uint32_t *ids = malloc((n ? n : 1) * sizeof *ids);
    uint8_t (*sha)[32] = malloc((n ? n : 1) * sizeof *sha);
    int rc = -1;
    if (!s || !en || !ids || !sha) {
        free(s);
        free(en);
        s = NULL;
        goto oom;
    }
    s->kind = MP_SRC_DIR;
    s->entries = en;
    s->read = bench_read;
    s->close = bench_close;
    s->impl = (void *)pool;
    for (size_t k = 0; k < n; k++) {
        char name[32];
        snprintf(name, sizeof name, "b/%010zu", k);
        if (!(en[k].path = mp_strdup(name))) goto oom;
        en[k].size = k + 1 < n ? FILE_BYTES : bytes - (uint64_t)k * FILE_BYTES;
        s->n = k + 1;
        ids[k] = (uint32_t)k;
    }
    mp_zpk_opts zo = {level, threads, MP_ZPK_VOLUME_LIMIT, prog};
    mp_zpk_sink sink = {null_vol, null_write, null_vol, null_header, NULL};
    mp_zpk_stats st;
    uint8_t *idx = NULL;
    size_t idx_len = 0;
    uint64_t t0 = mp_now_ns();
    rc = mp_zpk_write(s, ids, n, &zo, &sink, &idx, &idx_len, sha, &st, e);
    *ns = mp_now_ns() - t0;
    free(idx);
    goto done;
oom:
    mp_err_set(e, "out of memory");
done:
    mp_source_close(s);
    free(ids);
    free(sha);
    return rc;
}

static uint64_t per_second(uint64_t bytes, uint64_t ns) {
    return ns ? (uint64_t)llround((double)bytes * 1e9 / (double)ns) : 0;
}

// ---- the disk ------------------------------------------------------------------------------------

typedef struct {
    mp_progress *prog;
    uint64_t added;
} counter;

static void count(counter *c, uint64_t n) {
    c->added += n;
    mp_progress_add(c->prog, n);
    mp_progress_maybe_print(c->prog);
}

static void remove_temp(const char *path) {
    for (int k = 0; k < 40 && mp_path_kind(path) != 0; k++) {
        if (mp_remove_file(path) == 0) break;
        mp_sleep_ms(50);   // a scanner of the system may still hold it a moment
    }
}

// Writes up to planned bytes to a new temporary file in dir and reads them back, each for at most
// secs; the file goes in every case. 0 measured; -1 not (why in r->disk_error), also when cancelled.
static int disk_run(const char *dir, uint64_t planned, double secs, counter *c, mp_bench_report *r) {
    mp_err e = {{0}};
    size_t cap = strlen(dir) + 48;
    char *path = malloc(cap);
    uint8_t *raw = malloc(DISK_PIECE + 4096);
    mp_wfile *w = NULL;
    mp_file *f = NULL;
    int rc = -1, made = 0;
    if (!path || !raw) {
        mp_err_set(&e, "out of memory");
        goto done;
    }
    unsigned k = 0;
    for (; k < 100; k++) {
        if (k) snprintf(path, cap, "%s/.maxpack-measure-%u.tmp", dir, k);
        else snprintf(path, cap, "%s/.maxpack-measure.tmp", dir);
        if (mp_path_kind(path) == 0) break;
    }
    if (k == 100) {
        mp_err_set(&e, "no free name for a temporary file in %s", dir);
        goto done;
    }
    uint8_t *buf = raw + (4096 - ((uintptr_t)raw & 4095)) % 4096;   // aligned, to read past the cache
    uint64_t s = 0x6D6561737572ull;
    noise(buf, DISK_PIECE, &s);   // incompressible, should the disk compress what it stores
    if (mp_wfile_create(path, &w, &e) != 0) goto done;
    made = 1;
    mp_wfile_uncached(w);
    uint64_t limit = (uint64_t)(secs * 1e9), t0 = mp_now_ns(), written = 0;
    while (written < planned) {
        if (mp_cancel_check(&e) != 0 || mp_wfile_write(w, buf, DISK_PIECE, &e) != 0) goto done;
        written += DISK_PIECE;
        count(c, DISK_PIECE);
        if (written % DISK_SYNC == 0 && mp_wfile_sync(w, &e) != 0) goto done;
        if (mp_now_ns() - t0 >= limit) break;
    }
    if (mp_wfile_sync(w, &e) != 0) goto done;
    uint64_t wns = mp_now_ns() - t0;
    mp_wfile *closing = w;
    w = NULL;
    if (mp_wfile_close(closing, &e) != 0) goto done;
    count(c, planned - written);
    r->written = written;
    r->write_rate = per_second(written, wns);
    if (mp_file_open_read_uncached(path, &f, &e) != 0) goto done;
    uint64_t done_read = 0;
    t0 = mp_now_ns();
    while (done_read < written) {
        size_t got = 0;
        if (mp_cancel_check(&e) != 0 || mp_file_pread(f, buf, DISK_PIECE, done_read, &got, &e) != 0) goto done;
        if (got != DISK_PIECE) {
            mp_err_set(&e, "%s was read short", path);
            goto done;
        }
        done_read += DISK_PIECE;
        count(c, DISK_PIECE);
        if (mp_now_ns() - t0 >= limit) break;
    }
    uint64_t rns = mp_now_ns() - t0;
    count(c, planned - done_read);
    r->read = done_read;
    r->read_rate = per_second(done_read, rns);
    rc = 0;
done:
    if (w) mp_wfile_close(w, NULL);
    mp_file_close(f);
    if (made) {
        remove_temp(path);
        if (mp_path_kind(path) != 0 && rc == 0) {
            mp_err_set(&e, "could not delete the temporary file %s", path);
            rc = -1;
        }
    }
    if (rc != 0) snprintf(r->disk_error, sizeof r->disk_error, "%s", e.msg);
    free(path);
    free(raw);
    return rc;
}

// ---- measuring -----------------------------------------------------------------------------------

static uint64_t round_up(uint64_t v, uint64_t to) { return (v + to - 1) / to * to; }

int mp_bench_run(const mp_bench_opts *o, mp_bench_report *r, mp_err *e) {
    memset(r, 0, sizeof *r);
    uint64_t t0 = mp_now_ns();
    r->cpu_threads = o->cpu_threads ? o->cpu_threads : 1;
    r->level = o->level;
    if (o->level < 1 || o->level > 22) {
        mp_err_set(e, "zstd level outside 1..22: %d", o->level);
        return -1;
    }
    unsigned counts[MP_BENCH_MAX_TRIALS];
    size_t nc = o->ncounts;
    if (nc > MP_BENCH_MAX_TRIALS) nc = MP_BENCH_MAX_TRIALS;
    if (nc) memcpy(counts, o->counts, nc * sizeof *counts);
    else nc = mp_bench_counts(r->cpu_threads, counts);
    for (size_t i = 0; i < nc; i++)
        if (counts[i] < 1 || counts[i] > 1024 || (i && counts[i] <= counts[i - 1])) {
            mp_err_set(e, "the thread counts go from 1 to 1024, in increasing order");
            return -1;
        }
    if (mp_cancel_check(e) != 0) return -1;

    // the disk: how much it can take, before anything is measured
    uint64_t planned = 0;
    if (o->dir) {
        uint64_t free_bytes = 0;
        mp_err de = {{0}};
        r->disk = -1;
        if (mp_path_kind(o->dir) != 2) {
            snprintf(r->disk_error, sizeof r->disk_error, "not a folder: %s", o->dir);
        } else if (mp_disk_free(o->dir, &free_bytes, &de) != 0) {
            snprintf(r->disk_error, sizeof r->disk_error, "%s", de.msg);
        } else {
            planned = (o->disk_max < free_bytes / 4 ? o->disk_max : free_bytes / 4) / DISK_PIECE * DISK_PIECE;
            if (planned < 4 * DISK_PIECE) {
                planned = 0;
                snprintf(r->disk_error, sizeof r->disk_error, "not enough free space in %s to measure it", o->dir);
            } else {
                r->disk = 0;   // to be measured
            }
        }
    }

    uint8_t *pool = malloc(POOL);
    if (!pool) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    mp_bench_fill(pool, POOL, 1);
    int rc = -1, prog_ok = 0;
    mp_progress prog;
    unsigned top = counts[nc - 1];
    // how fast the most threads go, on a run long enough to tell: the trials are sized from it
    uint64_t cbytes = round_up((uint64_t)top * 8 * MP_BLOCK_V1 > (8u << 20) ? (uint64_t)top * 8 * MP_BLOCK_V1 : (8u << 20),
                               MP_BLOCK_V1);
    uint64_t target = (uint64_t)(o->trial_seconds * 0.25 * 1e9), cns = 0;
    for (int k = 0; k < 6; k++) {
        if (pack_once(pool, cbytes, top, o->level, NULL, &cns, e) != 0) goto done;
        if (cns >= target || cbytes >= (4ull << 30)) break;
        double grow = cns ? (double)target / (double)cns * 1.25 : 16;
        if (grow < 2) grow = 2;
        if (grow > 16) grow = 16;
        cbytes = round_up((uint64_t)((double)cbytes * grow), MP_BLOCK_V1);
    }
    double top_rate = cns ? (double)cbytes * 1e9 / (double)cns : 1e9;
    uint64_t bytes[MP_BENCH_MAX_TRIALS], total = 0;
    for (size_t i = 0; i < nc; i++) {
        double want = top_rate * counts[i] / top * o->trial_seconds;
        uint64_t b = (uint64_t)want, least = (uint64_t)counts[i] * 8 * MP_BLOCK_V1;
        if (b < least) b = least;
        if (b > (32ull << 30)) b = 32ull << 30;
        bytes[i] = round_up(b, MP_BLOCK_V1);
        total += bytes[i];
    }
    mp_progress_init(&prog, "Measuring", total + 2 * planned, o->progress);
    prog_ok = 1;
    for (size_t i = 0; i < nc; i++) {
        mp_bench_trial *t = &r->trials[i];
        t->threads = counts[i];
        t->bytes = bytes[i];
        if (pack_once(pool, bytes[i], counts[i], o->level, &prog, &t->ns, e) != 0) goto done;
        t->rate = per_second(t->bytes, t->ns);
        r->ntrials = i + 1;
        if (o->on_trial) o->on_trial(o->ctx, t);
    }
    r->best = mp_bench_choose(r->trials, r->ntrials);
    uint64_t fastest_rate = 0;
    for (size_t i = 0; i < r->ntrials; i++)
        if (r->trials[i].rate > fastest_rate) {
            fastest_rate = r->trials[i].rate;
            r->fastest = r->trials[i].threads;
        }
    if (planned) {
        counter c = {&prog, 0};
        int drc = disk_run(o->dir, planned, o->disk_seconds, &c, r);
        if (c.added < 2 * planned) mp_progress_add(&prog, 2 * planned - c.added);
        if (mp_cancel_check(e) != 0) goto done;
        r->disk = drc == 0 ? 1 : -1;
    }
    if (o->progress) fprintf(stderr, "\rMeasuring: 100.0 %%\n");
    rc = 0;
done:
    if (prog_ok) mp_progress_free(&prog);
    free(pool);
    r->elapsed_ns = mp_now_ns() - t0;
    return rc;
}
