// SPDX-License-Identifier: GPL-3.0-or-later
// .p5z writer. Volumes go raw (install = copy); the other files, in zstd 19 frames with a long
// window of up to 32 MiB, written in order. The frames are a pipeline: a pool compresses them while
// this thread reads the next ones and writes those already compressed, also across files.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_p5z_internal.h"
#include "mp_platform.h"

typedef struct {
    char *path;
    int role, storage;
    uint64_t size, data_off, data_len;
    int64_t mtime;
    uint32_t first_frame, nframes;
    uint8_t sha[32];
} p5file;

typedef struct {
    uint64_t off;
    uint32_t csize, usize;
} p5frame;

// A piece of a file in the frame pipeline: a frame (ilen > 0) or, with no data, only the end of
// the file (one that is empty or a multiple of 32 MiB).
typedef struct {
    uint8_t *in, *out;   // MP_LONG_FRAME and out_cap bytes, allocated when first needed
    size_t ilen, olen;
    size_t file;         // index in files
    int first, last;     // the file's first and last piece
} fslot;

typedef struct {
    mp_wfile *f;
    unsigned threads;          // compression threads (at most MP_LONG_MAX_THREADS)
    p5file *files;
    size_t nfiles, cap;
    p5frame *frames;
    size_t nframes, fcap;
    // File in progress and frame pipeline. The pool and its contexts (~120 MB each) exist only
    // while there are frames: they are freed before the volumes.
    int in_file;
    size_t cur_file;           // index in files of the file being received
    int cur_first;             // its first piece has not been sent yet
    mp_pipe *pipe;
    fslot *slots;
    size_t nslots;
    long cur;                  // slot being filled, or -1
    ZSTD_CCtx **cctx;          // one per thread of the pool
    size_t out_cap;
    // Volume in progress.
    mp_sha256 vsha;
    int64_t vol_file;          // index in files of the open volume
    // The file is created on the first write (a rejected source leaves nothing) and is deleted
    // if it is never finished (a .p5z without a footer is useless).
    char *path;
    int created, finished;
    uint64_t max_size;         // cap on the .p5z (0 = no cap)
    int too_big;               // stopped because of the cap
    // Appending to a .p5z that exists (p5z_out_append): f is the caller's, the index is version 2.
    int append;
    uint64_t prev_size;        // the size f had
    uint32_t grown;
} p5out;

static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// Before writing n more bytes at the end: fails if the .p5z would go over the cap, without writing them, so
// it never goes over it (on FAT32, a write that went past 4 GiB - 1 would fail earlier in the
// system, as an I/O error). What was written is deleted by the writer when it is destroyed.
static int room(p5out *o, size_t n, mp_err *e) {
    uint64_t size = o->f ? mp_wfile_size(o->f) : 0;
    if (o->max_size && (size > o->max_size || (uint64_t)n > o->max_size - size)) {
        o->too_big = 1;
        mp_err_set(e, "the .p5z would go over %llu bytes", (unsigned long long)o->max_size);
        return -1;
    }
    return 0;
}

// Writes at the end of the .p5z, if it fits under the cap.
static int put(p5out *o, const void *p, size_t n, mp_err *e) {
    if (room(o, n, e) != 0) return -1;
    return mp_wfile_write(o->f, p, n, e);
}

static int ensure_open(p5out *o, mp_err *e) {
    if (o->f) return 0;
    static const uint8_t zero[P5Z_HEADER] = {0};
    if (mp_wfile_create(o->path, &o->f, e) != 0) return -1;
    o->created = 1;   // CREATE_NEW: we created it, so we can delete it if it fails
    return put(o, zero, sizeof zero, e);
}

static int files_room(p5out *o, mp_err *e) {
    if (o->nfiles < o->cap) return 0;
    size_t cap = o->cap ? o->cap * 2 : 256;
    p5file *v = realloc(o->files, cap * sizeof *v);
    if (!v) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    o->files = v;
    o->cap = cap;
    return 0;
}

// Room for k more frames.
static int frames_room(p5out *o, size_t k, mp_err *e) {
    if (o->nframes + k <= o->fcap) return 0;
    size_t cap = o->fcap ? o->fcap : 1024;
    while (cap < o->nframes + k) cap *= 2;
    p5frame *v = realloc(o->frames, cap * sizeof *v);
    if (!v) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    o->frames = v;
    o->fcap = cap;
    return 0;
}

static p5file *new_file(p5out *o, const char *rel, int role, int storage, uint64_t size, int64_t mtime,
                        mp_err *e) {
    if (ensure_open(o, e) != 0) return NULL;
    if (files_room(o, e) != 0) return NULL;
    p5file *f = &o->files[o->nfiles];
    memset(f, 0, sizeof *f);
    if (!(f->path = mp_strdup(rel))) {
        mp_err_set(e, "out of memory");
        return NULL;
    }
    f->role = role;
    f->storage = storage;
    f->size = size;
    f->mtime = mtime;
    f->data_off = mp_wfile_size(o->f);
    f->first_frame = (uint32_t)o->nframes;
    o->nfiles++;
    return f;
}

// ---- volumes: raw regions ------------------------------------------------------------------

static int drain(p5out *o, int wait, mp_err *e);
static void frames_stop(p5out *o);

static int v_begin(void *ctx, uint32_t vol, mp_err *e) {
    p5out *o = ctx;
    if (drain(o, 1, e) != 0) return -1;   // the frames of the files before it go first
    frames_stop(o);                        // the packs have their own threads
    char name[32];
    snprintf(name, sizeof name, "ampr_zstd-%03u.pak", vol);
    if (!new_file(o, name, MP_ROLE_VOLUME, P5Z_STORE_RAW, 0, 0, e)) return -1;
    o->vol_file = (int64_t)o->nfiles - 1;
    mp_sha256_init(&o->vsha);   // SHA-256 of bytes [64, end): the header is fixed up at the end
    static const uint8_t zero[MP_ZPK_VOL_HEADER] = {0};
    return put(o, zero, sizeof zero, e);
}

static int v_write(void *ctx, const void *p, size_t n, mp_err *e) {
    p5out *o = ctx;
    if (room(o, n, e) != 0) return -1;
    mp_sha256_update(&o->vsha, p, n);
    return mp_wfile_write(o->f, p, n, e);
}

static int v_end(void *ctx, uint32_t vol, mp_err *e) {
    p5out *o = ctx;
    (void)vol;
    (void)e;
    p5file *f = &o->files[o->vol_file];
    f->data_len = f->size = mp_wfile_size(o->f) - f->data_off;
    mp_sha256_final(&o->vsha, f->sha);
    return 0;
}

static int v_header(void *ctx, uint32_t vol, const uint8_t hdr[64], mp_err *e) {
    p5out *o = ctx;
    char name[32];
    snprintf(name, sizeof name, "ampr_zstd-%03u.pak", vol);
    for (size_t i = 0; i < o->nfiles; i++)
        if (o->files[i].role == MP_ROLE_VOLUME && !strcmp(o->files[i].path, name))
            return mp_wfile_pwrite(o->f, hdr, 64, o->files[i].data_off, e);
    mp_err_set(e, "unknown volume: %s", name);
    return -1;
}

// ---- other files: zstd frames --------------------------------------------------------------

static int frame_work(void *vc, size_t i, unsigned tid) {
    p5out *o = vc;
    fslot *sl = &o->slots[i];
    size_t r = ZSTD_compress2(o->cctx[tid], sl->out, o->out_cap, sl->in, sl->ilen);
    if (ZSTD_isError(r)) return 1;
    sl->olen = r;
    return 0;
}

// Starts the pool and the compression contexts for the frames.
static int frames_start(p5out *o, mp_err *e) {
    if (o->pipe) return 0;
    // As many frames in memory as 1.0.1 had: compressing one takes far longer than reading the next.
    o->nslots = o->threads;
    o->cur = -1;
    if (!o->slots) o->slots = calloc(o->nslots, sizeof *o->slots);
    if (!o->cctx) o->cctx = calloc(o->threads, sizeof *o->cctx);
    int ok = o->slots && o->cctx;
    for (unsigned t = 0; ok && t < o->threads; t++)
        if (!o->cctx[t] && (ok = (o->cctx[t] = ZSTD_createCCtx()) != NULL)) mp_zstd_set_long(o->cctx[t]);
    if (ok) ok = (o->pipe = mp_pipe_new(o->threads, o->nslots, frame_work, o)) != NULL;
    if (!ok) mp_err_set(e, "out of memory");
    return ok ? 0 : -1;
}

// Joins the pool and frees the contexts and buffers (whatever was still pending is dropped).
static void frames_stop(p5out *o) {
    if (o->pipe) {
        mp_pipe_abort(o->pipe);
        mp_pipe_free(o->pipe);
        o->pipe = NULL;
    }
    for (size_t i = 0; o->slots && i < o->nslots; i++) {
        free(o->slots[i].in);
        free(o->slots[i].out);
    }
    free(o->slots);
    o->slots = NULL;
    if (o->cctx)
        for (unsigned t = 0; t < o->threads; t++) ZSTD_freeCCtx(o->cctx[t]);
    free(o->cctx);
    o->cctx = NULL;
    o->cur = -1;
}

// Writes a compressed piece: where its file starts (after the frames of the files before it), its
// frame and, if it is the last one, the file's size in the .p5z.
static int write_piece(p5out *o, const fslot *sl, int rc, mp_err *e) {
    p5file *f = &o->files[sl->file];
    if (sl->first) {
        f->data_off = mp_wfile_size(o->f);
        f->first_frame = (uint32_t)o->nframes;
    }
    if (sl->ilen) {
        if (rc != 0) {
            if (mp_cancel_check(e) == 0) mp_err_set(e, "error compressing for the .p5z");
            return -1;
        }
        if (frames_room(o, 1, e) != 0) return -1;
        p5frame *fr = &o->frames[o->nframes++];
        fr->off = mp_wfile_size(o->f);
        fr->csize = (uint32_t)sl->olen;
        fr->usize = (uint32_t)sl->ilen;
        if (put(o, sl->out, sl->olen, e) != 0) return -1;
    }
    if (sl->last) {
        f->nframes = (uint32_t)(o->nframes - f->first_frame);
        f->data_len = mp_wfile_size(o->f) - f->data_off;
    }
    return 0;
}

// Writes the oldest piece if it is compressed (with wait, waiting for it). 1 if it wrote one, 0 if
// not, -1 on error.
static int write_next(p5out *o, int wait, mp_err *e) {
    if (!o->pipe || !mp_pipe_pending(o->pipe)) return 0;
    int rc = 0;
    long s = mp_pipe_next(o->pipe, wait, &rc);
    if (s < 0) {
        if (!wait) return 0;
        mp_err_set(e, "error compressing for the .p5z");
        return -1;
    }
    int r = write_piece(o, &o->slots[s], rc, e);
    mp_pipe_release(o->pipe, (size_t)s);
    return r != 0 ? -1 : 1;
}

// Writes, in order, the pieces already compressed; with wait, all those sent.
static int drain(p5out *o, int wait, mp_err *e) {
    int r;
    while ((r = write_next(o, wait, e)) > 0) {
    }
    return r;
}

// The slot for the current file's next piece. When all are in use, the oldest is written first:
// this thread is also the one that empties the pipe, so it never waits for a free slot.
static fslot *open_slot(p5out *o, mp_err *e) {
    if (o->cur >= 0) return &o->slots[o->cur];
    if (frames_start(o, e) != 0) return NULL;
    if (mp_pipe_pending(o->pipe) >= o->nslots && write_next(o, 1, e) < 0) return NULL;
    long s = mp_pipe_acquire(o->pipe);
    if (s < 0) {
        mp_err_set(e, "error compressing for the .p5z");
        return NULL;
    }
    fslot *sl = &o->slots[s];
    sl->ilen = sl->olen = 0;
    sl->file = o->cur_file;
    sl->first = o->cur_first;
    sl->last = 0;
    o->cur_first = 0;
    o->cur = s;
    return sl;
}

// Sends the slot being filled to the pool (a piece without data needs no compression) and writes
// whatever is already compressed.
static int send_slot(p5out *o, int last, mp_err *e) {
    fslot *sl = &o->slots[o->cur];
    sl->last = last;
    mp_pipe_submit(o->pipe, (size_t)o->cur, sl->ilen > 0);
    o->cur = -1;
    return drain(o, 0, e);
}

static int f_begin(void *ctx, const char *rel, int role, uint64_t size, int64_t mtime, mp_err *e) {
    p5out *o = ctx;
    // data_off and first_frame are set again when its first piece is written
    if (!new_file(o, rel, role, P5Z_STORE_FRAMES, size, mtime, e)) return -1;
    o->cur_file = o->nfiles - 1;
    o->cur_first = 1;
    o->in_file = 1;
    return 0;
}

static int f_write(void *ctx, const void *p, size_t n, mp_err *e) {
    p5out *o = ctx;
    const uint8_t *b = p;
    while (n) {
        if (mp_cancel_check(e) != 0) return -1;
        fslot *sl = open_slot(o, e);
        if (!sl) return -1;
        if ((!sl->in && !(sl->in = malloc(MP_LONG_FRAME))) || (!sl->out && !(sl->out = malloc(o->out_cap)))) {
            mp_err_set(e, "out of memory");
            return -1;
        }
        size_t k = MP_LONG_FRAME - sl->ilen < n ? MP_LONG_FRAME - sl->ilen : n;
        memcpy(sl->in + sl->ilen, b, k);
        sl->ilen += k;
        b += k;
        n -= k;
        if (sl->ilen == MP_LONG_FRAME && send_slot(o, 0, e) != 0) return -1;
    }
    return 0;
}

static int f_end(void *ctx, const uint8_t sha[32], mp_err *e) {
    p5out *o = ctx;
    if (!open_slot(o, e)) return -1;   // with nothing pending, a piece that only ends the file
    memcpy(o->files[o->cur_file].sha, sha, 32);
    o->in_file = 0;
    return send_slot(o, 1, e);
}

// Before the packs: the frames of the files before them are written and the pool and its contexts
// freed now, so the packs' buffers and read-ahead do not add to them (v_begin would do the same, but
// only once the first block is compressed, after the read-ahead has filled up).
static int f_packs_begin(void *ctx, mp_err *e) {
    p5out *o = ctx;
    if (drain(o, 1, e) != 0) return -1;
    frames_stop(o);
    return 0;
}

// ---- index, footer and header --------------------------------------------------------------

typedef struct {
    const char *path;
    int group;   // 0 loose, 1 replaced original
    uint32_t idx;
} pathref;

static int cmp_ref(const void *a, const void *b) {
    const pathref *x = a, *y = b;
    if (x->group != y->group) return x->group - y->group;
    return strcmp(x->path, y->path);
}

// The order of the records of an index version 2: loose files by path, volumes by id, ampr_emu.index,
// the runtime, the originals by path, ampr_zstd.index (a fresh pack's order, whatever was kept or new).
static int rank_of(int role) {
    static const int rank[] = {0, 1, 5, 2, 3, 4};   // by role: loose, volume, zpk index, idx3, runtime, original
    return role >= 0 && role <= MP_ROLE_ORIGINAL ? rank[role] : 6;
}

static unsigned volume_id(const char *path) {
    unsigned id = 0;
    return sscanf(path, "ampr_zstd-%u.pak", &id) == 1 ? id : 0;
}

static int cmp_record(const void *a, const void *b) {
    const p5file *x = a, *y = b;
    int rx = rank_of(x->role), ry = rank_of(y->role);
    if (rx != ry) return rx - ry;
    if (x->role == MP_ROLE_VOLUME) {
        unsigned ix = volume_id(x->path), iy = volume_id(y->path);
        if (ix != iy) return ix < iy ? -1 : 1;
    }
    return strcmp(x->path, y->path);
}

// Sorts the records as an index version 2 has them, with their frames renumbered in that order.
static int canonical_order(p5out *o, mp_err *e) {
    qsort(o->files, o->nfiles, sizeof *o->files, cmp_record);
    size_t cap = o->nframes ? o->nframes : 1;
    p5frame *nf = malloc(cap * sizeof *nf);
    if (!nf) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    size_t at = 0;
    for (size_t i = 0; i < o->nfiles; i++) {
        p5file *f = &o->files[i];
        if (f->storage != P5Z_STORE_FRAMES) continue;
        memcpy(nf + at, o->frames + f->first_frame, f->nframes * sizeof *nf);
        f->first_frame = (uint32_t)at;
        at += f->nframes;
    }
    free(o->frames);
    o->frames = nf;
    o->fcap = cap;
    o->nframes = at;
    return 0;
}

static int f_finish(void *ctx, const mp_pack_manifest *m, mp_err *e) {
    p5out *o = ctx;
    if (drain(o, 1, e) != 0) return -1;
    frames_stop(o);
    if (o->append && canonical_order(o, e) != 0) return -1;
    // An append: what it added goes to the disk before its index and footer do, so a whole footer at the
    // end, after a power cut, means a whole append (the journal's recovery keeps it then).
    if (o->append && mp_wfile_sync_full(o->f, e) != 0) return -1;
    const uint32_t hs = o->append ? P5Z_INDEX_HEADER_V2 : P5Z_INDEX_HEADER;
    // For each manifest record, the stored file that contains it.
    pathref *refs = malloc((o->nfiles ? o->nfiles : 1) * sizeof *refs);
    size_t nr = 0;
    if (!refs) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < o->nfiles; i++)
        if (o->files[i].role == MP_ROLE_LOOSE || o->files[i].role == MP_ROLE_ORIGINAL) {
            refs[nr].path = o->files[i].path;
            refs[nr].group = o->files[i].role == MP_ROLE_ORIGINAL;
            refs[nr].idx = (uint32_t)i;
            nr++;
        }
    qsort(refs, nr, sizeof *refs, cmp_ref);
    uint64_t strings = strlen(m->title_id) + strlen(m->content_version) + strlen(m->title_name);
    for (size_t i = 0; i < o->nfiles; i++) strings += strlen(o->files[i].path);
    for (size_t i = 0; i < m->n; i++) strings += strlen(m->v[i].path);
    uint64_t size = hs + (uint64_t)P5Z_FILE_REC * o->nfiles + (uint64_t)P5Z_FRAME_REC * o->nframes +
                    (uint64_t)P5Z_MANIFEST_REC * m->n + strings;
    uint8_t *ix = calloc(1, (size_t)size);
    if (!ix) {
        free(refs);
        mp_err_set(e, "out of memory");
        return -1;
    }
    uint8_t *sp = ix + size - strings;
    uint32_t soff = 0;
#define PUT_STR(s, at)                                                                             \
    do {                                                                                           \
        size_t l_ = strlen(s);                                                                     \
        memcpy(sp + soff, (s), l_);                                                                \
        wr32((at), soff);                                                                          \
        wr32((at) + 4, (uint32_t)l_);                                                              \
        soff += (uint32_t)l_;                                                                      \
    } while (0)
    memcpy(ix, "P5ZIDX01", 8);
    wr32(ix + 8, o->append ? 2 : 1);
    if (o->append) {
        wr64(ix + 56, o->prev_size);
        wr32(ix + 64, o->grown);
    }
    wr32(ix + 12, (uint32_t)o->nfiles);
    wr32(ix + 16, (uint32_t)o->nframes);
    wr32(ix + 20, (uint32_t)m->n);
    wr64(ix + 24, strings);
    PUT_STR(m->title_id, ix + 32);
    PUT_STR(m->content_version, ix + 40);
    PUT_STR(m->title_name, ix + 48);
    uint8_t *r = ix + hs;
    for (size_t i = 0; i < o->nfiles; i++, r += P5Z_FILE_REC) {
        const p5file *f = &o->files[i];
        PUT_STR(f->path, r);
        r[8] = (uint8_t)f->role;
        r[9] = (uint8_t)f->storage;
        wr64(r + 12, f->size);
        wr64(r + 20, (uint64_t)f->mtime);
        wr64(r + 28, f->data_off);
        wr64(r + 36, f->data_len);
        wr32(r + 44, f->first_frame);
        wr32(r + 48, f->nframes);
        memcpy(r + 52, f->sha, 32);
    }
    for (size_t i = 0; i < o->nframes; i++, r += P5Z_FRAME_REC) {
        wr64(r, o->frames[i].off);
        wr32(r + 8, o->frames[i].csize);
        wr32(r + 12, o->frames[i].usize);
    }
    int rc = 0;
    for (size_t i = 0; i < m->n; i++, r += P5Z_MANIFEST_REC) {
        const mp_manifest_entry *me = &m->v[i];
        uint32_t fi = P5Z_NO_FILE;
        if (!(me->flags & MP_MANIFEST_PACKED)) {
            pathref key = {me->path, (me->flags & MP_MANIFEST_REPLACED) ? 1 : 0, 0};
            pathref *hit = bsearch(&key, refs, nr, sizeof *refs, cmp_ref);
            if (!hit) {
                mp_err_set(e, "the original file %s is missing from the .p5z", me->path);
                rc = -1;
                break;
            }
            fi = hit->idx;
        }
        PUT_STR(me->path, r);
        wr32(r + 8, me->flags);
        wr32(r + 12, fi);
        wr64(r + 16, me->size);
        wr64(r + 24, (uint64_t)me->mtime);
        memcpy(r + 32, me->sha, 32);
    }
#undef PUT_STR
    free(refs);
    // Compressed index (one frame), footer and header.
    size_t cap = ZSTD_compressBound((size_t)size);
    uint8_t *cz = rc ? NULL : malloc(cap);
    size_t clen = 0;
    if (!rc && cz) {
        clen = ZSTD_compress(cz, cap, ix, (size_t)size, 19);
        if (ZSTD_isError(clen)) rc = -1;
    } else if (!rc) {
        rc = -1;
    }
    if (rc && !e->msg[0]) mp_err_set(e, "could not compress the .p5z's index");
    if (!rc) rc = ensure_open(o, e);
    uint64_t ioff = rc ? 0 : mp_wfile_size(o->f);
    if (!rc) rc = put(o, cz, clen, e);
    if (!rc) {
        uint8_t ft[P5Z_FOOTER] = {0};
        memcpy(ft, "P5ZFOOT1", 8);
        wr64(ft + 8, ioff);
        wr64(ft + 16, clen);
        wr32(ft + 24, mp_crc32(cz, clen, 0));
        wr32(ft + 28, mp_crc32(ft, 28, 0));
        rc = put(o, ft, sizeof ft, e);
    }
    if (!rc && !o->append) {   // a .p5z that grows keeps the header it has
        uint8_t h[P5Z_HEADER] = {0};
        memcpy(h, "MAXPKP5Z", 8);
        wr32(h + 8, 1);
        wr32(h + 12, P5Z_HEADER);
        h[20] = (uint8_t)m->level;
        memcpy(h + 24, m->archive_id, 16);
        wr32(h + 60, mp_crc32(h, 64, 0));
        rc = mp_wfile_pwrite(o->f, h, sizeof h, 0, e);
    }
    free(cz);
    free(ix);
    if (!rc && o->append) {
        o->finished = 1;   // f is the caller's: it syncs and closes it
    } else if (!rc) {
        rc = mp_wfile_close(o->f, e);
        o->f = NULL;
        o->finished = rc == 0;
    }
    return rc;
}

static void f_destroy(void *ctx) {
    p5out *o = ctx;
    if (!o) return;
    frames_stop(o);   // joins the pool before anything it uses is freed
    if (o->f && !o->append) mp_wfile_close(o->f, NULL);
    if (o->created && !o->finished) mp_remove_file(o->path);   // half-written .p5z: useless
    free(o->path);
    for (size_t i = 0; i < o->nfiles; i++) free(o->files[i].path);
    free(o->files);
    free(o->frames);
    free(o);
}

int mp_pack_out_p5z_too_big(const mp_pack_out *out) {
    return out && out->destroy == f_destroy && out->ctx && ((const p5out *)out->ctx)->too_big;
}

int mp_pack_out_p5z(const char *path, unsigned threads, mp_pack_out *out, mp_err *e) {
    return mp_pack_out_p5z_ex(path, threads, 0, out, e);
}

static void set_out(p5out *o, mp_pack_out *out) {
    out->zpk.vol_begin = v_begin;
    out->zpk.vol_write = v_write;
    out->zpk.vol_end = v_end;
    out->zpk.vol_header = v_header;
    out->zpk.ctx = o;
    out->file_begin = f_begin;
    out->file_write = f_write;
    out->file_end = f_end;
    out->packs_begin = f_packs_begin;
    out->finish = f_finish;
    out->destroy = f_destroy;
    out->ctx = o;
}

int p5z_out_append(mp_wfile *f, unsigned threads, uint64_t max_size, uint32_t grown, mp_pack_out *out, mp_err *e) {
    memset(out, 0, sizeof *out);
    p5out *o = calloc(1, sizeof *o);
    if (!o) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    o->max_size = max_size;
    o->threads = threads == 0 ? 1 : threads > MP_LONG_MAX_THREADS ? MP_LONG_MAX_THREADS : threads;
    o->out_cap = ZSTD_compressBound(MP_LONG_FRAME);
    o->cur = -1;
    o->f = f;
    o->append = 1;
    o->prev_size = mp_wfile_size(f);
    o->grown = grown;
    set_out(o, out);
    return 0;
}

static p5out *writer_of(mp_pack_out *out, mp_err *e) {
    if (!out || out->destroy != f_destroy || !out->ctx) {
        mp_err_set(e, "not a .p5z output");
        return NULL;
    }
    return out->ctx;
}

int p5z_out_keep(mp_pack_out *out, const mp_p5z *from, size_t i, int64_t mtime, mp_err *e) {
    p5out *o = writer_of(out, e);
    if (!o) return -1;
    const p5z_file *s = &from->files[i];
    // what is still being compressed goes first, so that each file's frames stay together
    if (drain(o, 1, e) != 0 || files_room(o, e) != 0 || frames_room(o, s->nframes, e) != 0) return -1;
    p5file *f = &o->files[o->nfiles];
    memset(f, 0, sizeof *f);
    if (!(f->path = mp_strdup(s->path))) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    f->role = s->role;
    f->storage = s->storage;
    f->size = s->size;
    f->data_off = s->data_off;
    f->data_len = s->data_len;
    f->mtime = mtime;
    memcpy(f->sha, s->sha, 32);
    f->first_frame = (uint32_t)o->nframes;
    f->nframes = s->storage == P5Z_STORE_FRAMES ? s->nframes : 0;
    for (uint32_t k = 0; k < f->nframes; k++) {
        const p5z_frame *fr = &from->frames[s->first_frame + k];
        o->frames[o->nframes].off = fr->off;
        o->frames[o->nframes].csize = fr->csize;
        o->frames[o->nframes].usize = fr->usize;
        o->nframes++;
    }
    o->nfiles++;
    return 0;
}

#define COPY_PIECE (4u << 20)

static int copy_range(p5out *o, mp_file *src, uint64_t off, uint64_t len, uint8_t *buf, mp_err *e) {
    for (uint64_t at = 0; at < len; at += COPY_PIECE) {
        size_t k = len - at < COPY_PIECE ? (size_t)(len - at) : COPY_PIECE, got = 0;
        if (mp_cancel_check(e) != 0 || mp_file_pread(src, buf, k, off + at, &got, e) != 0) return -1;
        if (got != k) {
            mp_err_set(e, "truncated .p5z (data missing at %llu)", (unsigned long long)(off + at));
            return -1;
        }
        if (put(o, buf, k, e) != 0) return -1;
    }
    return 0;
}

int p5z_out_copy(mp_pack_out *out, mp_p5z *from, size_t i, mp_err *e) {
    p5out *o = writer_of(out, e);
    if (!o) return -1;
    const p5z_file *s = &from->files[i];
    if (drain(o, 1, e) != 0) return -1;
    p5file *f = new_file(o, s->path, s->role, s->storage, s->size, s->mtime, e);
    if (!f) return -1;
    size_t fi = o->nfiles - 1;
    memcpy(f->sha, s->sha, 32);
    uint8_t *buf = malloc(COPY_PIECE);
    int rc = buf ? 0 : -1;
    if (!buf) mp_err_set(e, "out of memory");
    if (!rc && s->storage == P5Z_STORE_RAW) {
        rc = copy_range(o, from->f, s->data_off, s->data_len, buf, e);
    } else if (!rc) {
        for (uint32_t k = 0; !rc && k < s->nframes; k++) {
            const p5z_frame *fr = &from->frames[s->first_frame + k];
            uint64_t at = mp_wfile_size(o->f);
            rc = copy_range(o, from->f, fr->off, fr->csize, buf, e);
            if (!rc) rc = frames_room(o, 1, e);
            if (!rc) {
                o->frames[o->nframes].off = at;
                o->frames[o->nframes].csize = fr->csize;
                o->frames[o->nframes].usize = fr->usize;
                o->nframes++;
            }
        }
    }
    free(buf);
    f = &o->files[fi];
    f->nframes = (uint32_t)(o->nframes - f->first_frame);
    f->data_len = mp_wfile_size(o->f) - f->data_off;
    return rc;
}

int mp_pack_out_p5z_ex(const char *path, unsigned threads, uint64_t max_size, mp_pack_out *out, mp_err *e) {
    memset(out, 0, sizeof *out);
    p5out *o = calloc(1, sizeof *o);
    if (!o) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    o->max_size = max_size;
    o->threads = threads == 0 ? 1 : threads > MP_LONG_MAX_THREADS ? MP_LONG_MAX_THREADS : threads;
    o->out_cap = ZSTD_compressBound(MP_LONG_FRAME);
    o->cur = -1;
    if (mp_path_kind(path) != 0) {
        mp_err_set(e, "the output .p5z already exists: %s", path);
        f_destroy(o);
        return -1;
    }
    if (!(o->path = mp_strdup(path))) {
        f_destroy(o);
        mp_err_set(e, "out of memory");
        return -1;
    }
    set_out(o, out);
    return 0;
}
