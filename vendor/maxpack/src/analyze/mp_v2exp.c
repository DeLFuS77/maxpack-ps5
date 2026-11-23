// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_v2exp.h"

#include <stdlib.h>
#include <string.h>
#include <zdict.h>

#define BLOCK_TRAIN_BUDGET (16u << 20)
#define SMALL_TRAIN_BUDGET (8u << 20)

typedef struct {
    uint8_t *buf;
    size_t used, cap;
    size_t *sizes;
    size_t n, ncap;
} train_set;

static int ts_push(train_set *t, size_t len) {
    if (t->n == t->ncap) {
        size_t c = t->ncap ? t->ncap * 2 : 1024;
        size_t *s = realloc(t->sizes, c * sizeof *s);
        if (!s) return -1;
        t->sizes = s;
        t->ncap = c;
    }
    t->sizes[t->n++] = len;
    t->used += len;
    return 0;
}

// Entrena con lo reunido. El tamaño del diccionario se limita a una décima parte de los datos
// (con pocos datos, un diccionario grande solo memoriza). NULL si ZDICT no puede.
static void *train(const train_set *t, size_t *out_size) {
    *out_size = 0;
    if (t->n < 8 || t->used == 0) return NULL;
    size_t cap = t->used / 10;
    if (cap > MP_DICT_SIZE) cap = MP_DICT_SIZE;
    if (cap < 4096) cap = 4096;
    void *d = malloc(cap);
    if (!d) return NULL;
    size_t r = ZDICT_trainFromBuffer(d, cap, t->buf, t->sizes, (unsigned)t->n);
    if (ZDICT_isError(r)) {
        free(d);
        return NULL;
    }
    *out_size = r;
    return d;
}

static void ts_free(train_set *t) {
    free(t->buf);
    free(t->sizes);
    memset(t, 0, sizeof *t);
}

int mp_dict_train_blocks(mp_source *src, const uint32_t *packable, const mp_sample *s,
                         mp_dict_pair *out, mp_err *e) {
    memset(out, 0, sizeof *out);
    for (unsigned p = 0; p < 2; p++) {
        uint64_t tp = 0;
        for (size_t i = 0; i < s->n; i++)
            if ((s->w[i].ordinal & 1) == p) tp += s->w[i].len;
        uint64_t k = (tp + BLOCK_TRAIN_BUDGET - 1) / BLOCK_TRAIN_BUDGET;
        if (k == 0) k = 1;
        train_set t = {0};
        t.cap = BLOCK_TRAIN_BUDGET;
        if (!(t.buf = malloc(t.cap))) {
            mp_err_set(e, "sin memoria");
            return -1;
        }
        uint64_t rank = 0;
        for (size_t i = 0; i < s->n; i++) {
            const mp_window *w = &s->w[i];
            if ((w->ordinal & 1) != p || rank++ % k) continue;
            if (t.used + w->len > t.cap) break;
            if (mp_source_read(src, packable[w->file], w->off, t.buf + t.used, w->len, e) != 0) {
                ts_free(&t);
                mp_dict_pair_free(out);
                return -1;
            }
            for (size_t off = 0; off < w->len; off += MP_BLOCK_V1) {
                size_t n = w->len - off < MP_BLOCK_V1 ? w->len - off : MP_BLOCK_V1;
                if (ts_push(&t, n) != 0) {
                    ts_free(&t);
                    mp_dict_pair_free(out);
                    mp_err_set(e, "sin memoria");
                    return -1;
                }
            }
        }
        out->dict[p] = train(&t, &out->size[p]);
        ts_free(&t);
    }
    return 0;
}

int mp_dict_train_small(mp_source *src, const uint32_t *small, size_t n, mp_dict_pair *out,
                        mp_err *e) {
    memset(out, 0, sizeof *out);
    for (unsigned p = 0; p < 2; p++) {
        uint64_t tp = 0;
        for (size_t i = p; i < n; i += 2) tp += mp_source_entry(src, small[i])->size;
        uint64_t k = (tp + SMALL_TRAIN_BUDGET - 1) / SMALL_TRAIN_BUDGET;
        if (k == 0) k = 1;
        train_set t = {0};
        t.cap = SMALL_TRAIN_BUDGET;
        if (!(t.buf = malloc(t.cap))) {
            mp_err_set(e, "sin memoria");
            return -1;
        }
        uint64_t rank = 0;
        for (size_t i = p; i < n; i += 2) {
            if (rank++ % k) continue;
            size_t len = (size_t)mp_source_entry(src, small[i])->size;
            if (t.used + len > t.cap) break;
            if (mp_source_read(src, small[i], 0, t.buf + t.used, len, e) != 0 ||
                ts_push(&t, len) != 0) {
                if (!e || !e->msg[0]) mp_err_set(e, "sin memoria");
                ts_free(&t);
                mp_dict_pair_free(out);
                return -1;
            }
        }
        out->dict[p] = train(&t, &out->size[p]);
        ts_free(&t);
    }
    return 0;
}

void mp_dict_pair_free(mp_dict_pair *p) {
    if (!p) return;
    free(p->dict[0]);
    free(p->dict[1]);
    memset(p, 0, sizeof *p);
}

typedef struct {
    ZSTD_CCtx *c, *d[2];
    uint8_t *buf, *out;
    size_t out_cap;
    uint64_t individual, individual_dict, grouped;
} tstate;

typedef struct {
    mp_source *src;
    const uint32_t *small;
    size_t n;
    uint64_t *prefix;   // n + 1 posiciones del flujo concatenado
    tstate *ts;
    mp_progress *prog;
    mp_mutex *mu;
    int failed;
    mp_err err;
} sctx;

static size_t keep(size_t frame, size_t raw) { return frame && frame < raw ? frame : raw; }

static int fail(sctx *c, const mp_err *e) {
    mp_mutex_lock(c->mu);
    if (!c->failed) c->err = *e;
    c->failed = 1;
    mp_mutex_unlock(c->mu);
    return 1;
}

static int work_file(void *vc, size_t i, unsigned tid) {
    sctx *c = vc;
    tstate *t = &c->ts[tid];
    size_t size = (size_t)mp_source_entry(c->src, c->small[i])->size;
    mp_err e = {{0}};
    if (mp_source_read(c->src, c->small[i], 0, t->buf, size, &e) != 0) return fail(c, &e);
    size_t f = keep(mp_zstd_block(t->c, 19, 18, t->buf, size, t->out, t->out_cap), size);
    t->individual += f;
    ZSTD_CCtx *dc = t->d[1 - (i & 1)];
    if (dc) {
        size_t r = ZSTD_compress2(dc, t->out, t->out_cap, t->buf, size);
        t->individual_dict += keep(ZSTD_isError(r) ? 0 : r, size);
    } else {
        t->individual_dict += f;
    }
    mp_progress_add(c->prog, size);
    return 0;
}

static int work_group(void *vc, size_t b, unsigned tid) {
    sctx *c = vc;
    tstate *t = &c->ts[tid];
    uint64_t start = (uint64_t)b * MP_BLOCK_V1, end = start + MP_BLOCK_V1;
    if (end > c->prefix[c->n]) end = c->prefix[c->n];
    size_t lo = 0, hi = c->n;   // primer archivo con prefix[j + 1] > start
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (c->prefix[mid + 1] <= start)
            lo = mid + 1;
        else
            hi = mid;
    }
    mp_err e = {{0}};
    for (uint64_t pos = start; pos < end; lo++) {
        uint64_t in = pos - c->prefix[lo], fsize = c->prefix[lo + 1] - c->prefix[lo];
        size_t take = (size_t)(end - pos < fsize - in ? end - pos : fsize - in);
        if (mp_source_read(c->src, c->small[lo], in, t->buf + (pos - start), take, &e) != 0)
            return fail(c, &e);
        pos += take;
    }
    size_t n = (size_t)(end - start);
    t->grouped += keep(mp_zstd_block(t->c, 19, 18, t->buf, n, t->out, t->out_cap), n);
    mp_progress_add(c->prog, n);
    return 0;
}

int mp_small_run(mp_source *src, const uint32_t *small, size_t n, const mp_dict_pair *dicts,
                 unsigned threads, mp_progress *prog, mp_small_result *out, mp_err *e) {
    memset(out, 0, sizeof *out);
    if (threads == 0) threads = 1;
    sctx c = {0};
    c.src = src;
    c.small = small;
    c.n = n;
    c.prog = prog;
    c.mu = mp_mutex_new();
    c.prefix = malloc((n + 1) * sizeof *c.prefix);
    c.ts = calloc(threads, sizeof *c.ts);
    int rc = -1;
    if (!c.mu || !c.prefix || !c.ts) goto oom;
    c.prefix[0] = 0;
    for (size_t i = 0; i < n; i++) c.prefix[i + 1] = c.prefix[i] + mp_source_entry(src, small[i])->size;
    for (unsigned k = 0; k < threads; k++) {
        tstate *t = &c.ts[k];
        t->out_cap = ZSTD_compressBound(MP_BLOCK_V1);
        t->c = ZSTD_createCCtx();
        t->buf = malloc(MP_BLOCK_V1);
        t->out = malloc(t->out_cap);
        if (!t->c || !t->buf || !t->out) goto oom;
        for (int p = 0; p < 2; p++) {
            if (!dicts || !dicts->dict[p]) continue;
            ZSTD_CCtx *d = ZSTD_createCCtx();
            if (!d) goto oom;
            ZSTD_CCtx_setParameter(d, ZSTD_c_compressionLevel, 19);
            ZSTD_CCtx_setParameter(d, ZSTD_c_windowLog, 18);
            ZSTD_CCtx_setParameter(d, ZSTD_c_checksumFlag, 1);
            ZSTD_CCtx_setParameter(d, ZSTD_c_contentSizeFlag, 1);
            if (ZSTD_isError(ZSTD_CCtx_loadDictionary(d, dicts->dict[p], dicts->size[p]))) {
                ZSTD_freeCCtx(d);
                continue;
            }
            t->d[p] = d;
        }
    }
    if (mp_parallel_run(threads, n, work_file, &c, prog) != 0 ||
        mp_parallel_run(threads, (size_t)((c.prefix[n] + MP_BLOCK_V1 - 1) / MP_BLOCK_V1), work_group,
                        &c, prog) != 0) {
        if (e) *e = c.err;
        goto done;
    }
    out->files = n;
    out->raw = c.prefix[n];
    for (unsigned k = 0; k < threads; k++) {
        out->individual += c.ts[k].individual;
        out->individual_dict += c.ts[k].individual_dict;
        out->grouped += c.ts[k].grouped;
    }
    out->dicts = (c.ts[0].d[0] != NULL) + (c.ts[0].d[1] != NULL);
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "sin memoria");
done:
    if (c.ts)
        for (unsigned k = 0; k < threads; k++) {
            ZSTD_freeCCtx(c.ts[k].c);
            ZSTD_freeCCtx(c.ts[k].d[0]);
            ZSTD_freeCCtx(c.ts[k].d[1]);
            free(c.ts[k].buf);
            free(c.ts[k].out);
        }
    free(c.ts);
    free(c.prefix);
    mp_mutex_free(c.mu);
    return rc;
}
