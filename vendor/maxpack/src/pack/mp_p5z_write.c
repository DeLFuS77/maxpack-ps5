// SPDX-License-Identifier: GPL-3.0-or-later
// Escritor del .p5z. Los volúmenes van en bruto (instalar = copiar); los demás archivos, en frames
// zstd 19 con ventana larga de hasta 32 MiB, comprimidos en paralelo y escritos en orden.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_p5z.h"
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

typedef struct {
    uint8_t *in, *out;
    size_t ilen, olen;
} fjob;

typedef struct {
    mp_wfile *f;
    unsigned threads;          // hilos de compresión (máximo MP_LONG_MAX_THREADS)
    p5file *files;
    size_t nfiles, cap;
    p5frame *frames;
    size_t nframes, fcap;
    // Archivo en curso.
    int in_file;
    uint8_t *pend;             // datos pendientes de comprimir (threads × 32 MiB)
    size_t npend;
    fjob *jobs;
    ZSTD_CCtx **cctx;
    size_t out_cap;
    // Volumen en curso.
    mp_sha256 vsha;
    int64_t vol_file;          // índice en files del volumen abierto
    // El archivo se crea con la primera escritura (un origen rechazado no deja nada) y se borra
    // si no se llega a terminar (un .p5z sin pie no sirve).
    char *path;
    int created, finished;
    uint64_t max_size;         // tope del .p5z (0 = sin tope)
    int too_big;               // se paró por el tope
} p5out;

static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// Antes de escribir n bytes más al final: falla si el .p5z pasaría del tope, sin escribirlos, así
// que nunca pasa de él (en FAT32, la escritura que pasara de 4 GiB - 1 fallaría antes en el
// sistema, como un error de E/S). Lo escrito lo borra el escritor al destruirse.
static int room(p5out *o, size_t n, mp_err *e) {
    uint64_t size = o->f ? mp_wfile_size(o->f) : 0;
    if (o->max_size && (size > o->max_size || (uint64_t)n > o->max_size - size)) {
        o->too_big = 1;
        mp_err_set(e, "el .p5z pasaría de %llu bytes", (unsigned long long)o->max_size);
        return -1;
    }
    return 0;
}

// Escribe al final del .p5z, si cabe en el tope.
static int put(p5out *o, const void *p, size_t n, mp_err *e) {
    if (room(o, n, e) != 0) return -1;
    return mp_wfile_write(o->f, p, n, e);
}

static int ensure_open(p5out *o, mp_err *e) {
    if (o->f) return 0;
    static const uint8_t zero[P5Z_HEADER] = {0};
    if (mp_wfile_create(o->path, &o->f, e) != 0) return -1;
    o->created = 1;   // CREATE_NEW: lo hemos creado nosotros, así que podemos borrarlo si falla
    return put(o, zero, sizeof zero, e);
}

static p5file *new_file(p5out *o, const char *rel, int role, int storage, uint64_t size, int64_t mtime,
                        mp_err *e) {
    if (ensure_open(o, e) != 0) return NULL;
    if (o->nfiles == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 256;
        p5file *v = realloc(o->files, cap * sizeof *v);
        if (!v) {
            mp_err_set(e, "sin memoria");
            return NULL;
        }
        o->files = v;
        o->cap = cap;
    }
    p5file *f = &o->files[o->nfiles];
    memset(f, 0, sizeof *f);
    if (!(f->path = mp_strdup(rel))) {
        mp_err_set(e, "sin memoria");
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

// ---- volúmenes: regiones en bruto ----------------------------------------------------------

static int v_begin(void *ctx, uint32_t vol, mp_err *e) {
    p5out *o = ctx;
    char name[32];
    snprintf(name, sizeof name, "ampr_zstd-%03u.pak", vol);
    if (!new_file(o, name, MP_ROLE_VOLUME, P5Z_STORE_RAW, 0, 0, e)) return -1;
    o->vol_file = (int64_t)o->nfiles - 1;
    mp_sha256_init(&o->vsha);   // SHA-256 de los bytes [64, fin): la cabecera se corrige al final
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
    mp_err_set(e, "volumen desconocido: %s", name);
    return -1;
}

// ---- demás archivos: frames zstd -----------------------------------------------------------

static int frame_work(void *vc, size_t i, unsigned tid) {
    p5out *o = vc;
    fjob *j = &o->jobs[i];
    size_t r = ZSTD_compress2(o->cctx[tid], j->out, o->out_cap, j->in, j->ilen);
    if (ZSTD_isError(r)) return 1;
    j->olen = r;
    return 0;
}

// Comprime y escribe los frames completos pendientes (o todo, si last).
static int flush_frames(p5out *o, int last, mp_err *e) {
    size_t nj = 0, off = 0;
    while (off < o->npend && (last || o->npend - off >= MP_LONG_FRAME)) {
        size_t k = o->npend - off < MP_LONG_FRAME ? o->npend - off : MP_LONG_FRAME;
        o->jobs[nj].in = o->pend + off;
        o->jobs[nj].ilen = k;
        nj++;
        off += k;
    }
    if (!nj) return 0;
    if (mp_parallel_run(o->threads, nj, frame_work, o, NULL) != 0) {
        if (mp_cancel_check(e) == 0) mp_err_set(e, "error al comprimir para el .p5z");
        return -1;
    }
    for (size_t j = 0; j < nj; j++) {
        if (o->nframes == o->fcap) {
            size_t cap = o->fcap ? o->fcap * 2 : 1024;
            p5frame *v = realloc(o->frames, cap * sizeof *v);
            if (!v) {
                mp_err_set(e, "sin memoria");
                return -1;
            }
            o->frames = v;
            o->fcap = cap;
        }
        p5frame *fr = &o->frames[o->nframes++];
        fr->off = mp_wfile_size(o->f);
        fr->csize = (uint32_t)o->jobs[j].olen;
        fr->usize = (uint32_t)o->jobs[j].ilen;
        if (put(o, o->jobs[j].out, o->jobs[j].olen, e) != 0) return -1;
    }
    memmove(o->pend, o->pend + off, o->npend - off);
    o->npend -= off;
    return 0;
}

static int f_begin(void *ctx, const char *rel, int role, uint64_t size, int64_t mtime, mp_err *e) {
    p5out *o = ctx;
    if (!new_file(o, rel, role, P5Z_STORE_FRAMES, size, mtime, e)) return -1;
    o->in_file = 1;
    o->npend = 0;
    return 0;
}

static int f_write(void *ctx, const void *p, size_t n, mp_err *e) {
    p5out *o = ctx;
    const uint8_t *b = p;
    size_t cap = (size_t)o->threads * MP_LONG_FRAME;
    while (n) {
        if (mp_cancel_check(e) != 0) return -1;
        size_t k = cap - o->npend < n ? cap - o->npend : n;
        memcpy(o->pend + o->npend, b, k);
        o->npend += k;
        b += k;
        n -= k;
        if (o->npend == cap && flush_frames(o, 0, e) != 0) return -1;
    }
    return 0;
}

static int f_end(void *ctx, const uint8_t sha[32], mp_err *e) {
    p5out *o = ctx;
    if (flush_frames(o, 1, e) != 0) return -1;
    p5file *f = &o->files[o->nfiles - 1];
    f->nframes = (uint32_t)(o->nframes - f->first_frame);
    f->data_len = mp_wfile_size(o->f) - f->data_off;
    memcpy(f->sha, sha, 32);
    o->in_file = 0;
    return 0;
}

// ---- índice, pie y cabecera ----------------------------------------------------------------

typedef struct {
    const char *path;
    int group;   // 0 suelto, 1 original reemplazado
    uint32_t idx;
} pathref;

static int cmp_ref(const void *a, const void *b) {
    const pathref *x = a, *y = b;
    if (x->group != y->group) return x->group - y->group;
    return strcmp(x->path, y->path);
}

static int f_finish(void *ctx, const mp_pack_manifest *m, mp_err *e) {
    p5out *o = ctx;
    // Para cada registro del manifiesto, el archivo guardado que lo contiene.
    pathref *refs = malloc((o->nfiles ? o->nfiles : 1) * sizeof *refs);
    size_t nr = 0;
    if (!refs) {
        mp_err_set(e, "sin memoria");
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
    uint64_t size = P5Z_INDEX_HEADER + (uint64_t)P5Z_FILE_REC * o->nfiles + (uint64_t)P5Z_FRAME_REC * o->nframes +
                    (uint64_t)P5Z_MANIFEST_REC * m->n + strings;
    uint8_t *ix = calloc(1, (size_t)size);
    if (!ix) {
        free(refs);
        mp_err_set(e, "sin memoria");
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
    wr32(ix + 8, 1);
    wr32(ix + 12, (uint32_t)o->nfiles);
    wr32(ix + 16, (uint32_t)o->nframes);
    wr32(ix + 20, (uint32_t)m->n);
    wr64(ix + 24, strings);
    PUT_STR(m->title_id, ix + 32);
    PUT_STR(m->content_version, ix + 40);
    PUT_STR(m->title_name, ix + 48);
    uint8_t *r = ix + P5Z_INDEX_HEADER;
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
                mp_err_set(e, "falta en el .p5z el archivo original %s", me->path);
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
    // Índice comprimido (un frame), pie y cabecera.
    size_t cap = ZSTD_compressBound((size_t)size);
    uint8_t *cz = rc ? NULL : malloc(cap);
    size_t clen = 0;
    if (!rc && cz) {
        clen = ZSTD_compress(cz, cap, ix, (size_t)size, 19);
        if (ZSTD_isError(clen)) rc = -1;
    } else if (!rc) {
        rc = -1;
    }
    if (rc && !e->msg[0]) mp_err_set(e, "no se pudo comprimir el índice del .p5z");
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
    if (!rc) {
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
    if (!rc) {
        rc = mp_wfile_close(o->f, e);
        o->f = NULL;
        o->finished = rc == 0;
    }
    return rc;
}

static void f_destroy(void *ctx) {
    p5out *o = ctx;
    if (!o) return;
    if (o->f) mp_wfile_close(o->f, NULL);
    if (o->created && !o->finished) mp_remove_file(o->path);   // .p5z a medias: no sirve
    free(o->path);
    for (size_t i = 0; i < o->nfiles; i++) free(o->files[i].path);
    free(o->files);
    free(o->frames);
    free(o->pend);
    if (o->jobs)
        for (unsigned t = 0; t < o->threads; t++) free(o->jobs[t].out);
    free(o->jobs);
    if (o->cctx)
        for (unsigned t = 0; t < o->threads; t++) ZSTD_freeCCtx(o->cctx[t]);
    free(o->cctx);
    free(o);
}

int mp_pack_out_p5z_too_big(const mp_pack_out *out) {
    return out && out->destroy == f_destroy && out->ctx && ((const p5out *)out->ctx)->too_big;
}

int mp_pack_out_p5z(const char *path, unsigned threads, mp_pack_out *out, mp_err *e) {
    return mp_pack_out_p5z_ex(path, threads, 0, out, e);
}

int mp_pack_out_p5z_ex(const char *path, unsigned threads, uint64_t max_size, mp_pack_out *out, mp_err *e) {
    memset(out, 0, sizeof *out);
    p5out *o = calloc(1, sizeof *o);
    if (!o) {
        mp_err_set(e, "sin memoria");
        return -1;
    }
    o->max_size = max_size;
    o->threads = threads == 0 ? 1 : threads > MP_LONG_MAX_THREADS ? MP_LONG_MAX_THREADS : threads;
    o->out_cap = ZSTD_compressBound(MP_LONG_FRAME);
    o->pend = malloc((size_t)o->threads * MP_LONG_FRAME);
    o->jobs = calloc(o->threads, sizeof *o->jobs);
    o->cctx = calloc(o->threads, sizeof *o->cctx);
    int ok = o->pend && o->jobs && o->cctx;
    for (unsigned t = 0; ok && t < o->threads; t++) {
        ok = (o->jobs[t].out = malloc(o->out_cap)) && (o->cctx[t] = ZSTD_createCCtx());
        if (ok) mp_zstd_set_long(o->cctx[t]);
    }
    if (!ok) {
        f_destroy(o);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    if (mp_path_kind(path) != 0) {
        mp_err_set(e, "el .p5z de salida ya existe: %s", path);
        f_destroy(o);
        return -1;
    }
    if (!(o->path = mp_strdup(path))) {
        f_destroy(o);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    out->zpk.vol_begin = v_begin;
    out->zpk.vol_write = v_write;
    out->zpk.vol_end = v_end;
    out->zpk.vol_header = v_header;
    out->zpk.ctx = o;
    out->file_begin = f_begin;
    out->file_write = f_write;
    out->file_end = f_end;
    out->finish = f_finish;
    out->destroy = f_destroy;
    out->ctx = o;
    return 0;
}
