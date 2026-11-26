// SPDX-License-Identifier: GPL-3.0-or-later
// Zstd pack writer. Walks the files' 256 KiB blocks in the index's order (path_hash, path) and
// writes them in that same order, so the output does not depend on the number of threads. It is a
// pipeline: a read-ahead reads the files in that order in big pieces with a few threads and computes
// each file's SHA-256, one thread cuts the pieces into blocks, a pool that lives for the whole pack
// compresses them and this thread writes them, all at the same time.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_idx3.h"
#include "mp_platform.h"
#include "mp_readahead.h"
#include "mp_zpk.h"

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

typedef struct {
    uint64_t hash, size;
    const char *path;
    uint32_t src;    // source index
    size_t pos;      // position in files[] (for sha[])
    uint64_t first;  // first global block
} ford;

static int cmp_ford(const void *a, const void *b) {
    const ford *x = a, *y = b;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    return strcmp(x->path, y->path);
}

typedef struct {
    size_t len, out_len;
    int codec;
    uint8_t *in, *out;
} item;

typedef struct {
    item *items;
    ZSTD_CCtx **cctx;
    size_t out_cap;
    int level;
} cctx_set;

static int compress_work(void *vc, size_t i, unsigned tid) {
    cctx_set *c = vc;
    item *it = &c->items[i];
    size_t f = mp_zstd_block(c->cctx[tid], c->level, 18, it->in, it->len, it->out, c->out_cap);
    it->codec = f && f < it->len;
    it->out_len = it->codec ? f : it->len;
    return 0;
}

// The reading thread: fills the pipe's slots, in the index's order, with the blocks of the pieces
// that the read-ahead hands out (it reads the files in that order and hashes them). On an error (or
// a cancel) it stops the pipe.
typedef struct {
    mp_readahead *ra;
    item *items;
    mp_pipe *pipe;
    int failed;
    mp_err err;
} reader;

static void read_blocks(void *arg) {
    reader *r = arg;
    mp_readahead_piece pc;
    int rc;
    while ((rc = mp_readahead_next(r->ra, &pc, &r->err)) == 1) {
        for (size_t at = 0; at < pc.len; at += MP_BLOCK_V1) {
            long slot = mp_pipe_acquire(r->pipe);
            if (slot < 0) return;   // the writer stopped
            item *it = &r->items[slot];
            it->len = pc.len - at < MP_BLOCK_V1 ? pc.len - at : MP_BLOCK_V1;
            if (mp_cancel_check(&r->err) != 0) {
                r->failed = 1;
                mp_pipe_abort(r->pipe);
                return;
            }
            memcpy(it->in, pc.data + at, it->len);
            mp_pipe_submit(r->pipe, (size_t)slot, 1);
        }
    }
    if (rc < 0) {
        r->failed = 1;
        mp_pipe_abort(r->pipe);
        return;
    }
    mp_pipe_close(r->pipe);
}

// Blocks in the ring: enough for every thread to have several ahead of the one being written.
static size_t ring_slots(unsigned threads) { return (size_t)threads * 4 + 4; }

void mp_zpk_chunks_free(mp_zpk_chunks *c) {
    if (!c) return;
    free(c->chunks);
    free(c->first);
    free(c->vol_size);
    memset(c, 0, sizeof *c);
}

int mp_zpk_write_chunks(mp_source *src, const uint32_t *files, size_t n, const mp_zpk_opts *o,
                        const mp_zpk_sink *sink, mp_zpk_chunks *out, uint8_t (*sha)[32], mp_err *e) {
    memset(out, 0, sizeof *out);
    if (o->level < 1 || o->level > 22) {
        mp_err_set(e, "zstd level outside 1..22: %d", o->level);
        return -1;
    }
    if (o->volume_limit < MP_ZPK_VOL_HEADER + MP_BLOCK_V1 || o->volume_limit > MP_ZPK_VOLUME_LIMIT) {
        mp_err_set(e, "invalid volume limit");
        return -1;
    }
    if (o->first_volume >= MP_ZPK_MAX_VOLUMES) {
        mp_err_set(e, "the pack would need more than %u volumes", MP_ZPK_MAX_VOLUMES);
        return -1;
    }
    unsigned threads = o->threads ? o->threads : 1;
    ford *fo = malloc((n ? n : 1) * sizeof *fo);
    if (!fo) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        const mp_entry *en = mp_source_entry(src, files[i]);
        if (mp_pack_path_check(en->path, e) != 0) {
            free(fo);
            return -1;
        }
        fo[i].hash = mp_zpk_path_hash(en->path);
        fo[i].size = en->size;
        fo[i].path = en->path;
        fo[i].src = files[i];
        fo[i].pos = i;
    }
    qsort(fo, n, sizeof *fo, cmp_ford);
    uint64_t nchunks = 0, logical = 0;
    for (size_t i = 0; i < n; i++) {
        if (i && strcmp(fo[i - 1].path, fo[i].path) == 0) {
            mp_err_set(e, "repeated path: %s", fo[i].path);
            free(fo);
            return -1;
        }
        fo[i].first = nchunks;
        nchunks += (fo[i].size + MP_BLOCK_V1 - 1) / MP_BLOCK_V1;
        logical += fo[i].size;
    }
    if (nchunks >= 0xFFFFFFFFull) {
        mp_err_set(e, "too many blocks for a pack (%llu)", (unsigned long long)nchunks);
        free(fo);
        return -1;
    }

    size_t nslots = ring_slots(threads);
    uint64_t *chunk_rec = malloc((size_t)(nchunks ? nchunks : 1) * sizeof *chunk_rec);
    uint64_t *vol_size = calloc(MP_ZPK_MAX_VOLUMES, sizeof *vol_size);
    uint64_t *first = malloc((n ? n : 1) * sizeof *first);
    item *items = calloc(nslots, sizeof *items);
    uint8_t *inbuf = malloc(nslots * MP_BLOCK_V1);
    cctx_set cs = {items, calloc(threads, sizeof(ZSTD_CCtx *)), ZSTD_compressBound(MP_BLOCK_V1), o->level};
    uint8_t *outbuf = malloc(nslots * cs.out_cap);
    uint32_t *order = malloc((n ? n : 1) * sizeof *order);
    uint8_t (*osha)[32] = malloc((n ? n : 1) * sizeof *osha);   // in the index's order
    mp_pipe *pipe = NULL;
    mp_thread *rthread = NULL;
    reader rd;
    memset(&rd, 0, sizeof rd);
    int rc = -1, failed = 0;
    uint64_t gchunk = 0, stored_total = 0, cur_off = 0;
    int64_t cur = -1;   // the volume being written, counted from o->first_volume
    uint32_t base = o->first_volume;
    if (!chunk_rec || !vol_size || !first || !items || !inbuf || !cs.cctx || !outbuf || !order || !osha) goto oom;
    for (unsigned t = 0; t < threads; t++)
        if (!(cs.cctx[t] = ZSTD_createCCtx())) goto oom;
    for (size_t b = 0; b < nslots; b++) {
        items[b].in = inbuf + b * MP_BLOCK_V1;
        items[b].out = outbuf + b * cs.out_cap;
    }
    if (mp_cancel_check(e) != 0) goto done;
    if (!(pipe = mp_pipe_new(threads, nslots, compress_work, &cs))) goto oom;
    for (size_t i = 0; i < n; i++) order[i] = fo[i].src;
    mp_readahead_opts ro;
    mp_readahead_opts_default(&ro);
    // A compressed image (.ffpfsc) is read by more threads: each one inflates what it reads.
    if (mp_source_readers(src) > ro.threads) ro.threads = mp_source_readers(src);
    if (ro.threads > threads) ro.threads = threads;   // --threads 1 reads with one thread too
    if (mp_readahead_open(src, order, n, osha, &ro, &rd.ra, e) != 0) goto done;
    rd.items = items;
    rd.pipe = pipe;
    if (mp_thread_start(&rthread, read_blocks, &rd) != 0) {
        mp_err_set(e, "could not start the reading thread");
        goto done;
    }

    // Writes the blocks in order as they come out of the pipe, opening volumes when the current one
    // fills up.
    for (;;) {
        int wrc = 0;
        long slot = mp_pipe_next(pipe, 1, &wrc);
        if (slot < 0) break;   // everything was read, or the reader stopped
        item *it = &items[slot];
        failed = 1;
        if (wrc != 0) {
            if (mp_cancel_check(e) == 0) mp_err_set(e, "compression error");
            break;
        }
        if (mp_cancel_check(e) != 0) break;
        if (cur < 0 || cur_off + it->out_len > o->volume_limit) {
            if (cur >= 0) {
                vol_size[cur] = cur_off;
                if (sink->vol_end(sink->ctx, base + (uint32_t)cur, e) != 0) break;
            }
            if (base + (uint64_t)(++cur) >= MP_ZPK_MAX_VOLUMES) {
                mp_err_set(e, "the pack would need more than %u volumes", MP_ZPK_MAX_VOLUMES);
                break;
            }
            if (sink->vol_begin(sink->ctx, base + (uint32_t)cur, e) != 0) break;
            cur_off = MP_ZPK_VOL_HEADER;
        }
        if (sink->vol_write(sink->ctx, it->codec ? it->out : it->in, it->out_len, e) != 0) break;
        chunk_rec[gchunk++] = cur_off | (uint64_t)(base + (uint32_t)cur) << 32 | (uint64_t)(it->out_len - 1) << 44 |
                              (uint64_t)it->codec << 62;
        cur_off += it->out_len;
        stored_total += it->out_len;
        size_t len = it->len;
        mp_pipe_release(pipe, (size_t)slot);
        failed = 0;
        mp_progress_add(o->prog, len);
        mp_progress_maybe_print(o->prog);
    }
    if (failed) mp_pipe_abort(pipe);   // the reader may be waiting for a free slot
    mp_thread_join(rthread);
    if (failed) goto done;
    if (rd.failed) {
        if (e) *e = rd.err;
        goto done;
    }
    for (size_t i = 0; i < n; i++) memcpy(sha[fo[i].pos], osha[i], 32);
    if (gchunk != nchunks) {   // cannot happen: the reader closes the pipe after the last block
        mp_err_set(e, "the pack lost blocks (%llu of %llu)", (unsigned long long)gchunk,
                   (unsigned long long)nchunks);
        goto done;
    }
    if (cur >= 0) {
        vol_size[cur] = cur_off;
        if (sink->vol_end(sink->ctx, base + (uint32_t)cur, e) != 0) goto done;
    }
    for (size_t i = 0; i < n; i++) first[fo[i].pos] = fo[i].first;
    out->chunks = chunk_rec;
    out->nchunks = nchunks;
    out->first = first;
    out->vol_size = vol_size;
    out->nvol = (uint32_t)(cur + 1);
    out->logical = logical;
    out->stored = stored_total;
    chunk_rec = NULL;
    first = NULL;
    vol_size = NULL;
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "out of memory");
done:
    mp_readahead_close(rd.ra);   // its threads may still be reading if the writer stopped
    mp_pipe_free(pipe);          // joins the compression threads before their buffers are freed
    if (cs.cctx)
        for (unsigned t = 0; t < threads; t++) ZSTD_freeCCtx(cs.cctx[t]);
    free(cs.cctx);
    free(chunk_rec);
    free(vol_size);
    free(first);
    free(items);
    free(inbuf);
    free(outbuf);
    free(order);
    free(osha);
    free(fo);
    return rc;
}

typedef struct {
    uint64_t hash;
    const mp_zpk_entry *en;
} bent;

static int cmp_bent(const void *a, const void *b) {
    const bent *x = a, *y = b;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    return strcmp(x->en->path, y->en->path);
}

void mp_zpk_volume_header(uint32_t vol, uint32_t nvol, const uint8_t aid[16], uint64_t size, uint8_t h[64]) {
    memset(h, 0, 64);
    memcpy(h, "AMPRZDT1", 8);
    wr32(h + 8, 1);
    wr32(h + 12, MP_ZPK_VOL_HEADER);
    wr32(h + 16, vol);
    wr32(h + 20, nvol);
    memcpy(h + 24, aid, 16);
    wr64(h + 40, size - MP_ZPK_VOL_HEADER);
    wr32(h + 60, mp_crc32(h, 64, 0));
}

int mp_zpk_build_index(mp_zpk_entry *v, size_t n, const uint64_t *vol_size, uint32_t nvol, int level,
                       const uint8_t *archive_id, uint8_t **index, size_t *len, uint8_t aid[16], mp_err *e) {
    *index = NULL;
    *len = 0;
    if (level < 1 || level > 22 || nvol > MP_ZPK_MAX_VOLUMES) {
        mp_err_set(e, "zstd level outside 1..22 or too many volumes");
        return -1;
    }
    bent *b = malloc((n ? n : 1) * sizeof *b);
    if (!b) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    uint64_t nchunks = 0, strings = 0, logical = 0;
    for (size_t i = 0; i < n; i++) {
        if (mp_pack_path_check(v[i].path, e) != 0) {
            free(b);
            return -1;
        }
        b[i].hash = mp_zpk_path_hash(v[i].path);
        b[i].en = &v[i];
        nchunks += (v[i].size + MP_BLOCK_V1 - 1) / MP_BLOCK_V1;
        strings += strlen(v[i].path);
        logical += v[i].size;
    }
    qsort(b, n, sizeof *b, cmp_bent);
    for (size_t i = 1; i < n; i++)
        if (!strcmp(b[i - 1].en->path, b[i].en->path)) {
            mp_err_set(e, "repeated path: %s", b[i].en->path);
            free(b);
            return -1;
        }
    if (nchunks >= 0xFFFFFFFFull || (nchunks == 0) != (nvol == 0)) {
        mp_err_set(e, nchunks ? "too many blocks for a pack (%llu)" : "a pack with volumes and no blocks",
                   (unsigned long long)nchunks);
        free(b);
        return -1;
    }
    // Index: header, files, blocks, volumes, strings.
    uint64_t chunks_off = MP_ZPK_HEADER + 32ull * n, vols_off = chunks_off + 8 * nchunks,
             strings_off = vols_off + 8ull * nvol, total = strings_off + strings;
    uint8_t *d = calloc(1, (size_t)total);
    if (!d) {
        free(b);
        mp_err_set(e, "out of memory");
        return -1;
    }
    uint32_t path_off = 0;
    uint64_t at = 0;
    for (size_t i = 0; i < n; i++) {
        const mp_zpk_entry *en = b[i].en;
        uint8_t *r = d + MP_ZPK_HEADER + 32 * i;
        size_t pl = strlen(en->path);
        uint64_t cnt = (en->size + MP_BLOCK_V1 - 1) / MP_BLOCK_V1;
        wr64(r, b[i].hash);
        wr64(r + 8, en->size);
        wr32(r + 16, (uint32_t)at);
        wr32(r + 20, path_off);
        wr16(r + 24, (uint16_t)pl);
        memcpy(d + strings_off + path_off, en->path, pl);
        path_off += (uint32_t)pl;
        for (uint64_t k = 0; k < cnt; k++) wr64(d + chunks_off + 8 * (at + k), en->chunks[k]);
        at += cnt;
    }
    for (uint32_t k = 0; k < nvol; k++) wr64(d + vols_off + 8ull * k, vol_size[k]);
    free(b);
    if (archive_id) {
        memcpy(aid, archive_id, 16);
    } else {   // deterministic archive_id: SHA-256("MAXPACK1" + tables).
        mp_sha256 s;
        uint8_t digest[32];
        mp_sha256_init(&s);
        mp_sha256_update(&s, "MAXPACK1", 8);
        mp_sha256_update(&s, d + MP_ZPK_HEADER, (size_t)(total - MP_ZPK_HEADER));
        mp_sha256_final(&s, digest);
        memcpy(aid, digest, 16);
    }
    memcpy(d, "AMPRZPK1", 8);
    wr32(d + 8, 1);
    wr32(d + 12, MP_ZPK_HEADER);
    d[16] = 18;
    d[17] = (uint8_t)level;
    wr32(d + 20, nvol);
    memcpy(d + 24, aid, 16);
    wr64(d + 40, n);
    wr64(d + 48, nchunks);
    wr64(d + 56, MP_ZPK_HEADER);
    wr64(d + 64, chunks_off);
    wr64(d + 72, vols_off);
    wr64(d + 80, strings_off);
    wr64(d + 88, strings);
    wr64(d + 96, logical);
    uint32_t body_crc = mp_crc32(d + MP_ZPK_HEADER, (size_t)(total - MP_ZPK_HEADER), 0);
    uint32_t head_crc = mp_crc32(d, MP_ZPK_HEADER, 0);   // with 112..120 still zero
    wr32(d + 112, body_crc);
    wr32(d + 116, head_crc);
    *index = d;
    *len = (size_t)total;
    return 0;
}

int mp_zpk_write(mp_source *src, const uint32_t *files, size_t n, const mp_zpk_opts *o,
                 const mp_zpk_sink *sink, uint8_t **index, size_t *index_len, uint8_t (*sha)[32],
                 mp_zpk_stats *st, mp_err *e) {
    *index = NULL;
    *index_len = 0;
    memset(st, 0, sizeof *st);
    mp_zpk_opts oo = *o;
    oo.first_volume = 0;
    mp_zpk_chunks c;
    if (mp_zpk_write_chunks(src, files, n, &oo, sink, &c, sha, e) != 0) return -1;
    mp_zpk_entry *v = malloc((n ? n : 1) * sizeof *v);
    int rc = -1;
    uint8_t aid[16];
    if (!v) {
        mp_err_set(e, "out of memory");
        goto done;
    }
    for (size_t k = 0; k < n; k++) {
        const mp_entry *en = mp_source_entry(src, files[k]);
        v[k].path = en->path;
        v[k].size = en->size;
        v[k].chunks = c.chunks + c.first[k];
    }
    if (mp_zpk_build_index(v, n, c.vol_size, c.nvol, o->level, NULL, index, index_len, aid, e) != 0) goto done;
    // Volume headers.
    for (uint32_t k = 0; k < c.nvol; k++) {
        uint8_t h[64];
        mp_zpk_volume_header(k, c.nvol, aid, c.vol_size[k], h);
        if (sink->vol_header(sink->ctx, k, h, e) != 0) {
            free(*index);
            *index = NULL;
            *index_len = 0;
            goto done;
        }
    }
    st->files = n;
    st->chunks = c.nchunks;
    st->volumes = c.nvol;
    st->logical = c.logical;
    st->stored = c.stored;
    memcpy(st->archive_id, aid, 16);
    rc = 0;
done:
    free(v);
    mp_zpk_chunks_free(&c);
    return rc;
}

// ---- Volumes in a folder ---------------------------------------------------------------------

typedef struct {
    char *dir;
    mp_wfile *cur;
} dirsink;

static void vol_path(const dirsink *d, uint32_t vol, char *out, size_t cap) {
    snprintf(out, cap, "%s/ampr_zstd-%03u.pak", d->dir, vol);
}

static int ds_begin(void *ctx, uint32_t vol, mp_err *e) {
    dirsink *d = ctx;
    char p[2048];
    vol_path(d, vol, p, sizeof p);
    static const uint8_t zero[MP_ZPK_VOL_HEADER] = {0};
    if (mp_wfile_create(p, &d->cur, e) != 0) return -1;
    return mp_wfile_write(d->cur, zero, sizeof zero, e);
}

static int ds_write(void *ctx, const void *p, size_t n, mp_err *e) {
    return mp_wfile_write(((dirsink *)ctx)->cur, p, n, e);
}

static int ds_end(void *ctx, uint32_t vol, mp_err *e) {
    dirsink *d = ctx;
    (void)vol;
    int rc = mp_wfile_close(d->cur, e);
    d->cur = NULL;
    return rc;
}

static int ds_header(void *ctx, uint32_t vol, const uint8_t hdr[64], mp_err *e) {
    dirsink *d = ctx;
    char p[2048];
    vol_path(d, vol, p, sizeof p);
    mp_wfile *f = NULL;
    if (mp_wfile_reopen(p, &f, e) != 0) return -1;
    int rc = mp_wfile_pwrite(f, hdr, 64, 0, e);
    if (mp_wfile_close(f, e) != 0) rc = -1;
    return rc;
}

int mp_zpk_sink_dir(const char *dir, mp_zpk_sink *out, mp_err *e) {
    memset(out, 0, sizeof *out);
    dirsink *d = calloc(1, sizeof *d);
    if (!d || !(d->dir = mp_strdup(dir))) {
        free(d);
        mp_err_set(e, "out of memory");
        return -1;
    }
    out->vol_begin = ds_begin;
    out->vol_write = ds_write;
    out->vol_end = ds_end;
    out->vol_header = ds_header;
    out->ctx = d;
    return 0;
}

void mp_zpk_sink_free(mp_zpk_sink *s) {
    dirsink *d = s ? s->ctx : NULL;
    if (!d) return;
    if (d->cur) mp_wfile_close(d->cur, NULL);
    free(d->dir);
    free(d);
    s->ctx = NULL;
}
