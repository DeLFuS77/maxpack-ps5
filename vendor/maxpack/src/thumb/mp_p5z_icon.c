// SPDX-License-Identifier: GPL-3.0-or-later
// A .p5z's icon for the file manager thumbnails (format in docs/p5z-format.md). It checks the header,
// the footer and the index as mp_p5z_read.c does, then the icon's record, and only then reads it.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <zstd.h>

#include "mp_p5z_icon.h"

#define HEADER 64u
#define FOOTER 32u
#define INDEX_HEADER 56u
#define INDEX_HEADER_V2 72u   // index version 2: a .p5z that grew
#define FILE_REC 88u
#define FRAME_REC 16u
#define MANIFEST_REC 64u
#define FRAME_MAX (32u << 20)   // the .p5z's frames are of up to 32 MiB
// The packer gives an icon of up to MP_ICON_MAX one frame of FRAME_MAX: more than this is not an icon it
// wrote, and each frame reads at most what zstd takes for its size, so an icon reads about its own size.
#define ICON_FRAMES_MAX 64u
#define ROLE_LOOSE 0
#define STORE_RAW 0
#define STORE_FRAMES 1
#define ICON_PATH "sce_sys/icon0.png"

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static uint32_t crc(const uint8_t *p, size_t n) {
    uLong c = crc32(0L, Z_NULL, 0);
    while (n) {
        uInt k = n > 0x40000000u ? 0x40000000u : (uInt)n;
        c = crc32(c, p, k);
        p += k;
        n -= k;
    }
    return (uint32_t)c;
}

static int fail(char *err, size_t errlen, const char *fmt, ...) {
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static int read_at(const mp_icon_src *s, void *buf, size_t n, uint64_t off, char *err, size_t errlen) {
    if (off > s->size || n > s->size - off || s->read_at(s->ctx, buf, n, off) != 0)
        return fail(err, errlen, "cannot read the .p5z at %llu", (unsigned long long)off);
    return 0;
}

static int ieq(const uint8_t *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

// The loose file sce_sys/icon0.png in the index: by its exact path, else in any case. NULL if there is
// none; *bad if a path is outside the strings.
static const uint8_t *find_icon(const uint8_t *ix, uint64_t hs, uint32_t nf, const uint8_t *strings, uint64_t slen,
                                int *bad) {
    const size_t want = sizeof ICON_PATH - 1;
    const uint8_t *any_case = NULL;
    *bad = 0;
    for (uint32_t i = 0; i < nf; i++) {
        const uint8_t *r = ix + hs + (size_t)FILE_REC * i;
        uint32_t off = rd32(r), len = rd32(r + 4);
        if ((uint64_t)off + len > slen) {
            *bad = 1;
            return NULL;
        }
        if (r[8] != ROLE_LOOSE || len != want) continue;
        if (!memcmp(strings + off, ICON_PATH, want)) return r;
        if (!any_case && ieq(strings + off, ICON_PATH, want)) any_case = r;
    }
    return any_case;
}

int mp_p5z_icon_read(const mp_icon_src *src, uint8_t **png, size_t *len, char *err, size_t errlen) {
    *png = NULL;
    *len = 0;
    if (err && errlen) err[0] = 0;
    uint8_t h[HEADER], ft[FOOTER], *cz = NULL, *ix = NULL, *in = NULL, *out = NULL;
    ZSTD_DCtx *dc = NULL;
    int rc = -1;
    if (src->size < HEADER + FOOTER) return fail(err, errlen, "not a .p5z (too small)");
    if (read_at(src, h, sizeof h, 0, err, errlen) != 0) return -1;
    if (memcmp(h, "MAXPKP5Z", 8) != 0 || rd32(h + 8) != 1 || rd32(h + 12) != HEADER)
        return fail(err, errlen, "not a .p5z or from a newer version");
    uint8_t hz[HEADER];
    memcpy(hz, h, sizeof hz);
    memset(hz + 60, 0, 4);
    if (crc(hz, sizeof hz) != rd32(h + 60)) return fail(err, errlen, "invalid .p5z: header CRC");
    if (read_at(src, ft, sizeof ft, src->size - FOOTER, err, errlen) != 0) return -1;
    if (memcmp(ft, "P5ZFOOT1", 8) != 0)
        return fail(err, errlen, "truncated or incomplete .p5z (the footer is missing)");
    if (crc(ft, 28) != rd32(ft + 28)) return fail(err, errlen, "invalid .p5z: footer CRC");
    uint64_t ioff = rd64(ft + 8), ilen = rd64(ft + 16);
    if (ioff < HEADER || ilen == 0 || ilen > src->size - FOOTER || ioff != src->size - FOOTER - ilen)
        return fail(err, errlen, "truncated .p5z or with the index out of place");
    if (ilen > MP_ICON_INDEX_MAX)
        return fail(err, errlen, "index too large (%llu bytes compressed)", (unsigned long long)ilen);

    if (!(cz = malloc((size_t)ilen))) {
        fail(err, errlen, "out of memory");
        goto done;
    }
    if (read_at(src, cz, (size_t)ilen, ioff, err, errlen) != 0) goto done;
    if (crc(cz, (size_t)ilen) != rd32(ft + 24)) {
        fail(err, errlen, "invalid .p5z: index CRC");
        goto done;
    }
    unsigned long long us = ZSTD_getFrameContentSize(cz, (size_t)ilen);
    if (us == ZSTD_CONTENTSIZE_ERROR || us == ZSTD_CONTENTSIZE_UNKNOWN || us < INDEX_HEADER) {
        fail(err, errlen, "invalid .p5z: compressed index");
        goto done;
    }
    if (us > MP_ICON_UINDEX_MAX) {
        fail(err, errlen, "index too large (%llu bytes)", us);
        goto done;
    }
    if (!(ix = malloc((size_t)us)) || !(dc = ZSTD_createDCtx())) {
        fail(err, errlen, "out of memory");
        goto done;
    }
    size_t got = ZSTD_decompressDCtx(dc, ix, (size_t)us, cz, (size_t)ilen);
    free(cz);
    cz = NULL;
    if (ZSTD_isError(got) || got != us) {
        fail(err, errlen, "invalid .p5z: compressed index");
        goto done;
    }

    size_t n = (size_t)us;
    // version 1, or 2 for a .p5z that grew by appending: its header is 72 bytes, the records the same
    uint32_t version = rd32(ix + 8);
    uint64_t hs = version == 2 ? INDEX_HEADER_V2 : INDEX_HEADER;
    if (memcmp(ix, "P5ZIDX01", 8) != 0 || (version != 1 && version != 2) || n < hs) {
        fail(err, errlen, "invalid .p5z: index");
        goto done;
    }
    uint64_t nf = rd32(ix + 12), nfr = rd32(ix + 16), nm = rd32(ix + 20), slen = rd64(ix + 24);
    uint64_t fixed = hs + FILE_REC * nf + FRAME_REC * nfr + MANIFEST_REC * nm;   // u32 counters: no wrap
    if (fixed > n || slen != n - fixed) {
        fail(err, errlen, "invalid .p5z: index size");
        goto done;
    }
    int bad = 0;
    const uint8_t *r = find_icon(ix, hs, (uint32_t)nf, ix + fixed, slen, &bad);
    if (bad) {
        fail(err, errlen, "invalid .p5z: string outside the index");
        goto done;
    }
    if (!r) {
        if (err && errlen) snprintf(err, errlen, "the .p5z has no %s", ICON_PATH);
        rc = 1;
        goto done;
    }

    // The icon's record, checked before reading or allocating it.
    int storage = r[9];
    uint64_t size = rd64(r + 12), data_off = rd64(r + 28), data_len = rd64(r + 36);
    uint32_t first = rd32(r + 44), nframes = rd32(r + 48);
    if (size > MP_ICON_MAX) {
        fail(err, errlen, "icon too large (%llu bytes)", (unsigned long long)size);
        goto done;
    }
    if (data_off < HEADER || data_off > ioff || data_len > ioff - data_off) {
        fail(err, errlen, "invalid .p5z: the icon's region");
        goto done;
    }
    if (storage == STORE_RAW && (data_len != size || nframes)) {
        fail(err, errlen, "invalid .p5z: the icon's region");
        goto done;
    }
    const uint8_t *frames = ix + hs + FILE_REC * nf;
    size_t in_cap = 0;
    if (storage == STORE_FRAMES) {
        if ((uint64_t)first + nframes > nfr || nframes > ICON_FRAMES_MAX) {
            fail(err, errlen, "invalid .p5z: the icon's frames");
            goto done;
        }
        uint64_t pos = data_off, total = 0;
        for (uint32_t k = 0; k < nframes; k++) {
            const uint8_t *fr = frames + (size_t)FRAME_REC * (first + k);
            uint32_t csize = rd32(fr + 8), usize = rd32(fr + 12);
            if (rd64(fr) != pos || usize == 0 || usize > FRAME_MAX || csize > ZSTD_COMPRESSBOUND(usize)) {
                fail(err, errlen, "invalid .p5z: the icon's frames");
                goto done;
            }
            pos += csize;
            total += usize;
            if (csize > in_cap) in_cap = csize;
        }
        if (pos != data_off + data_len || total != size) {
            fail(err, errlen, "invalid .p5z: the icon's frames");
            goto done;
        }
    } else if (storage != STORE_RAW) {
        fail(err, errlen, "invalid .p5z: the icon's record");
        goto done;
    }

    if (!(out = malloc(size ? (size_t)size : 1)) || (in_cap && !(in = malloc(in_cap)))) {
        fail(err, errlen, "out of memory");
        goto done;
    }
    if (storage == STORE_RAW) {
        if (size && read_at(src, out, (size_t)size, data_off, err, errlen) != 0) goto done;
    } else {
        size_t at = 0;
        for (uint32_t k = 0; k < nframes; k++) {
            const uint8_t *fr = frames + (size_t)FRAME_REC * (first + k);
            uint32_t csize = rd32(fr + 8), usize = rd32(fr + 12);
            if (read_at(src, in, csize, rd64(fr), err, errlen) != 0) goto done;
            size_t d = ZSTD_decompressDCtx(dc, out + at, usize, in, csize);
            if (ZSTD_isError(d) || d != usize) {
                fail(err, errlen, "damaged icon in the .p5z (%s)", ZSTD_isError(d) ? ZSTD_getErrorName(d) : "different size");
                goto done;
            }
            at += usize;
        }
    }
    *png = out;
    *len = (size_t)size;
    out = NULL;
    rc = 0;
done:
    ZSTD_freeDCtx(dc);
    free(cz);
    free(ix);
    free(in);
    free(out);
    return rc;
}
