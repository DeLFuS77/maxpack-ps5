// SPDX-License-Identifier: GPL-3.0-or-later
// The files of the dump a .p5z was made of, as a source (mp_p5z_source in mp_p5z.h): each manifest record
// is a file, read from its frames, its raw region or the packs. Growing a .p5z takes the game from here, so
// the files that stay as they are never leave the .p5z.
#define ZSTD_STATIC_LINKING_ONLY
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "mp_p5z_internal.h"
#include "mp_source_impl.h"

typedef struct {
    mp_p5z *p;
    mp_zpk *z;
    size_t *man;   // of each entry, its manifest record
    long *zi;      // and, if packed, its file in the packs
} p5src;

static int pread_exact(mp_file *f, void *buf, size_t n, uint64_t off, mp_err *e) {
    size_t got = 0;
    if (mp_file_pread(f, buf, n, off, &got, e) != 0) return -1;
    if (got != n) {
        mp_err_set(e, "truncated .p5z (data missing at %llu)", (unsigned long long)off);
        return -1;
    }
    return 0;
}

// [off, off + len) of a stored file: its raw region, or the frames it overlaps decompressed.
static int read_stored(mp_p5z *p, const p5z_file *f, uint64_t off, uint8_t *buf, size_t len, mp_err *e) {
    if (f->storage == P5Z_STORE_RAW) return pread_exact(p->f, buf, len, f->data_off + off, e);
    uint64_t at = 0;   // where frame k starts in the file
    uint8_t *in = NULL, *dec = NULL;
    int rc = 0;
    for (uint32_t k = 0; k < f->nframes && len && !rc; k++) {
        const p5z_frame *fr = &p->frames[f->first_frame + k];
        if (off >= at + fr->usize) {
            at += fr->usize;
            continue;
        }
        if (!in) {
            in = malloc(ZSTD_compressBound(MP_LONG_FRAME));
            dec = malloc(MP_LONG_FRAME);
            if (!in || !dec) {
                mp_err_set(e, "out of memory");
                rc = -1;
                break;
            }
        }
        if (fr->csize > ZSTD_compressBound(MP_LONG_FRAME) || fr->usize > MP_LONG_FRAME) {
            mp_err_set(e, "invalid .p5z: frame too large");
            rc = -1;
            break;
        }
        rc = pread_exact(p->f, in, fr->csize, fr->off, e);
        if (rc) break;
        size_t got = ZSTD_decompress(dec, fr->usize, in, fr->csize);
        if (ZSTD_isError(got) || got != fr->usize) {
            mp_err_set(e, "%s: damaged data in the .p5z", f->path);
            rc = -1;
            break;
        }
        size_t from = (size_t)(off - at), take = fr->usize - from < len ? fr->usize - from : len;
        memcpy(buf, dec + from, take);
        buf += take;
        len -= take;
        off += take;
        at += fr->usize;
    }
    if (!rc && len) {
        mp_err_set(e, "%s: shorter in the .p5z than its record", f->path);
        rc = -1;
    }
    free(in);
    free(dec);
    return rc;
}

static int p5src_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e) {
    p5src *x = s->impl;
    if (!len) return 0;
    if (x->zi[i] >= 0) return mp_zpk_read_file(x->z, (size_t)x->zi[i], off, buf, len, e);
    return read_stored(x->p, &x->p->files[x->p->man_file[x->man[i]]], off, buf, len, e);
}

static void p5src_close(mp_source *s) {
    p5src *x = s->impl;
    if (!x) return;
    mp_zpk_close(x->z);   // the entries go with the source (mp_source_close)
    free(x->man);
    free(x->zi);
    free(x);
}

int mp_p5z_source(mp_p5z *p, int (*take)(void *ctx, const char *path), void *ctx, mp_source **out, mp_err *e) {
    *out = NULL;
    const mp_pack_manifest *m = &p->man;
    mp_source *s = calloc(1, sizeof *s);
    p5src *x = calloc(1, sizeof *x);
    size_t cap = m->n ? m->n : 1;
    if (s) s->entries = calloc(cap, sizeof *s->entries);
    if (x) {
        x->man = malloc(cap * sizeof *x->man);
        x->zi = malloc(cap * sizeof *x->zi);
    }
    if (!s || !x || !s->entries || !x->man || !x->zi) {
        if (s) free(s->entries);
        free(s);
        if (x) {
            free(x->man);
            free(x->zi);
        }
        free(x);
        mp_err_set(e, "out of memory");
        return -1;
    }
    x->p = p;
    s->kind = MP_SRC_DIR;
    s->read = p5src_read;
    s->close = p5src_close;
    s->impl = x;
    int packs = 0;
    for (size_t k = 0; k < m->n; k++) packs |= (m->v[k].flags & MP_MANIFEST_PACKED) != 0;
    if (packs && mp_p5z_zpk(p, &x->z, e) != 0) goto fail;
    for (size_t k = 0; k < m->n; k++) {
        const mp_manifest_entry *me = &m->v[k];
        if (take && !take(ctx, me->path)) continue;
        if (s->n && strcmp(s->entries[s->n - 1].path, me->path) >= 0) {
            mp_err_set(e, "invalid .p5z: its manifest is not in path order");
            goto fail;
        }
        long zi = -1;
        if (me->flags & MP_MANIFEST_PACKED) {
            zi = mp_zpk_find(x->z, me->path);
            if (zi < 0 || mp_zpk_size(x->z, (size_t)zi) != me->size) {
                mp_err_set(e, "%s is not in the .p5z's packs", me->path);
                goto fail;
            }
        }
        if (!(s->entries[s->n].path = mp_strdup(me->path))) {
            mp_err_set(e, "out of memory");
            goto fail;
        }
        s->entries[s->n].size = me->size;
        s->entries[s->n].mtime = me->mtime;
        x->man[s->n] = k;
        x->zi[s->n] = zi;
        s->n++;
    }
    *out = s;
    return 0;
fail:
    mp_source_close(s);
    return -1;
}
