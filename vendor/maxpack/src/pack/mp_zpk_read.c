// SPDX-License-Identifier: GPL-3.0-or-later
// Lector de packs zstd: valida el índice y los volúmenes como ampr-emu-ng y Forge, y decodifica.
#define ZSTD_STATIC_LINKING_ONLY
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "mp_hash.h"
#include "mp_idx3.h"
#include "mp_platform.h"
#include "mp_zpk.h"

typedef struct {
    uint64_t hash, size;
    uint32_t first, flags;
    char *path;
} zfile;

struct mp_zpk {
    zfile *f;
    size_t n;
    uint64_t *chunks;
    uint64_t nchunks;
    uint64_t *vols;
    uint32_t nvol;
    mp_zpk_volsrc vs;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int bad(mp_err *e, const char *why) {
    mp_err_set(e, "ampr_zstd.index no válido: %s", why);
    return -1;
}

#define C_OFF(c) ((c) & 0xFFFFFFFFull)
#define C_VOL(c) ((uint32_t)(((c) >> 32) & 0xFFF))
#define C_STORED(c) ((uint32_t)(((c) >> 44) & 0x3FFFF) + 1)
#define C_CODEC(c) ((int)((c) >> 62))

void mp_zpk_close(mp_zpk *z) {
    if (!z) return;
    for (size_t i = 0; i < z->n; i++) free(z->f[i].path);
    free(z->f);
    free(z->chunks);
    free(z->vols);
    free(z);
}

static int check_volume(mp_zpk *z, uint32_t v, const uint8_t aid[16], mp_err *e) {
    uint8_t h[64];
    if (z->vs.size(z->vs.ctx, v) != z->vols[v]) {
        mp_err_set(e, "el volumen ampr_zstd-%03u.pak no existe o no mide lo que dice el índice", v);
        return -1;
    }
    if (z->vs.read(z->vs.ctx, v, 0, h, 64, e) != 0) return -1;
    uint32_t crc = rd32(h + 60);
    uint8_t hz[64];
    memcpy(hz, h, 64);
    memset(hz + 60, 0, 4);
    int ok = memcmp(h, "AMPRZDT1", 8) == 0 && rd32(h + 8) == 1 && rd32(h + 12) == 64 && rd32(h + 16) == v &&
             rd32(h + 20) == z->nvol && memcmp(h + 24, aid, 16) == 0 && rd64(h + 40) == z->vols[v] - 64;
    for (int i = 48; i < 60 && ok; i++) ok = h[i] == 0;
    if (!ok) {
        mp_err_set(e, "cabecera del volumen ampr_zstd-%03u.pak no válida", v);
        return -1;
    }
    if (mp_crc32(hz, 64, 0) != crc) {
        mp_err_set(e, "CRC de la cabecera del volumen ampr_zstd-%03u.pak", v);
        return -1;
    }
    return 0;
}

int mp_zpk_open(const uint8_t *d, size_t len, const mp_zpk_volsrc *vs, mp_zpk **out, mp_err *e) {
    *out = NULL;
    if (len < MP_ZPK_HEADER || memcmp(d, "AMPRZPK1", 8) != 0) return bad(e, "cabecera");
    if (rd32(d + 8) != 1 || rd32(d + 12) != MP_ZPK_HEADER || d[16] != 18 || d[17] < 1 || d[17] > 22 ||
        rd16(d + 18) != 0 || rd64(d + 104) != 0 || rd64(d + 120) != 0)
        return bad(e, "versión, tamaño de bloque, nivel o campos reservados");
    uint8_t hz[MP_ZPK_HEADER];
    memcpy(hz, d, MP_ZPK_HEADER);
    memset(hz + 112, 0, 8);
    if (mp_crc32(hz, MP_ZPK_HEADER, 0) != rd32(d + 116)) return bad(e, "CRC de la cabecera");
    if (mp_crc32(d + MP_ZPK_HEADER, len - MP_ZPK_HEADER, 0) != rd32(d + 112)) return bad(e, "CRC de las tablas");
    uint32_t nvol = rd32(d + 20);
    uint64_t nf = rd64(d + 40), nc = rd64(d + 48), foff = rd64(d + 56), coff = rd64(d + 64), voff = rd64(d + 72),
             soff = rd64(d + 80), slen = rd64(d + 88), logical = rd64(d + 96);
    // Sin sumas que puedan dar la vuelta: cada parte se compara con lo que queda del archivo.
    if (nvol > MP_ZPK_MAX_VOLUMES || nc >= 0xFFFFFFFFull || nf > len / 32 || nc > len / 8 ||
        foff != MP_ZPK_HEADER || coff != foff + 32 * nf || voff != coff + 8 * nc ||
        soff != voff + 8ull * nvol || soff > len || slen != len - soff || (nc == 0) != (nvol == 0))
        return bad(e, "posición o tamaño de las tablas");
    mp_zpk *z = calloc(1, sizeof *z);
    if (!z) {
        mp_err_set(e, "sin memoria");
        return -1;
    }
    z->vs = *vs;
    z->n = (size_t)nf;
    z->nchunks = nc;
    z->nvol = nvol;
    z->f = calloc(z->n ? z->n : 1, sizeof *z->f);
    z->chunks = malloc((size_t)(nc ? nc : 1) * sizeof *z->chunks);
    z->vols = malloc((nvol ? nvol : 1) * sizeof *z->vols);
    if (!z->f || !z->chunks || !z->vols) {
        mp_zpk_close(z);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    for (uint64_t c = 0; c < nc; c++) z->chunks[c] = rd64(d + coff + 8 * c);
    for (uint32_t v = 0; v < nvol; v++) z->vols[v] = rd64(d + voff + 8ull * v);
    int rc = 0;
    uint64_t want_first = 0, want_off = 0, sum = 0;
    for (size_t i = 0; i < z->n && !rc; i++) {
        const uint8_t *r = d + foff + 32 * i;
        zfile *f = &z->f[i];
        f->hash = rd64(r);
        f->size = rd64(r + 8);
        f->first = rd32(r + 16);
        uint32_t po = rd32(r + 20), pl = rd16(r + 24);
        f->flags = rd16(r + 26);
        if (f->size > 0xFFFFFFFEull * MP_BLOCK_V1) {   // más bloques de los que caben en el índice
            rc = bad(e, "tamaño de archivo imposible");
            break;
        }
        if (rd32(r + 28) != 0 || (f->flags & ~1u) || pl == 0 || pl >= 1024 || po != want_off ||
            (uint64_t)po + pl > slen || f->first != want_first) {
            rc = bad(e, "registro de archivo");
            break;
        }
        f->path = malloc(pl + 1u);
        if (!f->path) {
            rc = -1;
            mp_err_set(e, "sin memoria");
            break;
        }
        memcpy(f->path, d + soff + po, pl);
        f->path[pl] = 0;
        if (memchr(f->path, 0, pl) || mp_pack_path_check(f->path, e) != 0) {
            rc = -1;
            break;
        }
        if (mp_zpk_path_hash(f->path) != f->hash) {
            rc = bad(e, "hash de ruta");
            break;
        }
        if (i && (z->f[i - 1].hash > f->hash ||
                  (z->f[i - 1].hash == f->hash && strcmp(z->f[i - 1].path, f->path) >= 0))) {
            rc = bad(e, "registros desordenados o repetidos");
            break;
        }
        want_off += pl;
        want_first += (f->size + MP_BLOCK_V1 - 1) / MP_BLOCK_V1;
        sum += f->size;
    }
    if (!rc && (want_first != nc || want_off != slen || sum != logical))
        rc = bad(e, "número de bloques, cadenas o bytes totales");
    // Bloques: seguidos desde 64 en el orden del índice, con su códec y tamaño.
    uint32_t cv = 0;
    uint64_t coff_v = MP_ZPK_VOL_HEADER;
    for (size_t i = 0; i < z->n && !rc; i++) {
        const zfile *f = &z->f[i];
        uint64_t cnt = (f->size + MP_BLOCK_V1 - 1) / MP_BLOCK_V1;
        for (uint64_t k = 0; k < cnt && !rc; k++) {
            uint64_t c = z->chunks[f->first + k];
            uint64_t want = f->size - k * MP_BLOCK_V1 < MP_BLOCK_V1 ? f->size - k * MP_BLOCK_V1 : MP_BLOCK_V1;
            if (C_VOL(c) >= z->nvol) {
                rc = bad(e, "bloque en un volumen inexistente");
                break;
            }
            if (C_VOL(c) != cv) {
                if (C_VOL(c) != cv + 1 || coff_v != z->vols[cv]) {
                    rc = bad(e, "bloques que no van seguidos entre volúmenes");
                    break;
                }
                cv++;
                coff_v = MP_ZPK_VOL_HEADER;
            }
            if (C_OFF(c) != coff_v || C_CODEC(c) > 1 || (C_CODEC(c) == 0 && C_STORED(c) != want) ||
                (C_CODEC(c) == 1 && C_STORED(c) >= want) || ((f->flags & 1) && C_CODEC(c) != 0)) {
                rc = bad(e, "bloque fuera de sitio, con códec o tamaño no válido");
                break;
            }
            coff_v += C_STORED(c);
        }
    }
    if (!rc && nvol && (cv != nvol - 1 || coff_v != z->vols[cv])) rc = bad(e, "tamaño de los volúmenes");
    for (uint32_t v = 0; v < nvol && !rc; v++) rc = check_volume(z, v, d + 24, e);
    if (rc) {
        mp_zpk_close(z);
        return -1;
    }
    *out = z;
    return 0;
}

size_t mp_zpk_count(const mp_zpk *z) { return z->n; }
const char *mp_zpk_path(const mp_zpk *z, size_t i) { return i < z->n ? z->f[i].path : NULL; }
uint64_t mp_zpk_size(const mp_zpk *z, size_t i) { return i < z->n ? z->f[i].size : 0; }
uint32_t mp_zpk_volume_count(const mp_zpk *z) { return z->nvol; }

long mp_zpk_find(const mp_zpk *z, const char *path) {
    uint64_t h = mp_zpk_path_hash(path);
    size_t lo = 0, hi = z->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const zfile *f = &z->f[mid];
        int c = f->hash != h ? (f->hash < h ? -1 : 1) : strcmp(f->path, path);
        if (c == 0) return (long)mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

int mp_zpk_chunk(const mp_zpk *z, size_t i, uint32_t k, uint32_t *vol, uint64_t *off, uint32_t *stored,
                 int *codec) {
    if (i >= z->n || (uint64_t)k * MP_BLOCK_V1 >= z->f[i].size) return -1;
    uint64_t c = z->chunks[z->f[i].first + k];
    if (vol) *vol = C_VOL(c);
    if (off) *off = C_OFF(c);
    if (stored) *stored = C_STORED(c);
    if (codec) *codec = C_CODEC(c);
    return 0;
}

int mp_zpk_read_file(mp_zpk *z, size_t i, uint64_t off, void *buf, size_t n, mp_err *e) {
    if (i >= z->n || off > z->f[i].size || n > z->f[i].size - off) {
        mp_err_set(e, "lectura fuera del archivo empaquetado");
        return -1;
    }
    if (n == 0) return 0;
    const zfile *f = &z->f[i];
    uint8_t *src = malloc(MP_BLOCK_V1), *dec = malloc(MP_BLOCK_V1);
    ZSTD_DCtx *dc = ZSTD_createDCtx();
    int rc = 0;
    if (!src || !dec || !dc) {
        mp_err_set(e, "sin memoria");
        rc = -1;
    }
    uint8_t *o = buf;
    for (uint64_t k = off / MP_BLOCK_V1; !rc && n; k++) {
        uint64_t c = z->chunks[f->first + k], base = k * MP_BLOCK_V1;
        size_t want = (size_t)(f->size - base < MP_BLOCK_V1 ? f->size - base : MP_BLOCK_V1);
        uint32_t stored = C_STORED(c);
        if (z->vs.read(z->vs.ctx, C_VOL(c), C_OFF(c), src, stored, e) != 0) {
            rc = -1;
            break;
        }
        const uint8_t *plain = src;
        if (C_CODEC(c) == 1) {
            ZSTD_frameHeader fh;
            size_t r = ZSTD_getFrameHeader(&fh, src, stored);
            if (r != 0 || fh.frameContentSize != want || !fh.checksumFlag || fh.windowSize > MP_BLOCK_V1 ||
                fh.dictID != 0) {
                mp_err_set(e, "%s: bloque %llu con un frame zstd no válido para el runtime", f->path,
                           (unsigned long long)k);
                rc = -1;
                break;
            }
            r = ZSTD_decompressDCtx(dc, dec, want, src, stored);
            if (ZSTD_isError(r) || r != want) {
                mp_err_set(e, "%s: bloque %llu dañado (%s)", f->path, (unsigned long long)k,
                           ZSTD_isError(r) ? ZSTD_getErrorName(r) : "tamaño distinto");
                rc = -1;
                break;
            }
            plain = dec;
        }
        size_t from = (size_t)(off > base ? off - base : 0), take = want - from < n ? want - from : n;
        memcpy(o, plain + from, take);
        o += take;
        n -= take;
        off += take;
    }
    ZSTD_freeDCtx(dc);
    free(src);
    free(dec);
    return rc;
}

// ---- Volúmenes de una carpeta, abiertos bajo demanda ----------------------------------------

typedef struct {
    char *dir;
    mp_file *f[MP_ZPK_MAX_VOLUMES];
    mp_mutex *mu;
} dirsrc;

static mp_file *ds_get(dirsrc *d, uint32_t vol) {
    if (vol >= MP_ZPK_MAX_VOLUMES) return NULL;
    mp_mutex_lock(d->mu);
    if (!d->f[vol]) {
        char p[2048];
        snprintf(p, sizeof p, "%s/ampr_zstd-%03u.pak", d->dir, vol);
        mp_file_open_read(p, &d->f[vol], NULL);
    }
    mp_file *f = d->f[vol];
    mp_mutex_unlock(d->mu);
    return f;
}

static int dsrc_read(void *ctx, uint32_t vol, uint64_t off, void *buf, size_t n, mp_err *e) {
    mp_file *f = ds_get(ctx, vol);
    size_t got = 0;
    if (!f) {
        mp_err_set(e, "falta el volumen ampr_zstd-%03u.pak", vol);
        return -1;
    }
    if (mp_file_pread(f, buf, n, off, &got, e) != 0) return -1;
    if (got != n) {
        mp_err_set(e, "volumen ampr_zstd-%03u.pak truncado", vol);
        return -1;
    }
    return 0;
}

static uint64_t dsrc_size(void *ctx, uint32_t vol) {
    mp_file *f = ds_get(ctx, vol);
    uint64_t s = 0;
    if (!f || mp_file_size(f, &s, NULL) != 0) return 0;
    return s;
}

int mp_zpk_volsrc_dir(const char *dir, mp_zpk_volsrc *out, mp_err *e) {
    memset(out, 0, sizeof *out);
    dirsrc *d = calloc(1, sizeof *d);
    if (!d || !(d->dir = mp_strdup(dir)) || !(d->mu = mp_mutex_new())) {
        if (d) free(d->dir);
        free(d);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    out->read = dsrc_read;
    out->size = dsrc_size;
    out->ctx = d;
    return 0;
}

void mp_zpk_volsrc_free(mp_zpk_volsrc *vs) {
    dirsrc *d = vs ? vs->ctx : NULL;
    if (!d) return;
    for (uint32_t v = 0; v < MP_ZPK_MAX_VOLUMES; v++) mp_file_close(d->f[v]);
    mp_mutex_free(d->mu);
    free(d->dir);
    free(d);
    vs->ctx = NULL;
}
