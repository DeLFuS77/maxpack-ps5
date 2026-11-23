// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_dedup.h"

#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_measure.h"

#define DEDUP_ITEM (64ull << 20)   // un ítem = hasta 64 MiB de un archivo
#define DEDUP_READ (1u << 20)      // se lee en trozos de 1 MiB (múltiplo de 256 KiB)

typedef struct {
    uint32_t file;   // índice en `files`
    uint64_t off, len, first_block;
} item;

typedef struct {
    mp_source *src;
    const uint32_t *files;
    item *items;
    uint64_t *hash;    // XXH64 por bloque global
    uint32_t *len;     // longitud por bloque global
    uint8_t **buf;     // un búfer por hilo
    mp_progress *prog;
    mp_mutex *mu;
    int failed;
    mp_err err;
} ctx;

static int work(void *vc, size_t i, unsigned tid) {
    ctx *c = vc;
    const item *it = &c->items[i];
    uint8_t *buf = c->buf[tid];
    for (uint64_t pos = 0; pos < it->len; pos += DEDUP_READ) {
        size_t n = it->len - pos < DEDUP_READ ? (size_t)(it->len - pos) : DEDUP_READ;
        mp_err e = {{0}};
        if (mp_source_read(c->src, c->files[it->file], it->off + pos, buf, n, &e) != 0) {
            mp_mutex_lock(c->mu);
            if (!c->failed) c->err = e;
            c->failed = 1;
            mp_mutex_unlock(c->mu);
            return 1;
        }
        uint64_t b = it->first_block + pos / MP_BLOCK_V1;
        for (size_t k = 0; k < n; k += MP_BLOCK_V1, b++) {
            size_t bl = n - k < MP_BLOCK_V1 ? n - k : MP_BLOCK_V1;
            c->hash[b] = mp_xxh64(buf + k, bl, 0);
            c->len[b] = (uint32_t)bl;
        }
        mp_progress_add(c->prog, n);
    }
    return 0;
}

typedef struct {
    uint64_t h;
    uint32_t len;
} hl;

static int cmp_hl(const void *a, const void *b) {
    const hl *x = a, *y = b;
    if (x->h != y->h) return x->h < y->h ? -1 : 1;
    return x->len < y->len ? -1 : x->len > y->len;
}

typedef struct {
    uint64_t size, key;
    size_t order;
} fk;

static int cmp_fk(const void *a, const void *b) {
    const fk *x = a, *y = b;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

int mp_dedup_run(mp_source *src, const uint32_t *files, size_t nfiles, unsigned threads,
                 mp_progress *prog, mp_dedup_result *out, mp_err *e) {
    memset(out, 0, sizeof *out);
    uint64_t t0 = mp_now_ns();
    if (threads == 0) threads = 1;
    uint64_t *first = malloc((nfiles + 1) * sizeof *first);
    size_t nitems = 0;
    uint64_t total = 0;
    if (!first) goto oom0;
    first[0] = 0;
    for (size_t f = 0; f < nfiles; f++) {
        uint64_t size = mp_source_entry(src, files[f])->size;
        first[f + 1] = first[f] + (size + MP_BLOCK_V1 - 1) / MP_BLOCK_V1;
        nitems += (size_t)((size + DEDUP_ITEM - 1) / DEDUP_ITEM);
        total += size;
    }
    uint64_t nblocks = first[nfiles];
    ctx c = {0};
    c.src = src;
    c.files = files;
    c.prog = prog;
    c.items = malloc((nitems ? nitems : 1) * sizeof *c.items);
    c.hash = malloc((nblocks ? nblocks : 1) * sizeof *c.hash);
    c.len = malloc((nblocks ? nblocks : 1) * sizeof *c.len);
    c.buf = calloc(threads, sizeof *c.buf);
    c.mu = mp_mutex_new();
    int rc = -1;
    if (!c.items || !c.hash || !c.len || !c.buf || !c.mu) goto oom;
    for (unsigned t = 0; t < threads; t++)
        if (!(c.buf[t] = malloc(DEDUP_READ))) goto oom;
    size_t k = 0;
    for (size_t f = 0; f < nfiles; f++) {
        uint64_t size = mp_source_entry(src, files[f])->size;
        for (uint64_t off = 0; off < size; off += DEDUP_ITEM) {
            item it = {(uint32_t)f, off, size - off < DEDUP_ITEM ? size - off : DEDUP_ITEM,
                       first[f] + off / MP_BLOCK_V1};
            c.items[k++] = it;
        }
    }
    if (mp_parallel_run(threads, nitems, work, &c, prog) != 0) {
        if (e) *e = c.err;
        goto done;
    }

    // Bloques repetidos: mismos (hash, longitud).
    hl *v = malloc((nblocks ? nblocks : 1) * sizeof *v);
    fk *keys = malloc((nfiles ? nfiles : 1) * sizeof *keys);
    if (!v || !keys) {
        free(v);
        free(keys);
        goto oom;
    }
    for (uint64_t b = 0; b < nblocks; b++) {
        v[b].h = c.hash[b];
        v[b].len = c.len[b];
    }
    qsort(v, (size_t)nblocks, sizeof *v, cmp_hl);
    for (uint64_t b = 1; b < nblocks; b++)
        if (v[b].h == v[b - 1].h && v[b].len == v[b - 1].len) {
            out->dup_blocks++;
            out->dup_block_bytes += v[b].len;
        }
    // Archivos idénticos: mismo tamaño y mismo XXH64 de la lista de hashes de sus bloques.
    for (size_t f = 0; f < nfiles; f++) {
        keys[f].size = mp_source_entry(src, files[f])->size;
        keys[f].key = mp_xxh64(c.hash + first[f], (size_t)(first[f + 1] - first[f]) * sizeof *c.hash, 0);
        keys[f].order = f;
    }
    qsort(keys, nfiles, sizeof *keys, cmp_fk);
    for (size_t f = 1; f < nfiles; f++)
        if (keys[f].size == keys[f - 1].size && keys[f].key == keys[f - 1].key) {
            out->dup_files++;
            out->dup_file_bytes += keys[f].size;
        }
    free(v);
    free(keys);
    out->blocks = nblocks;
    out->bytes_read = total;
    out->ns = mp_now_ns() - t0;
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "sin memoria");
done:
    if (c.buf)
        for (unsigned t = 0; t < threads; t++) free(c.buf[t]);
    free(c.buf);
    free(c.items);
    free(c.hash);
    free(c.len);
    mp_mutex_free(c.mu);
    free(first);
    return rc;
oom0:
    mp_err_set(e, "sin memoria");
    return -1;
}
