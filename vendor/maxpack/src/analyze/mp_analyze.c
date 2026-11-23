// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_analyze.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mp_platform.h"
#include "mp_rules.h"
#include "mp_sample.h"

#define AMPR_VOLUME_MAX 4294901760ull  // tamaño máximo de un ampr_zstd-NNN.pak, cabecera incluida

void mp_analyze_opts_default(mp_analyze_opts *o) {
    memset(o, 0, sizeof *o);
    o->sample_fraction = 0.02;
    o->sample_min = 1ull << 30;
    o->threads = mp_cpu_count();
    o->dedup = 1;
    o->transport = 1;
    o->progress = 1;
}

void mp_report_free(mp_report *r) {
    if (!r) return;
    free(r->source_name);
    r->source_name = NULL;
}

static char *base_name(const char *path) {
    size_t n = strlen(path);
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) n--;
    size_t s = n;
    while (s > 0 && path[s - 1] != '/' && path[s - 1] != '\\') s--;
    char *out = malloc(n - s + 1);
    if (out) {
        memcpy(out, path + s, n - s);
        out[n - s] = 0;
    }
    return out;
}

// ---- Medición de la muestra -------------------------------------------------------------

typedef struct {
    mp_source *src;
    const uint32_t *packable;
    const mp_sample *sample;
    mp_measurer **m;
    uint8_t **buf;
    mp_cfg_stat (*fst)[MP_CFG_COUNT];   // por archivo empaquetable
    mp_cfg_stat tot[MP_CFG_COUNT];
    mp_mutex *mu;
    mp_progress *prog;
    int failed;
    mp_err err;
} mctx;

static int measure_work(void *vc, size_t i, unsigned tid) {
    mctx *c = vc;
    const mp_window *w = &c->sample->w[i];
    mp_err e = {{0}};
    if (mp_source_read(c->src, c->packable[w->file], w->off, c->buf[tid], w->len, &e) != 0) {
        mp_mutex_lock(c->mu);
        if (!c->failed) c->err = e;
        c->failed = 1;
        mp_mutex_unlock(c->mu);
        return 1;
    }
    mp_cfg_stat st[MP_CFG_COUNT] = {{0}};
    mp_measure_window(c->m[tid], c->buf[tid], w->len, (unsigned)(w->ordinal & 1), st);
    mp_mutex_lock(c->mu);
    for (int k = 0; k < MP_CFG_COUNT; k++) {
        c->fst[w->file][k].raw += st[k].raw;
        c->fst[w->file][k].stored += st[k].stored;
        c->fst[w->file][k].ns += st[k].ns;
        c->tot[k].raw += st[k].raw;
        c->tot[k].stored += st[k].stored;
        c->tot[k].ns += st[k].ns;
    }
    mp_mutex_unlock(c->mu);
    mp_progress_add(c->prog, w->len);
    return 0;
}

static int run_measure(mp_source *src, const uint32_t *packable, const mp_sample *s,
                       const mp_dict_pair *dicts, unsigned threads, int progress,
                       mp_cfg_stat (*fst)[MP_CFG_COUNT], mp_cfg_stat tot[MP_CFG_COUNT], mp_err *e) {
    mctx c = {0};
    c.src = src;
    c.packable = packable;
    c.sample = s;
    c.fst = fst;
    c.mu = mp_mutex_new();
    c.m = calloc(threads, sizeof *c.m);
    c.buf = calloc(threads, sizeof *c.buf);
    int rc = -1;
    mp_progress prog;
    mp_progress_init(&prog, "Comprimiendo la muestra", s->sample_bytes, progress);
    c.prog = &prog;
    if (!c.mu || !c.m || !c.buf) goto oom;
    for (unsigned t = 0; t < threads; t++)
        if (!(c.m[t] = mp_measurer_new(dicts)) || !(c.buf[t] = malloc(MP_WINDOW))) goto oom;
    if (mp_parallel_run(threads, s->n, measure_work, &c, &prog) != 0) {
        if (e) *e = c.err;
        goto done;
    }
    memcpy(tot, c.tot, sizeof c.tot);
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "sin memoria");
done:
    for (unsigned t = 0; c.m && t < threads; t++) mp_measurer_free(c.m[t]);
    for (unsigned t = 0; c.buf && t < threads; t++) free(c.buf[t]);
    free(c.m);
    free(c.buf);
    mp_mutex_free(c.mu);
    mp_progress_free(&prog);
    return rc;
}

// ---- Extrapolación por estratos (extensión del nombre) ----------------------------------

typedef struct {
    char ext[32];
    uint64_t total, raw, stored;
} stratum;

static void ext_of(const char *path, char out[32]) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    out[0] = 0;
    if (!dot || strlen(dot + 1) >= 32) return;
    size_t i = 0;
    for (const char *p = dot + 1; *p; p++) out[i++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    out[i] = 0;
}

static uint64_t scale(uint64_t total, uint64_t stored, uint64_t raw) {
    if (raw == 0) return total;
    if (total == raw) return stored;   // exacto cuando se ha medido todo
    return (uint64_t)llround((double)stored / (double)raw * (double)total);
}

// Estimación por estratos (extensión en minúsculas) de una medida: raw[f]/stored[f] es lo medido
// del empaquetable f. Estrato con datos: total · guardado / medido; sin datos: proporción global.
static int strata_estimate(mp_source *src, const uint32_t *packable, size_t npack,
                           const uint64_t *raw, const uint64_t *stored, uint64_t *out, mp_err *e) {
    stratum *g = NULL;
    size_t ng = 0, cap = 0;
    uint64_t R = 0, S = 0;
    for (size_t f = 0; f < npack; f++) {
        const mp_entry *en = mp_source_entry(src, packable[f]);
        char ext[32];
        ext_of(en->path, ext);
        size_t k = 0;
        while (k < ng && strcmp(g[k].ext, ext) != 0) k++;
        if (k == ng) {
            if (ng == cap) {
                cap = cap ? cap * 2 : 32;
                stratum *ng2 = realloc(g, cap * sizeof *g);
                if (!ng2) {
                    free(g);
                    mp_err_set(e, "sin memoria");
                    return -1;
                }
                g = ng2;
            }
            memset(&g[ng], 0, sizeof *g);
            strcpy(g[ng].ext, ext);
            ng++;
        }
        g[k].total += en->size;
        g[k].raw += raw[f];
        g[k].stored += stored[f];
        R += raw[f];
        S += stored[f];
    }
    *out = 0;
    for (size_t k = 0; k < ng; k++)
        *out += g[k].raw ? scale(g[k].total, g[k].stored, g[k].raw) : scale(g[k].total, S, R);
    free(g);
    return 0;
}

static int extrapolate(mp_source *src, const uint32_t *packable, size_t npack,
                       mp_cfg_stat (*fst)[MP_CFG_COUNT], uint64_t est[MP_CFG_COUNT], mp_err *e) {
    uint64_t *raw = malloc((npack ? npack : 1) * sizeof *raw);
    uint64_t *stored = malloc((npack ? npack : 1) * sizeof *stored);
    int rc = raw && stored ? 0 : -1;
    if (rc) mp_err_set(e, "sin memoria");
    for (int c = 0; !rc && c < MP_CFG_COUNT; c++) {
        for (size_t f = 0; f < npack; f++) {
            // Sin regla del 1 % (archivo entero en bruto): Forge la quitó y el pack tampoco la
            // aplica; cada bloque ya va en bruto cuando zstd no lo reduce.
            raw[f] = fst[f][c].raw;
            stored[f] = fst[f][c].stored;
        }
        rc = strata_estimate(src, packable, npack, raw, stored, &est[c], e);
    }
    free(raw);
    free(stored);
    return rc;
}

// ---- Ventana larga, como en el .p5z: sueltos y transporte --------------------------------

typedef struct {
    uint32_t file;   // índice de origen
    uint64_t off;
    uint32_t len;
} lframe;

typedef struct {
    mp_source *src;
    const lframe *items;
    ZSTD_CCtx **c;
    uint8_t **in, **out;
    size_t out_cap;
    uint64_t *stored;   // por tramo
    uint64_t *ns;       // por hilo
    mp_progress *prog;
    mp_mutex *mu;
    int failed;
    mp_err err;
} lctx;

static int long_work(void *vc, size_t i, unsigned tid) {
    lctx *c = vc;
    const lframe *it = &c->items[i];
    mp_err e = {{0}};
    if (mp_source_read(c->src, it->file, it->off, c->in[tid], it->len, &e) != 0) {
        mp_mutex_lock(c->mu);
        if (!c->failed) c->err = e;
        c->failed = 1;
        mp_mutex_unlock(c->mu);
        return 1;
    }
    uint64_t t0 = mp_now_ns();
    size_t r = ZSTD_compress2(c->c[tid], c->out[tid], c->out_cap, c->in[tid], it->len);
    c->ns[tid] += mp_now_ns() - t0;
    c->stored[i] = !ZSTD_isError(r) && r < it->len ? r : it->len;
    mp_progress_add(c->prog, it->len);
    return 0;
}

// Comprime cada tramo (<= 32 MiB) con zstd 19 y ventana larga. stored[i] = lo que ocupa el tramo i;
// *ns = tiempo de compresión sumado de todos los hilos; *used = hilos usados.
static int run_long(mp_source *src, const lframe *items, size_t n, unsigned threads,
                    const char *phase, int progress, uint64_t *stored, uint64_t *ns,
                    unsigned *used, mp_err *e) {
    *ns = 0;
    *used = 0;
    if (n == 0) return 0;
    if (threads > MP_LONG_MAX_THREADS) threads = MP_LONG_MAX_THREADS;
    if (threads > n) threads = (unsigned)n;
    if (threads == 0) threads = 1;
    *used = threads;
    uint64_t bytes = 0;
    for (size_t i = 0; i < n; i++) bytes += items[i].len;
    lctx c = {0};
    c.src = src;
    c.items = items;
    c.stored = stored;
    c.c = calloc(threads, sizeof *c.c);
    c.in = calloc(threads, sizeof *c.in);
    c.out = calloc(threads, sizeof *c.out);
    c.ns = calloc(threads, sizeof *c.ns);
    c.mu = mp_mutex_new();
    c.out_cap = ZSTD_compressBound(MP_LONG_FRAME);
    mp_progress prog;
    mp_progress_init(&prog, phase, bytes, progress);
    c.prog = &prog;
    int rc = -1;
    if (!c.c || !c.in || !c.out || !c.ns || !c.mu) goto oom;
    for (unsigned t = 0; t < threads; t++) {
        if (!(c.c[t] = ZSTD_createCCtx()) || !(c.in[t] = malloc(MP_LONG_FRAME)) ||
            !(c.out[t] = malloc(c.out_cap)))
            goto oom;
        mp_zstd_set_long(c.c[t]);
    }
    if (mp_parallel_run(threads, n, long_work, &c, &prog) != 0) {
        if (e) *e = c.err;
        goto done;
    }
    for (unsigned t = 0; t < threads; t++) *ns += c.ns[t];
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "sin memoria");
done:
    for (unsigned t = 0; t < threads; t++) {
        if (c.c) ZSTD_freeCCtx(c.c[t]);
        if (c.in) free(c.in[t]);
        if (c.out) free(c.out[t]);
    }
    free(c.c);
    free(c.in);
    free(c.out);
    free(c.ns);
    mp_mutex_free(c.mu);
    mp_progress_free(&prog);
    return rc;
}

static int run_loose(mp_source *src, const uint32_t *loose, size_t nloose, unsigned threads,
                     int progress, uint64_t *out, mp_err *e) {
    *out = 0;
    size_t n = 0;
    for (size_t i = 0; i < nloose; i++)
        n += (size_t)((mp_source_entry(src, loose[i])->size + MP_LONG_FRAME - 1) / MP_LONG_FRAME);
    if (n == 0) return 0;
    lframe *items = malloc(n * sizeof *items);
    uint64_t *stored = calloc(n, sizeof *stored), ns = 0;
    unsigned used = 0;
    int rc = -1;
    if (items && stored) {
        size_t k = 0;
        for (size_t i = 0; i < nloose; i++) {
            uint64_t size = mp_source_entry(src, loose[i])->size;
            for (uint64_t off = 0; off < size; off += MP_LONG_FRAME) {
                lframe f = {loose[i], off,
                            (uint32_t)(size - off < MP_LONG_FRAME ? size - off : MP_LONG_FRAME)};
                items[k++] = f;
            }
        }
        rc = run_long(src, items, n, threads, "Comprimiendo los sueltos", progress, stored, &ns,
                      &used, e);
        for (size_t i = 0; rc == 0 && i < n; i++) *out += stored[i];
    } else {
        mp_err_set(e, "sin memoria");
    }
    free(items);
    free(stored);
    return rc;
}

// Transporte: lo empaquetable como iría en un .p5z pensado solo para el PC (zstd 19, ventana
// larga, frames de 32 MiB), sobre una muestra de tramos de 32 MiB y extrapolado por estratos.
static int run_transport(mp_source *src, const uint32_t *packable, const uint64_t *sizes,
                         size_t npack, uint64_t packable_bytes, const mp_analyze_opts *o,
                         unsigned threads, mp_xport_stat *x, mp_err *e) {
    memset(x, 0, sizeof *x);
    mp_sample s = {0};
    if (mp_sample_select_ex(sizes, npack, MP_LONG_FRAME, o->sample_fraction, o->sample_min, o->full,
                            &s, e) != 0)
        return -1;
    lframe *items = malloc((s.n ? s.n : 1) * sizeof *items);
    uint64_t *stored = calloc(s.n ? s.n : 1, sizeof *stored);
    uint64_t *raw_f = calloc(npack ? npack : 1, sizeof *raw_f);
    uint64_t *st_f = calloc(npack ? npack : 1, sizeof *st_f);
    int rc = -1;
    if (!items || !stored || !raw_f || !st_f) {
        mp_err_set(e, "sin memoria");
        goto done;
    }
    for (size_t i = 0; i < s.n; i++) {
        lframe f = {packable[s.w[i].file], s.w[i].off, s.w[i].len};
        items[i] = f;
    }
    if (run_long(src, items, s.n, threads, "Transporte (.p5z)", o->progress, stored, &x->ns,
                 &x->threads, e) != 0)
        goto done;
    for (size_t i = 0; i < s.n; i++) {
        raw_f[s.w[i].file] += s.w[i].len;
        st_f[s.w[i].file] += stored[i];
        x->sample_raw += s.w[i].len;
        x->sample_stored += stored[i];
    }
    x->frames = s.n;
    x->sample_bytes = s.sample_bytes;
    x->rate = s.rate;
    if (strata_estimate(src, packable, npack, raw_f, st_f, &x->est_packed, e) != 0) goto done;
    if (x->sample_raw && x->threads)
        x->est_seconds = (uint64_t)llround((double)x->ns * (double)packable_bytes /
                                           (double)x->sample_raw / x->threads / 1e9);
    rc = 0;
done:
    free(items);
    free(stored);
    free(raw_f);
    free(st_f);
    mp_sample_free(&s);
    return rc;
}

// ---- Índices v1 ---------------------------------------------------------------------------

static uint64_t v1_index_bytes(mp_source *src, const uint32_t *packable, size_t npack,
                               uint64_t packed_l19) {
    uint64_t chunks = 0, names = 0, blob = 0, files = mp_source_count(src);
    for (size_t f = 0; f < npack; f++) {
        const mp_entry *en = mp_source_entry(src, packable[f]);
        chunks += (en->size + MP_BLOCK_V1 - 1) / MP_BLOCK_V1;
        names += strlen(en->path);
    }
    for (size_t i = 0; i < files; i++) blob += 6 + strlen(mp_source_entry(src, i)->path) + 1;
    uint64_t vols = 0, zpk = 0;
    if (npack) {
        vols = (packed_l19 + (AMPR_VOLUME_MAX - 64) - 1) / (AMPR_VOLUME_MAX - 64);
        if (vols == 0) vols = 1;
        zpk = 128 + 32 * (uint64_t)npack + 8 * chunks + 8 * vols + names;   // AMPRZPK1
    }
    uint64_t slots = 2;
    while (slots < 2 * files) slots <<= 1;
    uint64_t idx3 = 48 + 24 * files + ((blob + 15) & ~15ull) + 16 * slots;   // AMPRIDX3
    return zpk + idx3 + 64 * vols;
}

// ---- Orquestación -------------------------------------------------------------------------

static const uint64_t bin_hi[MP_SIZE_BINS] = {0,        4096,          65536,          262144,
                                              1u << 20, 64ull << 20,   4ull << 30,     UINT64_MAX};

int mp_analyze(const char *src_path, const mp_analyze_opts *o, mp_report *r, mp_err *e) {
    memset(r, 0, sizeof *r);
    uint64_t t0 = mp_now_ns();
    unsigned threads = o->threads ? o->threads : 1;
    mp_source *src = NULL;
    mp_rules *rules = NULL;
    uint32_t *packable = NULL, *small = NULL, *loose = NULL;
    uint64_t *sizes = NULL;
    mp_cfg_stat (*fst)[MP_CFG_COUNT] = NULL;
    mp_sample sample = {0};
    mp_dict_pair bdict = {{0}}, sdict = {{0}};
    int rc = -1;

    if (mp_source_open(src_path, &src, e) != 0) return -1;
    r->source_name = base_name(src_path);
    r->kind = mp_source_kind_of(src);
    r->files = mp_source_count(src);
    r->dirs = mp_source_dir_count(src);
    r->threads = threads;
    r->full = o->full;
    rules = mp_rules_new();
    size_t n = (size_t)r->files;
    packable = malloc((n ? n : 1) * sizeof *packable);
    small = malloc((n ? n : 1) * sizeof *small);
    loose = malloc((n ? n : 1) * sizeof *loose);
    sizes = malloc((n ? n : 1) * sizeof *sizes);
    if (!r->source_name || !rules || !packable || !small || !loose || !sizes) goto oom;
    for (size_t k = 0; k < o->nkeep; k++)
        if (mp_rules_add_keep_loose(rules, o->keep_loose[k], e) != 0) goto done;

    size_t npack = 0, nsmall = 0, nloose = 0;
    for (int b = 0; b < MP_SIZE_BINS; b++) {
        r->bins[b].lo = b == 0 ? 0 : bin_hi[b - 1] + 1;
        r->bins[b].hi = bin_hi[b];
    }
    for (size_t i = 0; i < n; i++) {
        const mp_entry *en = mp_source_entry(src, i);
        r->total_bytes += en->size;
        int b = 0;
        while (en->size > bin_hi[b]) b++;
        r->bins[b].files++;
        r->bins[b].bytes += en->size;
        if (mp_rules_classify(rules, en->path, en->size) == MP_LOOSE) {
            loose[nloose++] = (uint32_t)i;
            r->loose_files++;
            r->loose_bytes += en->size;
        } else {
            sizes[npack] = en->size;
            packable[npack++] = (uint32_t)i;
            r->packable_files++;
            r->packable_bytes += en->size;
            if (en->size <= MP_SMALL_MAX) small[nsmall++] = (uint32_t)i;
        }
    }

    if (mp_sample_select(sizes, npack, o->sample_fraction, o->sample_min, o->full, &sample, e) != 0)
        goto done;
    r->sample_bytes = sample.sample_bytes;
    r->sample_windows = sample.n;
    r->sample_rate = sample.rate;

    if (npack) {
        if (mp_dict_train_blocks(src, packable, &sample, &bdict, e) != 0) goto done;
        r->block_dicts = (bdict.dict[0] != NULL) + (bdict.dict[1] != NULL);
        fst = calloc(npack, sizeof *fst);
        if (!fst) goto oom;
        if (run_measure(src, packable, &sample, &bdict, threads, o->progress, fst, r->sampled, e) != 0)
            goto done;
        if (extrapolate(src, packable, npack, fst, r->est_packed, e) != 0) goto done;
        for (int c = 0; c < MP_CFG_COUNT; c++)
            if (r->sampled[c].raw)
                r->est_seconds[c] = (uint64_t)llround((double)r->sampled[c].ns *
                                                      (double)r->packable_bytes /
                                                      (double)r->sampled[c].raw / threads / 1e9);
    }

    if (nsmall) {
        if (mp_dict_train_small(src, small, nsmall, &sdict, e) != 0) goto done;
        mp_progress prog;
        uint64_t sb = 0;
        for (size_t i = 0; i < nsmall; i++) sb += mp_source_entry(src, small[i])->size;
        mp_progress_init(&prog, "Archivos pequeños", 2 * sb, o->progress);
        int src_rc = mp_small_run(src, small, nsmall, &sdict, threads, &prog, &r->small, e);
        mp_progress_free(&prog);
        if (src_rc != 0) goto done;
    }

    if (o->dedup && npack) {
        mp_progress prog;
        mp_progress_init(&prog, "Deduplicación (lee todo el juego)", r->packable_bytes, o->progress);
        int drc = mp_dedup_run(src, packable, npack, threads, &prog, &r->dedup, e);
        mp_progress_free(&prog);
        if (drc != 0) goto done;
        r->dedup_done = 1;
    }

    if (run_loose(src, loose, nloose, threads, o->progress, &r->loose_compressed, e) != 0) goto done;
    if (o->transport && npack) {
        if (run_transport(src, packable, sizes, npack, r->packable_bytes, o, threads, &r->xport, e) != 0)
            goto done;
        r->xport_done = 1;
        r->p5z_transport = r->xport.est_packed + r->loose_compressed;
    }
    r->v1_index_bytes = v1_index_bytes(src, packable, npack, r->est_packed[MP_CFG_V1_L19]);
    r->p5z_estimate = r->est_packed[MP_CFG_V1_L19] + r->v1_index_bytes + r->loose_compressed;
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "sin memoria");
done:
    r->elapsed_ns = mp_now_ns() - t0;
    mp_dict_pair_free(&bdict);
    mp_dict_pair_free(&sdict);
    mp_sample_free(&sample);
    free(fst);
    free(sizes);
    free(packable);
    free(small);
    free(loose);
    mp_rules_free(rules);
    mp_source_close(src);
    if (rc != 0) mp_report_free(r);
    return rc;
}
