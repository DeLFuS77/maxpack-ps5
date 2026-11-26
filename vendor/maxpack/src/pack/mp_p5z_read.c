// SPDX-License-Identifier: GPL-3.0-or-later
// .p5z reader: header, footer, index and its consistency; access to the files and the packs.
#define ZSTD_STATIC_LINKING_ONLY
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "mp_hash.h"
#include "mp_idx3.h"
#include "mp_p5z_internal.h"

#define INDEX_MAX (1ull << 31)   // uncompressed index of at most 2 GiB
#define RAW_PIECE (4u << 20)

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int bad(mp_err *e, const char *why) {
    mp_err_set(e, "invalid .p5z: %s", why);
    return -1;
}

static int pread_exact(mp_file *f, void *buf, size_t n, uint64_t off, mp_err *e) {
    size_t got = 0;
    if (mp_file_pread(f, buf, n, off, &got, e) != 0) return -1;
    if (got != n) {
        mp_err_set(e, "truncated .p5z (data missing at %llu)", (unsigned long long)off);
        return -1;
    }
    return 0;
}

void mp_p5z_close(mp_p5z *p) {
    if (!p) return;
    mp_file_close(p->f);
    for (size_t i = 0; i < p->nfiles; i++) free(p->files[i].path);
    free(p->files);
    free(p->frames);
    mp_pack_manifest_free(&p->man);
    free(p->man_file);
    free(p);
}

static char *str_at(const uint8_t *strings, uint64_t slen, const uint8_t *rec, mp_err *e) {
    uint32_t off = rd32(rec), len = rd32(rec + 4);
    if ((uint64_t)off + len > slen) {
        bad(e, "string outside the index");
        return NULL;
    }
    char *s = malloc(len + 1u);
    if (!s) {
        mp_err_set(e, "out of memory");
        return NULL;
    }
    memcpy(s, strings + off, len);
    s[len] = 0;
    return s;
}

static void copy_meta(char *dst, size_t cap, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

// Index version 2 only: the volumes are ampr_zstd-000.pak... with the ids 0..n-1, each once, and there
// is at most one ampr_zstd.index and one ampr_emu.index (maxpack.image lists the volumes by id).
static int check_v2_records(const mp_p5z *p, mp_err *e) {
    size_t nvol = 0, nzpk = 0, nidx3 = 0;
    for (size_t i = 0; i < p->nfiles; i++) {
        nvol += p->files[i].role == MP_ROLE_VOLUME;
        nzpk += p->files[i].role == MP_ROLE_ZPK_INDEX;
        nidx3 += p->files[i].role == MP_ROLE_IDX3;
    }
    if (nzpk > 1 || nidx3 > 1) return bad(e, "an index twice");
    if (nvol > MP_ZPK_MAX_VOLUMES) return bad(e, "too many volumes");
    uint8_t *seen = calloc(nvol ? nvol : 1, 1);
    if (!seen) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    int rc = 0;
    for (size_t i = 0; i < p->nfiles && !rc; i++) {
        if (p->files[i].role != MP_ROLE_VOLUME) continue;
        unsigned id = 0;
        char name[32];
        if (sscanf(p->files[i].path, "ampr_zstd-%u.pak", &id) != 1 || id >= nvol) {
            rc = bad(e, "a volume that is not one of 0..n-1");
            break;
        }
        snprintf(name, sizeof name, "ampr_zstd-%03u.pak", id);
        if (strcmp(name, p->files[i].path) || seen[id]) rc = bad(e, "a volume that is not one of 0..n-1");
        seen[id] = 1;
    }
    free(seen);
    return rc;
}

static int parse_index(mp_p5z *p, const uint8_t *ix, size_t n, int flags, mp_err *e) {
    if (n < P5Z_INDEX_HEADER || memcmp(ix, "P5ZIDX01", 8) != 0) return bad(e, "index");
    p->version = rd32(ix + 8);
    if (p->version == 2 && (flags & MP_P5Z_OPEN_V1))
        return bad(e, "it grew (index version 2), and this reader takes only a .p5z as it was packed");
    if (p->version != 1 && (p->version != 2 || n < P5Z_INDEX_HEADER_V2)) return bad(e, "index");
    const uint64_t hs = p->version == 2 ? P5Z_INDEX_HEADER_V2 : P5Z_INDEX_HEADER;
    if (p->version == 2) {
        p->prev_size = rd64(ix + 56);
        p->grown = rd32(ix + 64);
        if (p->prev_size < P5Z_HEADER + P5Z_FOOTER || p->prev_size > p->index_off || p->grown == 0 ||
            rd32(ix + 68) != 0)
            return bad(e, "index version 2");
    }
    uint64_t nf = rd32(ix + 12), nfr = rd32(ix + 16), nm = rd32(ix + 20), slen = rd64(ix + 24);
    // The counters are u32, so the fixed part cannot wrap around; the strings can.
    uint64_t fixed = hs + P5Z_FILE_REC * nf + P5Z_FRAME_REC * nfr + P5Z_MANIFEST_REC * nm;
    if (fixed > n || slen != n - fixed) return bad(e, "index size");
    const uint8_t *strings = ix + n - slen;
    p->nfiles = (size_t)nf;
    p->nframes = (size_t)nfr;
    p->files = calloc(nf ? (size_t)nf : 1, sizeof *p->files);
    p->frames = calloc(nfr ? (size_t)nfr : 1, sizeof *p->frames);
    p->man.v = calloc(nm ? (size_t)nm : 1, sizeof *p->man.v);
    p->man_file = calloc(nm ? (size_t)nm : 1, sizeof *p->man_file);
    if (!p->files || !p->frames || !p->man.v || !p->man_file) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    char *meta[3];
    for (int k = 0; k < 3; k++) {
        meta[k] = str_at(strings, slen, ix + 32 + 8 * k, e);
        if (!meta[k]) {
            for (int j = 0; j < k; j++) free(meta[j]);
            return -1;
        }
    }
    copy_meta(p->man.title_id, sizeof p->man.title_id, meta[0]);
    copy_meta(p->man.content_version, sizeof p->man.content_version, meta[1]);
    copy_meta(p->man.title_name, sizeof p->man.title_name, meta[2]);
    for (int k = 0; k < 3; k++) free(meta[k]);
    const uint8_t *r = ix + hs;
    for (size_t i = 0; i < p->nfiles; i++, r += P5Z_FILE_REC) {
        p5z_file *f = &p->files[i];
        if (!(f->path = str_at(strings, slen, r, e))) return -1;
        f->role = r[8];
        f->storage = r[9];
        f->size = rd64(r + 12);
        f->mtime = (int64_t)rd64(r + 20);
        f->data_off = rd64(r + 28);
        f->data_len = rd64(r + 36);
        f->first_frame = rd32(r + 44);
        f->nframes = rd32(r + 48);
        memcpy(f->sha, r + 52, 32);
        if (f->role > MP_ROLE_ORIGINAL || f->storage > P5Z_STORE_FRAMES || f->data_off < P5Z_HEADER ||
            f->data_off + f->data_len > p->index_off || f->data_off + f->data_len < f->data_off ||
            mp_pack_path_check(f->path, e) != 0)
            return bad(e, "file record");
        if (f->storage == P5Z_STORE_RAW && (f->data_len != f->size || f->nframes))
            return bad(e, "raw file with a different size");
        if (f->storage == P5Z_STORE_FRAMES && (uint64_t)f->first_frame + f->nframes > nfr)
            return bad(e, "frames outside the index");
    }
    for (size_t i = 0; i < p->nframes; i++, r += P5Z_FRAME_REC) {
        p->frames[i].off = rd64(r);
        p->frames[i].csize = rd32(r + 8);
        p->frames[i].usize = rd32(r + 12);
    }
    // Each file's frames follow one another within its region and add up to its size.
    for (size_t i = 0; i < p->nfiles; i++) {
        const p5z_file *f = &p->files[i];
        if (f->storage != P5Z_STORE_FRAMES) continue;
        uint64_t pos = f->data_off, total = 0;
        for (uint32_t k = 0; k < f->nframes; k++) {
            const p5z_frame *fr = &p->frames[f->first_frame + k];
            if (fr->off != pos || fr->usize == 0 || fr->usize > MP_LONG_FRAME) return bad(e, "a file's frames");
            pos += fr->csize;
            total += fr->usize;
        }
        if (pos != f->data_off + f->data_len || total != f->size) return bad(e, "a file's frames");
    }
    if (p->version == 2 && check_v2_records(p, e) != 0) return -1;
    p->man.n = (size_t)nm;
    for (size_t i = 0; i < p->man.n; i++, r += P5Z_MANIFEST_REC) {
        mp_manifest_entry *m = &p->man.v[i];
        if (!(m->path = str_at(strings, slen, r, e))) return -1;
        m->flags = rd32(r + 8);
        p->man_file[i] = rd32(r + 12);
        m->size = rd64(r + 16);
        m->mtime = (int64_t)rd64(r + 24);
        memcpy(m->sha, r + 32, 32);
        if (m->flags > 2 || mp_pack_path_check(m->path, e) != 0) return bad(e, "manifest record");
        if (m->flags & MP_MANIFEST_PACKED) {
            if (p->man_file[i] != P5Z_NO_FILE) return bad(e, "manifest: packed entry with a file");
        } else {
            uint32_t fi = p->man_file[i];
            int want = (m->flags & MP_MANIFEST_REPLACED) ? MP_ROLE_ORIGINAL : MP_ROLE_LOOSE;
            if (fi >= p->nfiles || p->files[fi].role != want || p->files[fi].size != m->size ||
                strcmp(p->files[fi].path, m->path) || memcmp(p->files[fi].sha, m->sha, 32))
                return bad(e, "manifest that does not match its files");
        }
    }
    return 0;
}

int mp_p5z_open(const char *path, mp_p5z **out, mp_err *e) { return mp_p5z_open_ex(path, 0, out, e); }

uint32_t mp_p5z_version(const mp_p5z *p) { return p->version; }
uint32_t mp_p5z_grown(const mp_p5z *p) { return p->grown; }

int mp_p5z_open_ex(const char *path, int flags, mp_p5z **out, mp_err *e) {
    *out = NULL;
    mp_p5z *p = calloc(1, sizeof *p);
    if (!p) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    uint8_t h[P5Z_HEADER], ft[P5Z_FOOTER], *cz = NULL, *ix = NULL;
    int rc = -1;
    if (mp_file_open_read(path, &p->f, e) != 0 || mp_file_size(p->f, &p->size, e) != 0) goto done;
    // an append that has not ended (at work, or stopped): the .p5z is read as it was before it
    uint64_t before = 0;
    if (p5z_journal_matches(path, p->size, &before)) p->size = before;
    if (p->size < P5Z_HEADER + P5Z_FOOTER) {
        mp_err_set(e, "truncated or empty .p5z: %s", path);
        goto done;
    }
    if (pread_exact(p->f, h, sizeof h, 0, e) != 0 || pread_exact(p->f, ft, sizeof ft, p->size - P5Z_FOOTER, e) != 0)
        goto done;
    if (memcmp(h, "MAXPKP5Z", 8) != 0 || rd32(h + 8) != 1 || rd32(h + 12) != P5Z_HEADER) {
        bad(e, "not a .p5z or from a newer version");
        goto done;
    }
    uint8_t hz[P5Z_HEADER];
    memcpy(hz, h, sizeof hz);
    memset(hz + 60, 0, 4);
    if (mp_crc32(hz, sizeof hz, 0) != rd32(h + 60)) {
        bad(e, "header CRC");
        goto done;
    }
    if (memcmp(ft, "P5ZFOOT1", 8) != 0) {
        mp_err_set(e, "truncated or incomplete .p5z (the footer is missing): %s", path);
        goto done;
    }
    if (mp_crc32(ft, 28, 0) != rd32(ft + 28)) {
        bad(e, "footer CRC");
        goto done;
    }
    uint64_t ioff = rd64(ft + 8), ilen = rd64(ft + 16);
    if (ioff < P5Z_HEADER || ilen == 0 || ilen > INDEX_MAX || ioff + ilen + P5Z_FOOTER != p->size) {
        mp_err_set(e, "truncated .p5z or with the index out of place: %s", path);
        goto done;
    }
    p->index_off = ioff;
    p->index_len = ilen;
    cz = malloc((size_t)ilen);
    if (!cz) {
        mp_err_set(e, "out of memory");
        goto done;
    }
    if (pread_exact(p->f, cz, (size_t)ilen, ioff, e) != 0) goto done;
    if (mp_crc32(cz, (size_t)ilen, 0) != rd32(ft + 24)) {
        bad(e, "index CRC");
        goto done;
    }
    unsigned long long us = ZSTD_getFrameContentSize(cz, (size_t)ilen);
    if (us == ZSTD_CONTENTSIZE_ERROR || us == ZSTD_CONTENTSIZE_UNKNOWN || us > INDEX_MAX || us < P5Z_INDEX_HEADER) {
        bad(e, "compressed index");
        goto done;
    }
    ix = malloc((size_t)us);
    if (!ix || ZSTD_decompress(ix, (size_t)us, cz, (size_t)ilen) != us) {
        bad(e, "compressed index");
        goto done;
    }
    if (parse_index(p, ix, (size_t)us, flags, e) != 0) goto done;
    p->man.level = h[20];
    memcpy(p->man.archive_id, h + 24, 16);
    rc = 0;
done:
    free(cz);
    free(ix);
    if (rc) {
        mp_p5z_close(p);
        return -1;
    }
    *out = p;
    return 0;
}

const mp_pack_manifest *mp_p5z_manifest(const mp_p5z *p) { return &p->man; }

int p5z_read_file(mp_p5z *p, size_t i, p5z_sink sink, void *ctx, mp_err *e) {
    const p5z_file *f = &p->files[i];
    mp_sha256 s;
    mp_sha256_init(&s);
    size_t cap = f->storage == P5Z_STORE_FRAMES ? MP_LONG_FRAME : RAW_PIECE;
    uint8_t *in = malloc(f->storage == P5Z_STORE_FRAMES ? ZSTD_compressBound(MP_LONG_FRAME) : RAW_PIECE);
    uint8_t *dec = f->storage == P5Z_STORE_FRAMES ? malloc(cap) : NULL;
    ZSTD_DCtx *dc = f->storage == P5Z_STORE_FRAMES ? ZSTD_createDCtx() : NULL;
    int rc = 0;
    if (!in || (f->storage == P5Z_STORE_FRAMES && (!dec || !dc))) {
        mp_err_set(e, "out of memory");
        rc = -1;
    }
    if (!rc && f->storage == P5Z_STORE_RAW) {
        for (uint64_t off = 0; !rc && off < f->size; off += RAW_PIECE) {
            size_t k = f->size - off < RAW_PIECE ? (size_t)(f->size - off) : RAW_PIECE;
            rc = pread_exact(p->f, in, k, f->data_off + off, e);
            if (!rc && f->role == MP_ROLE_VOLUME) {   // a volume's SHA-256 starts at 64
                if (off + k > MP_ZPK_VOL_HEADER)
                    mp_sha256_update(&s, in + (off < MP_ZPK_VOL_HEADER ? MP_ZPK_VOL_HEADER - off : 0),
                                     (size_t)(off + k - (off < MP_ZPK_VOL_HEADER ? MP_ZPK_VOL_HEADER : off)));
            } else if (!rc) {
                mp_sha256_update(&s, in, k);
            }
            if (!rc) rc = sink(ctx, in, k, e);
        }
    } else if (!rc) {
        for (uint32_t k = 0; !rc && k < f->nframes; k++) {
            const p5z_frame *fr = &p->frames[f->first_frame + k];
            if (fr->csize > ZSTD_compressBound(MP_LONG_FRAME)) {
                rc = bad(e, "frame too large");
                break;
            }
            rc = pread_exact(p->f, in, fr->csize, fr->off, e);
            if (rc) break;
            size_t r = ZSTD_decompressDCtx(dc, dec, fr->usize, in, fr->csize);
            if (ZSTD_isError(r) || r != fr->usize) {
                mp_err_set(e, "%s: damaged data in the .p5z (%s)", f->path,
                           ZSTD_isError(r) ? ZSTD_getErrorName(r) : "different size");
                rc = -1;
                break;
            }
            mp_sha256_update(&s, dec, r);
            rc = sink(ctx, dec, r, e);
        }
    }
    if (!rc) {
        uint8_t d[32];
        mp_sha256_final(&s, d);
        if (memcmp(d, f->sha, 32) != 0) {
            mp_err_set(e, "%s: the SHA-256 does not match (file damaged in the .p5z)", f->path);
            rc = -1;
        }
    }
    ZSTD_freeDCtx(dc);
    free(in);
    free(dec);
    return rc;
}

// ---- packs from the .p5z's regions ----------------------------------------------------------

static const p5z_file *volume_file(const mp_p5z *p, uint32_t vol) {
    char name[32];
    snprintf(name, sizeof name, "ampr_zstd-%03u.pak", vol);
    for (size_t i = 0; i < p->nfiles; i++)
        if (p->files[i].role == MP_ROLE_VOLUME && !strcmp(p->files[i].path, name)) return &p->files[i];
    return NULL;
}

static int vread(void *ctx, uint32_t vol, uint64_t off, void *buf, size_t n, mp_err *e) {
    mp_p5z *p = ctx;
    const p5z_file *f = volume_file(p, vol);
    if (!f || off + n > f->size) {
        mp_err_set(e, "volume %u is missing from the .p5z", vol);
        return -1;
    }
    return pread_exact(p->f, buf, n, f->data_off + off, e);
}

static uint64_t vsize(void *ctx, uint32_t vol) {
    const p5z_file *f = volume_file(ctx, vol);
    return f ? f->size : 0;
}

typedef struct {
    uint8_t *buf;
    size_t n, cap;
} membuf;

static int to_mem(void *ctx, const void *p, size_t n, mp_err *e) {
    membuf *m = ctx;
    if (m->n + n > m->cap) {
        size_t cap = (m->n + n) * 2;
        uint8_t *b = realloc(m->buf, cap);
        if (!b) {
            mp_err_set(e, "out of memory");
            return -1;
        }
        m->buf = b;
        m->cap = cap;
    }
    memcpy(m->buf + m->n, p, n);
    m->n += n;
    return 0;
}

int mp_p5z_zpk(mp_p5z *p, mp_zpk **out, mp_err *e) {
    *out = NULL;
    size_t i = 0;
    while (i < p->nfiles && p->files[i].role != MP_ROLE_ZPK_INDEX) i++;
    if (i == p->nfiles) {
        mp_err_set(e, "the .p5z carries no packs");
        return -1;
    }
    membuf m = {0};
    if (p5z_read_file(p, i, to_mem, &m, e) != 0) {
        free(m.buf);
        return -1;
    }
    mp_zpk_volsrc vs = {vread, vsize, p};
    // a .p5z that grew has its packs as ampr-emu-ng's p5z mode takes them
    int rc = mp_zpk_open_ex(m.buf, m.n, &vs, p->version == 2 ? MP_ZPK_RULES_P5Z : MP_ZPK_RULES_PACK, out, e);
    free(m.buf);
    return rc;
}

static void size_text(uint64_t b, char out[32]) {
    double v = (double)b;
    const char *u = "B";
    if (v >= 1e9) {
        v /= 1e9;
        u = "GB";
    } else if (v >= 1e6) {
        v /= 1e6;
        u = "MB";
    } else if (v >= 1e3) {
        v /= 1e3;
        u = "KB";
    }
    snprintf(out, 32, "%.2f %s", v, u);
}

int mp_p5z_info(const mp_p5z *p, FILE *out) {
    uint64_t orig = 0, packed = 0, nloose = 0, npacked = 0, vols = 0, volbytes = 0;
    for (size_t i = 0; i < p->man.n; i++) {
        orig += p->man.v[i].size;
        if (p->man.v[i].flags & MP_MANIFEST_PACKED) {
            npacked++;
            packed += p->man.v[i].size;
        } else {
            nloose++;
        }
    }
    for (size_t i = 0; i < p->nfiles; i++)
        if (p->files[i].role == MP_ROLE_VOLUME) {
            vols++;
            volbytes += p->files[i].size;
        }
    char a[32], b[32], c[32], d[32];
    size_text(orig, a);
    size_text(packed, b);
    size_text(volbytes, c);
    size_text(p->size, d);
    fprintf(out, "Title:      %s\n", p->man.title_name[0] ? p->man.title_name : "(no name)");
    fprintf(out, "ID:         %s   version %s\n", p->man.title_id[0] ? p->man.title_id : "?",
            p->man.content_version[0] ? p->man.content_version : "?");
    fprintf(out, "Files:      %llu (%llu loose, %llu packed), original %s\n",
            (unsigned long long)p->man.n, (unsigned long long)nloose, (unsigned long long)npacked, a);
    fprintf(out, "Packs:      zstd level %d, %s -> %s in %llu volumes\n", p->man.level, b, c,
            (unsigned long long)vols);
    fprintf(out, ".p5z:       %s\n", d);
    if (p->version == 2) {
        uint64_t unused = 0;
        fprintf(out, "Grown:      %u %s (DLC or an update added to it)\n", p->grown, p->grown == 1 ? "time" : "times");
        if (mp_p5z_unused((mp_p5z *)p, &unused, NULL) == 0) {
            char u[32];
            size_text(unused, u);
            fprintf(out, "Unused:     %s%s\n", u,
                    unused ? " (maxpack compact <game.p5z> -o <new.p5z> makes a copy without it)" : "");
        }
    }
    return ferror(out) ? -1 : 0;
}

int mp_p5z_unused(mp_p5z *p, uint64_t *unused, mp_err *e) {
    *unused = 0;
    if (p->version < 2) return 0;   // a fresh pack leaves no room between its regions
    uint64_t used = P5Z_HEADER + p->index_len + P5Z_FOOTER;
    int vols = 0;
    for (size_t i = 0; i < p->nfiles; i++) {
        if (p->files[i].role == MP_ROLE_VOLUME) {
            used += MP_ZPK_VOL_HEADER;   // a volume uses its header and its chunks
            vols = 1;
        } else {
            used += p->files[i].data_len;
        }
    }
    if (vols) {
        mp_zpk *z = NULL;
        if (mp_p5z_zpk(p, &z, e) != 0) return -1;
        used += mp_zpk_stored_bytes(z);
        mp_zpk_close(z);
    }
    *unused = p->size > used ? p->size - used : 0;
    return 0;
}

uint64_t mp_p5z_volume_offset(const mp_p5z *p, uint32_t vol) {
    const p5z_file *f = volume_file(p, vol);
    return f ? f->data_off : 0;
}
