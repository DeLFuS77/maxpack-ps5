// SPDX-License-Identifier: GPL-3.0-or-later
// Comparing levels: the analyzer's sample of the packable data, seen as a game of its own (each
// 1 MiB window a file), is packed by mp_zpk_write at each level into nothing, and what each window
// took is read back from the index it makes. The bytes are the ones a pack of the whole game would
// give for those blocks, so the estimate is the analyzer's (same windows, same strata), and the time
// is the one of the pack's own pipeline on this PC.
#include "mp_levels.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_analyze.h"
#include "mp_platform.h"
#include "mp_readahead.h"
#include "mp_rules.h"
#include "mp_sample.h"
#include "mp_source_impl.h"
#include "mp_zpk.h"

static const int default_levels[] = {3, 9, 15, 19};

void mp_levels_opts_default(mp_levels_opts *o) {
    memset(o, 0, sizeof *o);
    memcpy(o->levels, default_levels, sizeof default_levels);
    o->nlevels = sizeof default_levels / sizeof *default_levels;
    o->sample_fraction = 0.02;
    o->sample_min = 1ull << 30;
    o->loose_min = 256ull << 20;
    o->threads = mp_cpu_count();
    o->progress = 1;
}

int mp_levels_parse(const char *list, mp_levels_opts *o, mp_err *e) {
    int want[MP_LEVELS_MAX + 1] = {0};
    const char *p = list;
    if (!p || !*p) goto bad;
    for (;;) {
        if (*p < '0' || *p > '9') goto bad;
        int v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p++ - '0');
            if (v > MP_LEVELS_MAX) goto bad;
        }
        if (v < 1) goto bad;
        want[v] = 1;
        if (!*p) break;
        if (*p++ != ',') goto bad;
    }
    o->nlevels = 0;
    for (int l = 1; l <= MP_LEVELS_MAX; l++)
        if (want[l]) o->levels[o->nlevels++] = l;
    return 0;
bad:
    mp_err_set(e, "the levels are numbers from 1 to 22 separated by commas, such as 3,9,15,19: %s",
               list ? list : "");
    return -1;
}

// ---- the sample as a game -------------------------------------------------------------------

typedef struct {
    mp_source *src;
    const uint32_t *packable;
    const mp_window *w;
} sample_impl;

static int sample_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e) {
    const sample_impl *x = s->impl;
    return mp_source_read(x->src, x->packable[x->w[i].file], x->w[i].off + off, buf, len, e);
}

static void sample_close(mp_source *s) { free(s->impl); }

// Window k is the file "w/<k>" (in the order of the windows, which is the order of the names).
static mp_source *sample_source(mp_source *src, const uint32_t *packable, const mp_sample *smp, mp_err *e) {
    mp_source *s = calloc(1, sizeof *s);
    sample_impl *x = calloc(1, sizeof *x);
    mp_entry *en = calloc(smp->n ? smp->n : 1, sizeof *en);
    if (!s || !x || !en) {
        free(s);
        free(x);
        free(en);
        mp_err_set(e, "out of memory");
        return NULL;
    }
    x->src = src;
    x->packable = packable;
    x->w = smp->w;
    s->kind = mp_source_kind_of(src);
    s->compressed = mp_source_compressed(src);
    s->entries = en;
    s->read = sample_read;
    s->close = sample_close;
    s->impl = x;
    for (size_t k = 0; k < smp->n; k++) {
        char name[32];
        snprintf(name, sizeof name, "w/%010zu", k);
        if (!(en[k].path = mp_strdup(name))) {
            mp_source_close(s);
            mp_err_set(e, "out of memory");
            return NULL;
        }
        en[k].size = smp->w[k].len;
        s->n = k + 1;
    }
    return s;
}

// The packs go nowhere: only their bytes count.
static int null_begin(void *ctx, uint32_t vol, mp_err *e) {
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

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

// stored[k]: what window k took in the packs, from the ampr_zstd.index of the sample (its files and
// their blocks: each block record has its size less one in bits 44 to 61).
static int window_stored(const uint8_t *d, size_t len, size_t nwin, uint64_t *stored, mp_err *e) {
    if (len < MP_ZPK_HEADER) goto bad;
    uint64_t n = rd64(d + 40), nchunks = rd64(d + 48), files = rd64(d + 56), chunks = rd64(d + 64),
             strings = rd64(d + 80);
    if (n != nwin || files > len || n > (len - files) / 32 || chunks > len || nchunks > (len - chunks) / 8 ||
        strings > len)
        goto bad;
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t *r = d + files + 32 * i;
        uint64_t size = rd64(r + 8), first = rd32(r + 16), poff = rd32(r + 20), plen = r[24] | r[25] << 8;
        if (plen < 3 || poff > len - strings || plen > len - strings - poff) goto bad;
        const char *name = (const char *)d + strings + poff;
        size_t k = 0;
        for (uint64_t c = 2; c < plen; c++) {
            if (name[c] < '0' || name[c] > '9') goto bad;
            k = k * 10 + (size_t)(name[c] - '0');
        }
        uint64_t blocks = (size + MP_BLOCK_V1 - 1) / MP_BLOCK_V1;
        if (k >= nwin || first > nchunks || blocks > nchunks - first) goto bad;
        uint64_t sum = 0;
        for (uint64_t b = 0; b < blocks; b++) sum += ((rd64(d + chunks + 8 * (first + b)) >> 44) & 0x3FFFFu) + 1;
        stored[k] = sum;
    }
    return 0;
bad:
    mp_err_set(e, "the index of the sample cannot be read");
    return -1;
}

// ---- the loose files, as a .p5z compresses them -----------------------------------------------

typedef struct {
    mp_source *src;
    const uint32_t *loose;
    const mp_sample *smp;
    ZSTD_CCtx **cctx;
    uint8_t **in, **out;
    size_t out_cap;
    uint64_t *stored;
    mp_progress *prog;
    mp_mutex *mu;
    int failed;
    mp_err err;
} lctx;

static int loose_work(void *vc, size_t i, unsigned tid) {
    lctx *c = vc;
    const mp_window *w = &c->smp->w[i];
    mp_err e = {{0}};
    if (mp_source_read(c->src, c->loose[w->file], w->off, c->in[tid], w->len, &e) != 0) {
        mp_mutex_lock(c->mu);
        if (!c->failed) c->err = e;
        c->failed = 1;
        mp_mutex_unlock(c->mu);
        return 1;
    }
    size_t r = ZSTD_compress2(c->cctx[tid], c->out[tid], c->out_cap, c->in[tid], w->len);
    c->stored[i] = !ZSTD_isError(r) && r < w->len ? r : w->len;
    mp_progress_add(c->prog, w->len);
    return 0;
}

// Compresses the frames of the sample of the loose files on up to MP_LONG_MAX_THREADS threads, as
// a .p5z does: *stored is what they take and *ns how long it took.
static int run_loose(mp_source *src, const uint32_t *loose, const mp_sample *smp, unsigned threads,
                     mp_progress *prog, uint64_t *stored, uint64_t *ns, mp_err *e) {
    *stored = *ns = 0;
    if (smp->n == 0) return 0;
    if (threads > MP_LONG_MAX_THREADS) threads = MP_LONG_MAX_THREADS;
    if (threads > smp->n) threads = (unsigned)smp->n;
    if (threads == 0) threads = 1;
    lctx c;
    memset(&c, 0, sizeof c);
    c.src = src;
    c.loose = loose;
    c.smp = smp;
    c.prog = prog;
    c.out_cap = ZSTD_compressBound(MP_LONG_FRAME);
    c.cctx = calloc(threads, sizeof *c.cctx);
    c.in = calloc(threads, sizeof *c.in);
    c.out = calloc(threads, sizeof *c.out);
    c.stored = calloc(smp->n, sizeof *c.stored);
    c.mu = mp_mutex_new();
    int rc = -1;
    if (!c.cctx || !c.in || !c.out || !c.stored || !c.mu) goto oom;
    for (unsigned t = 0; t < threads; t++) {
        if (!(c.cctx[t] = ZSTD_createCCtx()) || !(c.in[t] = malloc(MP_LONG_FRAME)) || !(c.out[t] = malloc(c.out_cap)))
            goto oom;
        mp_zstd_set_long(c.cctx[t]);
    }
    uint64_t t0 = mp_now_ns();
    if (mp_parallel_run(threads, smp->n, loose_work, &c, NULL) != 0) {
        if (mp_cancel_check(e) == 0 && e) *e = c.err;
        goto done;
    }
    *ns = mp_now_ns() - t0;
    for (size_t i = 0; i < smp->n; i++) *stored += c.stored[i];
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "out of memory");
done:
    for (unsigned t = 0; t < threads; t++) {
        if (c.cctx) ZSTD_freeCCtx(c.cctx[t]);
        if (c.in) free(c.in[t]);
        if (c.out) free(c.out[t]);
    }
    free(c.cctx);
    free(c.in);
    free(c.out);
    free(c.stored);
    mp_mutex_free(c.mu);
    return rc;
}

// ---- comparing -----------------------------------------------------------------------------------

// Reads the sample once, in order and through the read-ahead as the pack reads a game: how fast the
// game is read, and the sample is then in memory for the levels when it fits.
static int read_sample(mp_source *src, const uint32_t *packable, const mp_sample *smp, unsigned threads,
                       mp_progress *prog, uint64_t *ns, mp_err *e) {
    *ns = 0;
    mp_readahead_range *rg = malloc((smp->n ? smp->n : 1) * sizeof *rg);
    if (!rg) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    for (size_t k = 0; k < smp->n; k++) {
        rg[k].file = packable[smp->w[k].file];
        rg[k].off = smp->w[k].off;
        rg[k].len = smp->w[k].len;
    }
    mp_readahead_opts ro;
    mp_readahead_opts_default(&ro);
    if (mp_source_readers(src) > ro.threads) ro.threads = mp_source_readers(src);   // as mp_zpk_write
    if (ro.threads > threads) ro.threads = threads;
    mp_readahead *ra = NULL;
    mp_readahead_piece pc;
    uint64_t t0 = mp_now_ns();
    int rc = mp_readahead_open_ranges(src, rg, smp->n, NULL, &ro, &ra, e);
    while (rc == 0 && (rc = mp_readahead_next(ra, &pc, e)) == 1) {
        rc = mp_cancel_check(e);
        mp_progress_add(prog, pc.len);
        mp_progress_maybe_print(prog);
    }
    *ns = mp_now_ns() - t0;
    mp_readahead_close(ra);
    free(rg);
    return rc;
}

// Seconds for people: at least 1 when there was work.
static uint64_t seconds(double s, int work) {
    if (!work) return 0;
    long long r = llround(s);
    return r < 1 ? 1 : (uint64_t)r;
}

static uint64_t scaled(uint64_t part, uint64_t of, uint64_t whole) {
    if (of == 0) return 0;
    if (of == whole) return part;
    return (uint64_t)llround((double)part * (double)whole / (double)of);
}

int mp_levels_run(const char *src_path, const mp_levels_opts *o, mp_levels_report *r, mp_err *e) {
    memset(r, 0, sizeof *r);
    uint64_t t0 = mp_now_ns();
    unsigned threads = o->threads ? o->threads : 1;
    mp_source *src = NULL, *ssrc = NULL;
    mp_rules *rules = NULL;
    uint32_t *packable = NULL, *loose = NULL, *ids = NULL;
    uint64_t *psizes = NULL, *lsizes = NULL, *stored = NULL, *raw_f = NULL, *st_f = NULL;
    uint8_t (*sha)[32] = NULL;
    mp_sample smp = {0}, lsmp = {0};
    mp_progress prog;
    int prog_ok = 0, rc = -1;

    if (o->nlevels == 0 || o->nlevels > MP_LEVELS_MAX) {
        mp_err_set(e, "no levels to compare");
        return -1;
    }
    for (size_t i = 0; i < o->nlevels; i++)
        if (o->levels[i] < 1 || o->levels[i] > MP_LEVELS_MAX || (i && o->levels[i] <= o->levels[i - 1])) {
            mp_err_set(e, "the levels go from 1 to 22, in increasing order");
            return -1;
        }
    if (mp_source_open(src_path, &src, e) != 0) return -1;
    size_t n = mp_source_count(src);
    r->kind = mp_source_kind_of(src);
    if (r->kind != MP_SRC_DIR && (mp_source_compressed(src) || r->kind == MP_SRC_FPKG)) {
        int64_t mtime = 0;
        if (mp_stat(src_path, &r->image_size, &mtime) != 0) r->image_size = 0;
    }
    r->files = n;
    r->threads = threads;
    if (n == 0) {
        mp_err_set(e, "the source has no files: %s", src_path);
        goto done;
    }
    rules = mp_rules_new();
    packable = malloc(n * sizeof *packable);
    loose = malloc(n * sizeof *loose);
    psizes = malloc(n * sizeof *psizes);
    lsizes = malloc(n * sizeof *lsizes);
    if (!rules || !packable || !loose || !psizes || !lsizes) goto oom;
    for (size_t k = 0; k < o->nkeep; k++)
        if (mp_rules_add_keep_loose(rules, o->keep_loose[k], e) != 0) goto done;
    size_t npack = 0, nloose = 0;
    for (size_t i = 0; i < n; i++) {
        const mp_entry *en = mp_source_entry(src, i);
        if (mp_glob_match("ampr_zstd.index", en->path) || mp_glob_match("ampr_zstd-*.pak", en->path)) {
            mp_err_set(e, "%s is already packed (it has %s)", src_path, en->path);
            goto done;
        }
        r->original_size += en->size;
        if (mp_rules_classify(rules, en->path, en->size) == MP_LOOSE) {
            lsizes[nloose] = en->size;
            loose[nloose++] = (uint32_t)i;
            r->loose_files++;
            r->loose_bytes += en->size;
        } else {
            psizes[npack] = en->size;
            packable[npack++] = (uint32_t)i;
            r->packable_files++;
            r->packable_bytes += en->size;
        }
    }
    if (mp_sample_select(psizes, npack, o->sample_fraction, o->sample_min, o->full, &smp, e) != 0) goto done;
    if (mp_sample_select_ex(lsizes, nloose, MP_LONG_FRAME, o->sample_fraction, o->loose_min, o->full, &lsmp, e) != 0)
        goto done;
    r->sample_bytes = smp.sample_bytes;
    r->sample_rate = smp.rate;
    r->loose_sample_bytes = lsmp.sample_bytes;
    r->loose_rate = lsmp.rate;

    // one progress for everything: reading the sample, the loose files, then each level
    mp_progress_init(&prog, "Comparing the levels", smp.sample_bytes * (1 + o->nlevels) + lsmp.sample_bytes,
                     o->progress);
    prog_ok = 1;
    uint64_t read_ns = 0;
    if (smp.n && read_sample(src, packable, &smp, threads, &prog, &read_ns, e) != 0) goto done;
    if (smp.n && read_ns) r->read_rate = (uint64_t)llround((double)smp.sample_bytes * 1e9 / (double)read_ns);
    uint64_t lstored = 0, lns = 0;
    if (run_loose(src, loose, &lsmp, threads, &prog, &lstored, &lns, e) != 0) goto done;
    r->loose_compressed = scaled(lstored, lsmp.sample_bytes, r->loose_bytes);
    r->loose_ns = scaled(lns, lsmp.sample_bytes, r->loose_bytes);

    if (smp.n) {
        if (!(ssrc = sample_source(src, packable, &smp, e))) goto done;
        ids = malloc(smp.n * sizeof *ids);
        sha = malloc(smp.n * sizeof *sha);
        stored = malloc(smp.n * sizeof *stored);
        raw_f = calloc(npack, sizeof *raw_f);
        st_f = calloc(npack, sizeof *st_f);
        if (!ids || !sha || !stored || !raw_f || !st_f) goto oom;
        for (size_t k = 0; k < smp.n; k++) ids[k] = (uint32_t)k;
    }
    // the loose files of a folder are copied: read at the speed of the sample
    double copy_s = r->read_rate ? (double)r->loose_bytes / (double)r->read_rate : 0;
    for (size_t li = 0; li < o->nlevels; li++) {
        mp_level_row *row = &r->rows[li];
        row->level = o->levels[li];
        if (smp.n) {
            mp_zpk_opts zo = {row->level, threads, MP_ZPK_VOLUME_LIMIT, &prog};
            mp_zpk_sink sink = {null_begin, null_write, null_begin, null_header, NULL};
            mp_zpk_stats st;
            uint8_t *idx = NULL;
            size_t idx_len = 0;
            uint64_t z0 = mp_now_ns();
            if (mp_zpk_write(ssrc, ids, smp.n, &zo, &sink, &idx, &idx_len, sha, &st, e) != 0) goto done;
            uint64_t zns = mp_now_ns() - z0;
            int wr = window_stored(idx, idx_len, smp.n, stored, e);
            free(idx);
            if (wr != 0) goto done;
            memset(raw_f, 0, npack * sizeof *raw_f);
            memset(st_f, 0, npack * sizeof *st_f);
            for (size_t k = 0; k < smp.n; k++) {
                raw_f[smp.w[k].file] += smp.w[k].len;
                st_f[smp.w[k].file] += stored[k];
            }
            if (mp_strata_estimate(src, packable, npack, raw_f, st_f, &row->packed, e) != 0) goto done;
            row->rate = zns ? (uint64_t)llround((double)smp.sample_bytes * 1e9 / (double)zns) : 0;
        }
        uint64_t index = mp_v1_index_bytes(src, packable, npack, row->packed);
        row->p5z_size = row->packed + index + r->loose_compressed;
        row->folder_size = row->packed + index + r->loose_bytes;
        // the packs go as fast as the pipeline went through the sample, or as the game is read
        double speed = (double)row->rate;
        if (r->read_rate && (double)r->read_rate < speed) speed = (double)r->read_rate;
        double packs_s = speed > 0 ? (double)r->packable_bytes / speed : 0;
        row->p5z_seconds = seconds((double)r->loose_ns / 1e9 + packs_s, r->original_size > 0);
        row->folder_seconds = seconds(copy_s + packs_s, r->original_size > 0);
        r->nlevels = li + 1;
        if (o->on_level) o->on_level(o->ctx, row);
    }
    if (o->progress) fprintf(stderr, "\rComparing the levels: 100.0 %%\n");
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "out of memory");
done:
    r->elapsed_ns = mp_now_ns() - t0;
    if (prog_ok) mp_progress_free(&prog);
    mp_source_close(ssrc);
    mp_sample_free(&smp);
    mp_sample_free(&lsmp);
    free(ids);
    free(sha);
    free(stored);
    free(raw_f);
    free(st_f);
    free(psizes);
    free(lsizes);
    free(packable);
    free(loose);
    mp_rules_free(rules);
    mp_source_close(src);
    return rc;
}
