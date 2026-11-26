// SPDX-License-Identifier: GPL-3.0-or-later
// PFS image reader (.ffpfs and .ffpfsc, read-only): the unsigned PlayStation File System as MkPFS
// writes it. The layout was read from MkPFS's code (PSBrew/MkPFS, GPL-3.0; no code taken) and the
// PFS pages of psdevwiki; see docs/source-images.md. Block 0 is the header, the inode table
// follows, then the super-root folder (whose "uroot" is the root of the files). An unsigned file's
// bytes are contiguous from its first block, PFSC-compressed when its inode says so. A PFS image
// whose only file is itself an image (exFAT, UFS2 or PFS, by its signature) is a container: the
// source is then that image, read through this one.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_platform.h"
#include "mp_source_impl.h"

#define HDR_VERSION 0x00
#define HDR_MODE 0x1C
#define HDR_BSIZE 0x20
#define HDR_NINODES 0x30
#define HDR_NDBLOCK 0x38
#define HDR_INODE_BLOCKS 0x40
#define HDR_SUPERROOT 0x48
#define MODE_SIGNED 0x1
#define MODE_ENCRYPTED 0x4

#define INODE 0xA8   // unsigned 32-bit inode (D32)
#define DI_MODE 0x00
#define DI_FLAGS 0x04
#define DI_SIZE 0x08
#define DI_SIZE2 0x10   // the logical size of a compressed file
#define DI_MTIME 0x20   // the second of the four times (atime, mtime, ctime, birth), as in UFS
#define DI_BLOCKS 0x60
#define DI_DB 0x64
#define FLAG_COMPRESSED 0x1
#define IF_DIR 0x4000
#define IF_FILE 0x8000

#define DT_FILE 2
#define DT_DIR 3
#define DT_DOT 4
#define DT_DOTDOT 5
#define MAX_DIR (256ull << 20)

typedef struct {
    uint64_t off, stored;   // where its bytes are in the image
    int compressed;
    mp_pfsc *dec;           // decoder of a compressed file
} pfs_file;

typedef struct {
    mp_entry en;
    pfs_file f;
} rec;

typedef struct {
    mp_image *img;
    uint32_t bs, ipb;
    uint64_t ninodes, iblocks, ndblock, entries;
    uint64_t dir_bytes;   // of the folders read so far
    uint8_t *itab;   // the inode table
    uint8_t *seen;   // one bit per inode
    rec *v;
    size_t n, cap, ndirs;
    mp_image_range *rg;
    size_t nrg, caprg;
    pfs_file *files;   // parallel to mp_source.entries
} pfs;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int oom(mp_err *e) {
    mp_err_set(e, "out of memory");
    return -1;
}

static const char *shown(const char *path) { return path[0] ? path : "/"; }

static const uint8_t *inode(pfs *x, uint64_t ino) {
    return x->itab + (ino / x->ipb) * x->bs + (ino % x->ipb) * INODE;
}

// Where the stored bytes of inode di are; checks they are inside the image.
static int locate(pfs *x, const uint8_t *di, uint64_t stored, const char *path, uint64_t *off, mp_err *e) {
    *off = 0;
    if (stored == 0) return 0;
    int64_t db0 = (int32_t)rd32(di + DI_DB);
    uint64_t blocks = rd32(di + DI_BLOCKS), need = (stored + x->bs - 1) / x->bs;
    if (db0 <= (int64_t)x->iblocks || need > blocks || (uint64_t)db0 + need > x->ndblock) {
        mp_err_set(e, "damaged PFS image: the blocks of %s are out of range", shown(path));
        return -1;
    }
    for (uint64_t k = 1; k < 12 && k < need; k++) {
        int64_t b = (int32_t)rd32(di + DI_DB + 4 * k);
        if (b != -1 && b != 0 && b != db0 + (int64_t)k) {
            mp_err_set(e, "%s: the blocks of %s are not contiguous, which an unsigned PFS image does not do",
                       x->img->name, shown(path));
            return -1;
        }
    }
    uint64_t pos = (uint64_t)db0 * x->bs;
    if (pos > x->img->size || stored > x->img->size - pos) {
        mp_err_set(e, "truncated PFS image: the content of %s is missing", shown(path));
        return -1;
    }
    if (x->nrg == x->caprg) {
        size_t cap = x->caprg ? x->caprg * 2 : 256;
        mp_image_range *g = realloc(x->rg, cap * sizeof *g);
        if (!g) return oom(e);
        x->rg = g;
        x->caprg = cap;
    }
    x->rg[x->nrg].start = pos;
    x->rg[x->nrg].len = need * x->bs;
    x->nrg++;
    *off = pos;
    return 0;
}

// The content of the folder whose inode is di (malloc'd, *len bytes).
static int read_dir(pfs *x, const uint8_t *di, const char *path, uint8_t **out, uint64_t *len, mp_err *e) {
    *out = NULL;
    uint64_t size = rd64(di + DI_SIZE), off = 0;
    if ((rd16(di + DI_MODE) & (IF_DIR | IF_FILE)) != IF_DIR) {
        mp_err_set(e, "damaged PFS image: %s is not a folder", shown(path));
        return -1;
    }
    if (size > MAX_DIR || (rd32(di + DI_FLAGS) & FLAG_COMPRESSED)) {
        mp_err_set(e, "damaged PFS image: the folder %s has an invalid size", shown(path));
        return -1;
    }
    if (locate(x, di, size, path, &off, e) != 0) return -1;
    x->dir_bytes += size;
    if (x->dir_bytes > x->img->size) {   // folders that share blocks: a damaged image
        mp_err_set(e, "damaged PFS image: the folders take more room than the image has");
        return -1;
    }
    uint8_t *d = malloc(size ? (size_t)size : 1);
    size_t got = 0;
    if (!d) return oom(e);
    if (mp_image_pread(x->img, d, (size_t)size, off, &got, e) != 0 || got != size) {
        if (got != size) mp_err_set(e, "truncated PFS image: the folder %s is missing", shown(path));
        free(d);
        return -1;
    }
    *out = d;
    *len = size;
    return 0;
}

// One entry of a folder's content: 0 with the next one at *pos, 1 at the end of the folder.
typedef struct {
    uint32_t ino;
    int32_t type;
    const uint8_t *name;
    size_t namelen;
} dirent;

static int next_entry(pfs *x, const uint8_t *d, uint64_t len, uint64_t *pos, dirent *de, const char *path,
                      mp_err *e) {
    for (;;) {
        if (*pos + 16 > len) return 1;
        const uint8_t *p = d + *pos;
        uint32_t ino = rd32(p);
        int32_t type = (int32_t)rd32(p + 4), namelen = (int32_t)rd32(p + 8), entsize = (int32_t)rd32(p + 12);
        if (!ino && !type && !namelen && !entsize) {   // the rest of this block is free
            *pos = (*pos / x->bs + 1) * x->bs;
            continue;
        }
        if (entsize < 17 || entsize % 8 || namelen < 1 || namelen > entsize - 16 ||
            (uint64_t)entsize > len - *pos) {
            mp_err_set(e, "damaged PFS image: invalid entry in the folder %s", shown(path));
            return -1;
        }
        de->ino = ino;
        de->type = type;
        de->name = p + 16;
        de->namelen = (size_t)namelen;
        *pos += (uint64_t)entsize;
        return 0;
    }
}

static int seen(pfs *x, uint64_t ino) {
    int was = (x->seen[ino >> 3] >> (ino & 7)) & 1;
    x->seen[ino >> 3] |= (uint8_t)(1u << (ino & 7));
    return was;
}

static int push(pfs *x, rec v, mp_err *e) {
    if (x->n == x->cap) {
        size_t cap = x->cap ? x->cap * 2 : 256;
        rec *g = realloc(x->v, cap * sizeof *g);
        if (!g) return oom(e);
        x->v = g;
        x->cap = cap;
    }
    x->v[x->n++] = v;
    return 0;
}

static int walk(pfs *x, uint64_t dino, const char *prefix, int depth, mp_err *e);

static int child(pfs *x, const dirent *de, const char *path, int depth, mp_err *e) {
    if (++x->entries > MP_IMAGE_MAX_ENTRIES) {
        mp_err_set(e, "damaged PFS image: more than %u files and folders", MP_IMAGE_MAX_ENTRIES);
        return -1;
    }
    if (de->ino >= x->ninodes) {
        mp_err_set(e, "damaged PFS image: %s points to inode %" PRIu32 ", out of range", path, de->ino);
        return -1;
    }
    const uint8_t *di = inode(x, de->ino);
    unsigned fmt = rd16(di + DI_MODE) & (IF_DIR | IF_FILE);
    if (de->type == DT_DIR) {
        if (fmt != IF_DIR) {
            mp_err_set(e, "damaged PFS image: the entry of %s does not match its inode", path);
            return -1;
        }
        x->ndirs++;
        return walk(x, de->ino, path, depth + 1, e);
    }
    if (de->type != DT_FILE) {
        mp_err_set(e, "%s: an entry of type %" PRId32 " (neither a file nor a folder): not supported", path,
                   de->type);
        return -1;
    }
    if (fmt != IF_FILE) {
        mp_err_set(e, "damaged PFS image: the entry of %s does not match its inode", path);
        return -1;
    }
    if (seen(x, de->ino)) {
        mp_err_set(e, "%s is a hard link to another file: a game image has none", path);
        return -1;
    }
    int compressed = (rd32(di + DI_FLAGS) & FLAG_COMPRESSED) != 0;
    int64_t size = (int64_t)rd64(di + DI_SIZE), size2 = (int64_t)rd64(di + DI_SIZE2);
    // MkPFS's convention: a compressed file's size is what it takes stored, and the second size
    // its real length.
    int64_t stored = size, logical = compressed ? size2 : size;
    if (stored < 0 || logical < 0 || (uint64_t)stored > x->img->size ||
        (compressed && (uint64_t)logical > MP_PFSC_MAX_LOGICAL)) {
        mp_err_set(e, "damaged PFS image: %s has an invalid size", path);
        return -1;
    }
    rec v;
    memset(&v, 0, sizeof v);
    if (locate(x, di, (uint64_t)stored, path, &v.f.off, e) != 0) return -1;
    v.f.stored = (uint64_t)stored;
    v.f.compressed = compressed;
    if (!(v.en.path = mp_strdup(path))) return oom(e);
    v.en.size = (uint64_t)logical;
    v.en.mtime = (int64_t)rd64(di + DI_MTIME);
    if (push(x, v, e) != 0) {
        free(v.en.path);
        return -1;
    }
    return 0;
}

static int walk(pfs *x, uint64_t dino, const char *prefix, int depth, mp_err *e) {
    if (depth > MP_IMAGE_MAX_LEVELS) {
        mp_err_set(e, "too many folder levels in %s", shown(prefix));
        return -1;
    }
    if (seen(x, dino)) {
        mp_err_set(e, "damaged PFS image: the folder %s is reached twice (a loop)", shown(prefix));
        return -1;
    }
    uint8_t *d = NULL;
    uint64_t len = 0, pos = 0;
    if (read_dir(x, inode(x, dino), prefix, &d, &len, e) != 0) return -1;
    mp_image_name *names = NULL;
    size_t nn = 0, capn = 0;
    dirent de;
    int r;
    while ((r = next_entry(x, d, len, &pos, &de, prefix, e)) == 0) {
        int dot = de.namelen == 1 && de.name[0] == '.';
        int dotdot = de.namelen == 2 && de.name[0] == '.' && de.name[1] == '.';
        if (de.type == DT_DOT || de.type == DT_DOTDOT) {
            if ((de.type == DT_DOT && !dot) || (de.type == DT_DOTDOT && !dotdot)) {
                mp_err_set(e, "damaged PFS image: invalid entry in the folder %s", shown(prefix));
                r = -1;
                break;
            }
            continue;   // the folder itself and its parent
        }
        if (nn == capn) {
            size_t cap = capn ? capn * 2 : 64;
            mp_image_name *g = realloc(names, cap * sizeof *g);
            if (!g) {
                r = oom(e);
                break;
            }
            names = g;
            capn = cap;
        }
        names[nn].p = de.name;
        names[nn].len = de.namelen;
        nn++;
        char *path = mp_image_join(prefix, de.name, de.namelen, e);
        r = path ? child(x, &de, path, depth, e) : -1;
        free(path);
        if (r != 0) break;
    }
    if (r == 1) r = mp_image_unique(names, nn, shown(prefix), e);
    free(names);
    free(d);
    return r;
}

static int load(pfs *x, uint64_t *uroot, mp_err *e) {
    const char *name = x->img->name;
    uint8_t h[0x50];
    size_t got = 0;
    if (mp_image_pread(x->img, h, sizeof h, 0, &got, e) != 0) return -1;
    if (got != sizeof h) {
        mp_err_set(e, "%s: truncated PFS image (%" PRIu64 " bytes)", name, x->img->size);
        return -1;
    }
    uint64_t version = rd64(h + HDR_VERSION);
    unsigned mode = rd16(h + HDR_MODE);
    x->bs = rd32(h + HDR_BSIZE);
    x->ninodes = rd64(h + HDR_NINODES);
    x->ndblock = rd64(h + HDR_NDBLOCK);
    x->iblocks = rd64(h + HDR_INODE_BLOCKS);
    uint64_t sroot = rd64(h + HDR_SUPERROOT);
    if (mode & (MODE_SIGNED | MODE_ENCRYPTED)) {
        mp_err_set(e, "%s: a signed or encrypted PFS image (like the one inside a PKG); MaxPack reads "
                      "unsigned ones, as MkPFS makes them",
                   name);
        return -1;
    }
    const char *why = NULL;
    if (version != 1 && version != 2)
        why = "version";
    else if (x->bs < 4096 || x->bs > (1u << 20) || (x->bs & (x->bs - 1)))
        why = "block size";
    else if (x->ninodes == 0 || x->ninodes > MP_IMAGE_MAX_ENTRIES + 64 || sroot >= x->ninodes ||
             x->iblocks < (x->ninodes + x->bs / INODE - 1) / (x->bs / INODE) ||
             x->iblocks > (x->ninodes + x->bs / INODE - 1) / (x->bs / INODE) + 1)
        why = "inode table";
    else if (x->ndblock <= x->iblocks || x->ndblock > INT32_MAX)
        why = "number of blocks";
    if (why) {
        mp_err_set(e, "%s: invalid PFS header (%s)", name, why);
        return -1;
    }
    x->ipb = x->bs / INODE;
    if (x->img->size / x->bs < x->iblocks + 1) {
        mp_err_set(e, "%s: truncated PFS image (the inode table does not fit)", name);
        return -1;
    }
    size_t tlen = (size_t)(x->iblocks * x->bs);
    x->itab = malloc(tlen);
    x->seen = calloc((size_t)(x->ninodes / 8 + 1), 1);
    if (!x->itab || !x->seen) return oom(e);
    if (mp_image_pread(x->img, x->itab, tlen, x->bs, &got, e) != 0) return -1;
    if (got != tlen) {
        mp_err_set(e, "%s: truncated PFS image (the inode table does not fit)", name);
        return -1;
    }
    // The super-root lists flat_path_table (a hash index, not needed to list the files),
    // perhaps collision_resolver, and uroot.
    uint8_t *d = NULL;
    uint64_t len = 0, pos = 0;
    if (read_dir(x, inode(x, sroot), "(super-root)", &d, &len, e) != 0) return -1;
    dirent de;
    int r, found = 0;
    while (!found && (r = next_entry(x, d, len, &pos, &de, "(super-root)", e)) == 0)
        if (de.type == DT_DIR && de.namelen == 5 && !memcmp(de.name, "uroot", 5) && de.ino < x->ninodes) {
            *uroot = de.ino;
            found = 1;
        }
    free(d);
    if (!found) {
        if (r != -1) mp_err_set(e, "%s: damaged PFS image (no uroot folder)", name);
        return -1;
    }
    return 0;
}

static int pfs_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e) {
    pfs *x = s->impl;
    const pfs_file *f = &x->files[i];
    if (f->dec) return mp_pfsc_read(f->dec, buf, len, off, e);
    size_t got = 0;
    if (mp_image_pread(x->img, buf, len, f->off + off, &got, e) != 0) return -1;
    if (got != len) {
        mp_err_set(e, "truncated PFS image: the content of %s is missing", s->entries[i].path);
        return -1;
    }
    return 0;
}

static void pfs_free(pfs *x, size_t nfiles) {
    if (!x) return;
    if (x->files)
        for (size_t i = 0; i < nfiles; i++) mp_pfsc_free(x->files[i].dec);
    for (size_t i = 0; i < x->n; i++) {
        free(x->v[i].en.path);
        mp_pfsc_free(x->v[i].f.dec);
    }
    free(x->v);
    free(x->files);
    free(x->rg);
    free(x->itab);
    free(x->seen);
    mp_image_close(x->img);
    free(x);
}

static void pfs_close(mp_source *s) { pfs_free(s->impl, s->n); }

static int cmp_rec(const void *a, const void *b) {
    return strcmp(((const rec *)a)->en.path, ((const rec *)b)->en.path);
}

// The one file of the image, as an image that borrows x (for probing it).
typedef struct {
    pfs *x;
    rec *r;
} borrowed;

static int borrowed_read(mp_image *m, void *buf, size_t len, uint64_t off, mp_err *e) {
    borrowed *b = m->impl;
    if (b->r->f.dec) return mp_pfsc_read(b->r->f.dec, buf, len, off, e);
    size_t got = 0;
    if (mp_image_pread(b->x->img, buf, len, b->r->f.off + off, &got, e) != 0) return -1;
    if (got != len) {
        mp_err_set(e, "truncated PFS image: the content of %s is missing", b->r->en.path);
        return -1;
    }
    return 0;
}

// If the only file is an image, opens it through this one: 1 with *out set, or -2 with e set (x is
// freed in both cases); 0 if it is not an image, -1 with e set if it could not tell (x is kept).
static int open_inner(pfs *x, int depth, mp_source **out, mp_err *e) {
    rec *r = &x->v[0];
    char *name = malloc(strlen(x->img->name) + strlen(r->en.path) + 2);
    if (!name) return oom(e);
    sprintf(name, "%s/%s", x->img->name, r->en.path);
    if (r->f.compressed && mp_pfsc_open(x->img, r->f.off, r->f.stored, r->en.size, name, 64, &r->f.dec, e) != 0) {
        free(name);
        return -1;
    }
    borrowed b = {x, r};
    mp_image probe;
    memset(&probe, 0, sizeof probe);
    probe.size = r->en.size;
    probe.name = name;
    probe.read = borrowed_read;
    probe.impl = &b;
    mp_image_type t = MP_IMG_UNKNOWN;
    int rc = mp_image_probe(&probe, &t, e);
    if (rc != 0 || (t != MP_IMG_EXFAT && t != MP_IMG_UFS2 && t != MP_IMG_PFS)) {
        free(name);
        return rc != 0 ? -1 : 0;
    }
    mp_image *img = x->img, *view = NULL;
    mp_pfsc *dec = r->f.dec;
    uint64_t off = r->f.off, stored = r->f.stored;
    x->img = NULL;
    r->f.dec = NULL;
    pfs_free(x, 0);
    rc = dec ? mp_image_pfsc_from(img, dec, name, &view, e) : mp_image_slice(img, off, stored, name, &view, e);
    free(name);
    if (rc != 0) return -2;
    mp_source *s = NULL;
    if (mp_source_open_image(view, depth + 1, NULL, &s, e) != 0) return -2;
    if (!s->container) s->inner = s->kind;
    s->kind = MP_SRC_PFS;
    s->container = 1;
    *out = s;
    return 1;
}

int mp_pfs_open_image(mp_image *img, int depth, mp_source **out, mp_err *e) {
    *out = NULL;
    pfs *x = calloc(1, sizeof *x);
    if (!x) {
        mp_image_close(img);
        return oom(e);
    }
    x->img = img;
    uint64_t uroot = 0;
    int rc = load(x, &uroot, e);
    if (rc == 0) rc = walk(x, uroot, "", 0, e);
    if (rc == 0) rc = mp_image_disjoint(x->rg, x->nrg, "PFS", e);
    if (rc == 0 && x->n == 1) {
        rc = open_inner(x, depth, out, e);
        if (rc == 1) return 0;   // x is freed
        if (rc == -2) return -1;
    }
    mp_source *s = NULL;
    mp_entry *entries = NULL;
    if (rc == 0) {
        s = calloc(1, sizeof *s);
        x->files = malloc((x->n ? x->n : 1) * sizeof *x->files);
        entries = malloc((x->n ? x->n : 1) * sizeof *entries);
        if (!s || !x->files || !entries) rc = oom(e);
    }
    int compressed = img->compressed;
    for (size_t i = 0; rc == 0 && i < x->n; i++) {
        rec *r = &x->v[i];
        if (r->f.compressed && !r->f.dec)
            rc = mp_pfsc_open(x->img, r->f.off, r->f.stored, r->en.size, r->en.path, 0, &r->f.dec, e);
        compressed |= r->f.compressed;
    }
    if (rc != 0) {
        free(entries);
        free(s);
        pfs_free(x, 0);
        return -1;
    }
    if (x->n > 1) qsort(x->v, x->n, sizeof *x->v, cmp_rec);
    for (size_t i = 0; i < x->n; i++) {
        entries[i] = x->v[i].en;
        x->files[i] = x->v[i].f;
    }
    s->kind = s->inner = MP_SRC_PFS;
    s->compressed = compressed;
    s->entries = entries;
    s->n = x->n;
    s->ndirs = x->ndirs;
    s->read = pfs_read;
    s->close = pfs_close;
    s->impl = x;
    free(x->v);
    x->v = NULL;
    x->n = 0;
    free(x->rg);
    x->rg = NULL;
    free(x->itab);
    x->itab = NULL;
    free(x->seen);
    x->seen = NULL;
    *out = s;
    return 0;
}
