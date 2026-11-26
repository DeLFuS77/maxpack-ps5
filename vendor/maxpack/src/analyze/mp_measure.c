// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_measure.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "mp_platform.h"

struct mp_measurer {
    ZSTD_CCtx *cctx;      // no dictionary; reset on every block
    ZSTD_CCtx *dctx[2];   // dctx[p] has dict[p] loaded (NULL if none)
    z_stream zs;
    int zs_ok;
    uint8_t *out;
    size_t out_cap;
};

static const char *const names[MP_CFG_COUNT] = {"v1-l5",  "v1-l9",    "v1-l19",  "v1-l22",
                                                "1m-l19", "dict-l19", "zlib-64k"};

const char *mp_cfg_name(mp_cfg c) { return c < MP_CFG_COUNT ? names[c] : "?"; }

int mp_cfg_is_v1(mp_cfg c) { return c <= MP_CFG_V1_L22; }

size_t mp_zstd_block(ZSTD_CCtx *c, int level, int window_log, const void *src, size_t n, void *dst,
                     size_t cap) {
    ZSTD_CCtx_reset(c, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, window_log);
    ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 1);
    ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, 1);
    size_t r = ZSTD_compress2(c, dst, cap, src, n);
    return ZSTD_isError(r) ? 0 : r;
}

void mp_zstd_set_long(ZSTD_CCtx *c) {
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, 19);
    ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, 25);
    ZSTD_CCtx_setParameter(c, ZSTD_c_enableLongDistanceMatching, 1);
    ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 1);
}

mp_measurer *mp_measurer_new(const mp_dict_pair *dicts) {
    mp_measurer *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->cctx = ZSTD_createCCtx();
    for (int p = 0; p < 2; p++) {
        if (!dicts || !dicts->dict[p]) continue;
        ZSTD_CCtx *c = ZSTD_createCCtx();
        if (!c) continue;
        ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, 19);
        ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, 18);
        ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 1);
        ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, 1);
        if (ZSTD_isError(ZSTD_CCtx_loadDictionary(c, dicts->dict[p], dicts->size[p]))) {
            ZSTD_freeCCtx(c);
            continue;
        }
        m->dctx[p] = c;
    }
    m->zs_ok = deflateInit2(&m->zs, 9, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) == Z_OK;
    size_t a = ZSTD_compressBound(MP_WINDOW_MAX), b = compressBound(MP_BLOCK_ZLIB);
    m->out_cap = a > b ? a : b;
    m->out = malloc(m->out_cap);
    if (!m->cctx || !m->zs_ok || !m->out) {
        mp_measurer_free(m);
        return NULL;
    }
    return m;
}

void mp_measurer_free(mp_measurer *m) {
    if (!m) return;
    ZSTD_freeCCtx(m->cctx);
    ZSTD_freeCCtx(m->dctx[0]);
    ZSTD_freeCCtx(m->dctx[1]);
    if (m->zs_ok) deflateEnd(&m->zs);
    free(m->out);
    free(m);
}

static size_t keep(size_t frame, size_t raw) { return frame && frame < raw ? frame : raw; }

static void zstd_cfg(mp_measurer *m, const uint8_t *d, size_t len, size_t block, int level,
                     int wlog, mp_cfg_stat *st) {
    uint64_t t0 = mp_now_ns();
    for (size_t off = 0; off < len; off += block) {
        size_t n = len - off < block ? len - off : block;
        st->stored += keep(mp_zstd_block(m->cctx, level, wlog, d + off, n, m->out, m->out_cap), n);
        st->raw += n;
    }
    st->ns += mp_now_ns() - t0;
}

static void dict_cfg(mp_measurer *m, ZSTD_CCtx *c, const uint8_t *d, size_t len, mp_cfg_stat *st) {
    uint64_t t0 = mp_now_ns();
    for (size_t off = 0; off < len; off += MP_BLOCK_V1) {
        size_t n = len - off < MP_BLOCK_V1 ? len - off : MP_BLOCK_V1;
        size_t r = ZSTD_compress2(c, m->out, m->out_cap, d + off, n);
        st->stored += keep(ZSTD_isError(r) ? 0 : r, n);
        st->raw += n;
    }
    st->ns += mp_now_ns() - t0;
}

static void zlib_cfg(mp_measurer *m, const uint8_t *d, size_t len, mp_cfg_stat *st) {
    uint64_t t0 = mp_now_ns();
    for (size_t off = 0; off < len; off += MP_BLOCK_ZLIB) {
        size_t n = len - off < MP_BLOCK_ZLIB ? len - off : MP_BLOCK_ZLIB, out = 0;
        deflateReset(&m->zs);
        m->zs.next_in = (Bytef *)(d + off);
        m->zs.avail_in = (uInt)n;
        m->zs.next_out = m->out;
        m->zs.avail_out = (uInt)m->out_cap;
        if (deflate(&m->zs, Z_FINISH) == Z_STREAM_END) out = m->out_cap - m->zs.avail_out;
        st->stored += keep(out, n);
        st->raw += n;
    }
    st->ns += mp_now_ns() - t0;
}

void mp_measure_window(mp_measurer *m, const uint8_t *data, size_t len, unsigned parity,
                       mp_cfg_stat st[MP_CFG_COUNT]) {
    static const int levels[4] = {5, 9, 19, 22};
    mp_cfg_stat l19 = {0};
    for (int i = 0; i < 4; i++) {
        mp_cfg_stat w = {0};
        zstd_cfg(m, data, len, MP_BLOCK_V1, levels[i], 18, &w);
        st[i].raw += w.raw;
        st[i].stored += w.stored;
        st[i].ns += w.ns;
        if (i == MP_CFG_V1_L19) l19 = w;
    }
    zstd_cfg(m, data, len, MP_WINDOW_MAX, 19, 20, &st[MP_CFG_1M_L19]);
    ZSTD_CCtx *dc = m->dctx[1 - (parity & 1)];
    if (dc) {
        dict_cfg(m, dc, data, len, &st[MP_CFG_DICT_L19]);
    } else {
        // Without a dictionary for this parity, the result is exactly v1-l19's.
        st[MP_CFG_DICT_L19].raw += l19.raw;
        st[MP_CFG_DICT_L19].stored += l19.stored;
        st[MP_CFG_DICT_L19].ns += l19.ns;
    }
    zlib_cfg(m, data, len, &st[MP_CFG_ZLIB64K]);
}
