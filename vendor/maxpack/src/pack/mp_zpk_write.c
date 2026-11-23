// SPDX-License-Identifier: GPL-3.0-or-later
// Escritor de packs zstd. Recorre los bloques de 256 KiB de los archivos en el orden del índice
// (path_hash, ruta), los comprime por lotes en paralelo y los escribe en ese mismo orden, así que
// la salida no depende del número de hilos.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_idx3.h"
#include "mp_platform.h"
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
    uint32_t src;    // índice de origen
    size_t pos;      // posición en files[] (para sha[])
    uint64_t first;  // primer bloque global
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

int mp_zpk_write(mp_source *src, const uint32_t *files, size_t n, const mp_zpk_opts *o,
                 const mp_zpk_sink *sink, uint8_t **index, size_t *index_len, uint8_t (*sha)[32],
                 mp_zpk_stats *st, mp_err *e) {
    *index = NULL;
    *index_len = 0;
    memset(st, 0, sizeof *st);
    if (o->level < 1 || o->level > 22) {
        mp_err_set(e, "nivel de zstd fuera de 1..22: %d", o->level);
        return -1;
    }
    if (o->volume_limit < MP_ZPK_VOL_HEADER + MP_BLOCK_V1 || o->volume_limit > MP_ZPK_VOLUME_LIMIT) {
        mp_err_set(e, "límite de volumen no válido");
        return -1;
    }
    unsigned threads = o->threads ? o->threads : 1;
    ford *fo = malloc((n ? n : 1) * sizeof *fo);
    if (!fo) {
        mp_err_set(e, "sin memoria");
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
    uint64_t nchunks = 0, strings = 0, logical = 0;
    for (size_t i = 0; i < n; i++) {
        if (i && strcmp(fo[i - 1].path, fo[i].path) == 0) {
            mp_err_set(e, "ruta repetida: %s", fo[i].path);
            free(fo);
            return -1;
        }
        fo[i].first = nchunks;
        nchunks += (fo[i].size + MP_BLOCK_V1 - 1) / MP_BLOCK_V1;
        strings += strlen(fo[i].path);
        logical += fo[i].size;
    }
    if (nchunks >= 0xFFFFFFFFull) {
        mp_err_set(e, "demasiados bloques para un pack (%llu)", (unsigned long long)nchunks);
        free(fo);
        return -1;
    }

    size_t batch = (size_t)threads * 8;
    uint64_t *chunk_rec = malloc((size_t)(nchunks ? nchunks : 1) * sizeof *chunk_rec);
    uint64_t *vol_size = calloc(MP_ZPK_MAX_VOLUMES, sizeof *vol_size);
    item *items = calloc(batch, sizeof *items);
    uint8_t *inbuf = malloc(batch * MP_BLOCK_V1);
    cctx_set cs = {items, calloc(threads, sizeof(ZSTD_CCtx *)), ZSTD_compressBound(MP_BLOCK_V1), o->level};
    uint8_t *outbuf = malloc(batch * cs.out_cap);
    mp_sha256 *shs = malloc((n ? n : 1) * sizeof *shs);
    int rc = -1;
    if (!chunk_rec || !vol_size || !items || !inbuf || !cs.cctx || !outbuf || !shs) goto oom;
    for (unsigned t = 0; t < threads; t++)
        if (!(cs.cctx[t] = ZSTD_createCCtx())) goto oom;
    for (size_t b = 0; b < batch; b++) {
        items[b].in = inbuf + b * MP_BLOCK_V1;
        items[b].out = outbuf + b * cs.out_cap;
    }

    // Recorrido en orden del índice: archivo fi, bloque k.
    size_t fi = 0;
    uint64_t k = 0, gchunk = 0, stored_total = 0, cur_off = 0;
    int64_t cur_vol = -1;
    if (n) mp_sha256_init(&shs[0]);
    while (fi < n) {
        if (mp_cancel_check(e) != 0) goto done;
        // 1. Leer un lote (puede cruzar archivos) y actualizar los SHA-256.
        size_t nb = 0;
        uint64_t first_g = gchunk;
        while (nb < batch && fi < n) {
            uint64_t size = fo[fi].size, off = k * MP_BLOCK_V1;
            if (off >= size) {   // archivo terminado (o vacío)
                mp_sha256_final(&shs[fi], sha[fo[fi].pos]);
                fi++;
                k = 0;
                if (fi < n) mp_sha256_init(&shs[fi]);
                continue;
            }
            size_t len = size - off < MP_BLOCK_V1 ? (size_t)(size - off) : MP_BLOCK_V1;
            if (mp_source_read(src, fo[fi].src, off, items[nb].in, len, e) != 0) goto done;
            mp_sha256_update(&shs[fi], items[nb].in, len);
            items[nb].len = len;
            nb++;
            k++;
            gchunk++;
        }
        if (!nb) break;
        // 2. Comprimir en paralelo.
        if (mp_parallel_run(threads, nb, compress_work, &cs, NULL) != 0) {
            if (mp_cancel_check(e) == 0) mp_err_set(e, "error al comprimir");
            goto done;
        }
        // 3. Escribir en orden, abriendo volúmenes cuando el actual se llena.
        for (size_t b = 0; b < nb; b++) {
            item *it = &items[b];
            if (cur_vol < 0 || cur_off + it->out_len > o->volume_limit) {
                if (cur_vol >= 0) {
                    vol_size[cur_vol] = cur_off;
                    if (sink->vol_end(sink->ctx, (uint32_t)cur_vol, e) != 0) goto done;
                }
                if (++cur_vol >= (int64_t)MP_ZPK_MAX_VOLUMES) {
                    mp_err_set(e, "el pack necesitaría más de %u volúmenes", MP_ZPK_MAX_VOLUMES);
                    goto done;
                }
                if (sink->vol_begin(sink->ctx, (uint32_t)cur_vol, e) != 0) goto done;
                cur_off = MP_ZPK_VOL_HEADER;
            }
            if (sink->vol_write(sink->ctx, it->codec ? it->out : it->in, it->out_len, e) != 0) goto done;
            chunk_rec[first_g + b] = cur_off | (uint64_t)cur_vol << 32 | (uint64_t)(it->out_len - 1) << 44 |
                                     (uint64_t)it->codec << 62;
            cur_off += it->out_len;
            stored_total += it->out_len;
        }
        uint64_t batch_bytes = 0;
        for (size_t b = 0; b < nb; b++) batch_bytes += items[b].len;
        mp_progress_add(o->prog, batch_bytes);
        mp_progress_maybe_print(o->prog);
    }
    uint32_t nvol = (uint32_t)(cur_vol + 1);
    if (cur_vol >= 0) {
        vol_size[cur_vol] = cur_off;
        if (sink->vol_end(sink->ctx, (uint32_t)cur_vol, e) != 0) goto done;
    }

    // 4. Índice: cabecera, archivos, bloques, volúmenes, cadenas.
    uint64_t chunks_off = MP_ZPK_HEADER + 32ull * n, vols_off = chunks_off + 8 * nchunks,
             strings_off = vols_off + 8ull * nvol, total = strings_off + strings;
    uint8_t *d = calloc(1, (size_t)total);
    if (!d) goto oom;
    uint32_t path_off = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t *r = d + MP_ZPK_HEADER + 32 * i;
        size_t pl = strlen(fo[i].path);
        wr64(r, fo[i].hash);
        wr64(r + 8, fo[i].size);
        wr32(r + 16, (uint32_t)fo[i].first);
        wr32(r + 20, path_off);
        wr16(r + 24, (uint16_t)pl);
        memcpy(d + strings_off + path_off, fo[i].path, pl);
        path_off += (uint32_t)pl;
    }
    for (uint64_t c = 0; c < nchunks; c++) wr64(d + chunks_off + 8 * c, chunk_rec[c]);
    for (uint32_t v = 0; v < nvol; v++) wr64(d + vols_off + 8ull * v, vol_size[v]);
    // archive_id determinista: SHA-256("MAXPACK1" + tablas).
    mp_sha256 s;
    uint8_t digest[32];
    mp_sha256_init(&s);
    mp_sha256_update(&s, "MAXPACK1", 8);
    mp_sha256_update(&s, d + MP_ZPK_HEADER, (size_t)(total - MP_ZPK_HEADER));
    mp_sha256_final(&s, digest);
    memcpy(d, "AMPRZPK1", 8);
    wr32(d + 8, 1);
    wr32(d + 12, MP_ZPK_HEADER);
    d[16] = 18;
    d[17] = (uint8_t)o->level;
    wr32(d + 20, nvol);
    memcpy(d + 24, digest, 16);
    wr64(d + 40, n);
    wr64(d + 48, nchunks);
    wr64(d + 56, MP_ZPK_HEADER);
    wr64(d + 64, chunks_off);
    wr64(d + 72, vols_off);
    wr64(d + 80, strings_off);
    wr64(d + 88, strings);
    wr64(d + 96, logical);
    uint32_t body_crc = mp_crc32(d + MP_ZPK_HEADER, (size_t)(total - MP_ZPK_HEADER), 0);
    uint32_t head_crc = mp_crc32(d, MP_ZPK_HEADER, 0);   // con 112..120 todavía a cero
    wr32(d + 112, body_crc);
    wr32(d + 116, head_crc);
    // 5. Cabeceras de volumen.
    for (uint32_t v = 0; v < nvol; v++) {
        uint8_t h[64] = {0};
        memcpy(h, "AMPRZDT1", 8);
        wr32(h + 8, 1);
        wr32(h + 12, MP_ZPK_VOL_HEADER);
        wr32(h + 16, v);
        wr32(h + 20, nvol);
        memcpy(h + 24, digest, 16);
        wr64(h + 40, vol_size[v] - MP_ZPK_VOL_HEADER);
        wr32(h + 60, mp_crc32(h, 64, 0));
        if (sink->vol_header(sink->ctx, v, h, e) != 0) {
            free(d);
            goto done;
        }
    }
    st->files = n;
    st->chunks = nchunks;
    st->volumes = nvol;
    st->logical = logical;
    st->stored = stored_total;
    memcpy(st->archive_id, digest, 16);
    *index = d;
    *index_len = (size_t)total;
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "sin memoria");
done:
    if (cs.cctx)
        for (unsigned t = 0; t < threads; t++) ZSTD_freeCCtx(cs.cctx[t]);
    free(cs.cctx);
    free(chunk_rec);
    free(vol_size);
    free(items);
    free(inbuf);
    free(outbuf);
    free(shs);
    free(fo);
    return rc;
}

// ---- Volúmenes en una carpeta ----------------------------------------------------------------

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
        mp_err_set(e, "sin memoria");
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
