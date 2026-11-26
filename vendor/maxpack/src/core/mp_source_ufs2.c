// SPDX-License-Identifier: GPL-3.0-or-later
// UFS2 image reader (.ffpkg, read-only): FreeBSD's fast file system as `newfs -O 2` or `makefs -t
// ffs -o version=2` write it (ShadowMountPlus's profile: -b 65536 -f 65536 -m 0 -S 4096). The
// on-disk layout follows FreeBSD's sys/ufs/ffs/fs.h, sys/ufs/ufs/dinode.h and sys/ufs/ufs/dir.h
// (BSD licence; only the layout is used, no code). Loads every folder and every file's block map
// on open and checks them; files are read afterwards by contiguous runs.
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "mp_extent.h"
#include "mp_platform.h"
#include "mp_source_impl.h"

#define SBLOCKSIZE 8192
#define FS_UFS2_MAGIC 0x19540119u
#define ROOTINO 2u
#define NDADDR 12
#define NIADDR 3
#define DINODE 256u
#define DIRBLKSIZ 512u
#define MAX_DIR (256ull << 20)
#define HOLE UINT64_MAX   // img_off of a run that is a hole (reads as zeros)

// Superblock (struct fs) fields, by byte offset.
#define SB_IBLKNO 16
#define SB_NCG 44
#define SB_BSIZE 48
#define SB_FSIZE 52
#define SB_FRAG 56
#define SB_NINDIR 116
#define SB_INOPB 120
#define SB_IPG 184
#define SB_FPG 188
#define SB_SBLOCKLOC 1000
#define SB_SIZE 1080
#define SB_MAGIC 1372

// Inode (struct ufs2_dinode) fields.
#define DI_MODE 0
#define DI_SIZE 16
#define DI_MTIME 40
#define DI_DB 112
#define DI_IB 208

#define IFMT 0170000
#define IFDIR 0040000
#define IFREG 0100000
#define IFLNK 0120000
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8

typedef struct {
    mp_extent *ex;
    size_t nex;
} ufs_file;

typedef struct {
    mp_entry en;
    ufs_file f;
} rec;

typedef struct {
    mp_image *img;
    uint32_t bsize, fsize, frag, ncg, ipg, fpg, iblkno, nindir, inopb;
    uint64_t fs_size, vol, ninodes, holes, entries;
    uint64_t used;          // bytes of blocks mapped so far (data and indirect blocks)
    uint8_t *iblk;          // the last block of inodes read
    uint64_t iblk_at;       // its position in the image + 1 (0 = none)
    uint8_t *ind[NIADDR];   // one indirect block per level
    rec *v;
    size_t n, cap, ndirs;
    mp_image_range *rg;     // what the files and folders use, to find cross-links
    size_t nrg, caprg;
    uint32_t *seen;         // inode numbers met (hash set, 0 = empty slot)
    size_t nseen, capseen;
    ufs_file *files;        // parallel to mp_source.entries
} ufs;

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

// Exactly len bytes at off, or a "truncated" error naming what was being read.
static int get(ufs *u, void *buf, size_t len, uint64_t off, const char *what, mp_err *e) {
    size_t got = 0;
    if (mp_image_pread(u->img, buf, len, off, &got, e) != 0) return -1;
    if (got != len) {
        mp_err_set(e, "truncated UFS2 image: %s is missing", what);
        return -1;
    }
    return 0;
}

// Marks inode ino as met; -1 if it already was (a folder reached twice, or a hard link).
static int see(ufs *u, uint32_t ino, mp_err *e) {
    if (u->nseen * 2 >= u->capseen) {
        size_t cap = u->capseen ? u->capseen * 2 : 1024;
        uint32_t *t = calloc(cap, sizeof *t);
        if (!t) return oom(e);
        for (size_t i = 0; i < u->capseen; i++) {
            if (!u->seen[i]) continue;
            size_t h = (u->seen[i] * 2654435761u) & (cap - 1);
            while (t[h]) h = (h + 1) & (cap - 1);
            t[h] = u->seen[i];
        }
        free(u->seen);
        u->seen = t;
        u->capseen = cap;
    }
    size_t h = (ino * 2654435761u) & (u->capseen - 1);
    while (u->seen[h]) {
        if (u->seen[h] == ino) return 1;
        h = (h + 1) & (u->capseen - 1);
    }
    u->seen[h] = ino;
    u->nseen++;
    return 0;
}

static int add_range(ufs *u, uint64_t start, uint64_t len, mp_err *e) {
    if (u->nrg && u->rg[u->nrg - 1].start + u->rg[u->nrg - 1].len == start) {
        u->rg[u->nrg - 1].len += len;
        return 0;
    }
    if (u->nrg == u->caprg) {
        size_t cap = u->caprg ? u->caprg * 2 : 256;
        mp_image_range *g = realloc(u->rg, cap * sizeof *g);
        if (!g) return oom(e);
        u->rg = g;
        u->caprg = cap;
    }
    u->rg[u->nrg].start = start;
    u->rg[u->nrg].len = len;
    u->nrg++;
    return 0;
}

// The inode ino (DINODE bytes, valid until the next call).
static const uint8_t *inode(ufs *u, uint32_t ino, const char *path, mp_err *e) {
    if (ino < ROOTINO || ino >= u->ninodes) {
        mp_err_set(e, "damaged UFS2 image: %s points to inode %" PRIu32 ", out of range", shown(path), ino);
        return NULL;
    }
    uint64_t cg = ino / u->ipg, idx = ino % u->ipg;
    uint64_t blk = ((cg * u->fpg + u->iblkno) * u->fsize) + idx / u->inopb * u->bsize;
    if (u->iblk_at != blk + 1) {
        u->iblk_at = 0;
        if (get(u, u->iblk, u->bsize, blk, "a block of inodes", e) != 0) return NULL;
        u->iblk_at = blk + 1;
    }
    return u->iblk + (idx % u->inopb) * DINODE;
}

typedef struct {
    const char *path;
    uint64_t size, nblocks;
    mp_extent *ex;
    size_t nex, cap;
} map;

static int put_run(map *m, uint64_t file_off, uint64_t img_off, uint64_t len, mp_err *e) {
    if (m->nex) {
        mp_extent *l = &m->ex[m->nex - 1];
        int both_holes = l->img_off == HOLE && img_off == HOLE;
        if (both_holes || (l->img_off != HOLE && img_off != HOLE && l->img_off + l->len == img_off)) {
            l->len += len;
            return 0;
        }
    }
    if (m->nex == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 4;
        mp_extent *g = realloc(m->ex, cap * sizeof *g);
        if (!g) return oom(e);
        m->ex = g;
        m->cap = cap;
    }
    m->ex[m->nex].file_off = file_off;
    m->ex[m->nex].img_off = img_off;
    m->ex[m->nex].len = len;
    m->nex++;
    return 0;
}

// Counts n bytes of blocks: no more than the image holds, or blocks are used twice (and a damaged
// image would make the walk read the same blocks over and over).
static int charge(ufs *u, uint64_t n, const char *path, mp_err *e) {
    u->used += n;
    if (u->used > u->img->size) {
        mp_err_set(e, "damaged UFS2 image: %s and the files before it use more blocks than the image has",
                   shown(path));
        return -1;
    }
    return 0;
}

static int bad_block(const char *path, mp_err *e) {
    mp_err_set(e, "damaged UFS2 image: a block of %s is out of range", shown(path));
    return -1;
}

// Checks the fragment address addr for n bytes inside one block and returns its byte position.
static int frags_at(ufs *u, uint64_t addr, uint64_t n, const char *path, uint64_t *pos, mp_err *e) {
    uint64_t nfr = (n + u->fsize - 1) / u->fsize;
    if (addr >= u->fs_size || nfr > u->fs_size - addr || (addr % u->frag) + nfr > u->frag)
        return bad_block(path, e);
    *pos = addr * u->fsize;
    return 0;
}

// Blocks [lbn, ...) of the file under pointer addr: level 0 is a data block, 1 a single indirect
// block (NINDIR data blocks), 2 double, 3 triple. A zero pointer is a hole over all of them.
static int map_ptr(ufs *u, map *m, uint64_t addr, int level, uint64_t lbn, mp_err *e) {
    if (lbn >= m->nblocks) return 0;
    uint64_t span = 1;
    for (int k = 0; k < level; k++) span *= u->nindir;
    uint64_t last = span < m->nblocks - lbn ? lbn + span : m->nblocks;   // blocks [lbn, last)
    uint64_t from = lbn * u->bsize, to = last * u->bsize < m->size ? last * u->bsize : m->size;
    if (addr == 0) {
        u->holes += to - from;
        if (u->holes > u->vol) {
            mp_err_set(e, "damaged UFS2 image: %s has more holes than the volume has room", shown(m->path));
            return -1;
        }
        return put_run(m, from, HOLE, to - from, e);
    }
    uint64_t pos = 0;
    if (level == 0) {
        if (frags_at(u, addr, to - from, m->path, &pos, e) != 0) return -1;
        if (pos > u->img->size || to - from > u->img->size - pos) {
            mp_err_set(e, "truncated UFS2 image: the content of %s is missing", shown(m->path));
            return -1;
        }
        uint64_t held = (to - from + u->fsize - 1) / u->fsize * u->fsize;
        if (charge(u, held, m->path, e) != 0 || add_range(u, pos, held, e) != 0) return -1;
        return put_run(m, from, pos, to - from, e);
    }
    if (frags_at(u, addr, u->bsize, m->path, &pos, e) != 0 || charge(u, u->bsize, m->path, e) != 0) return -1;
    uint8_t *b = u->ind[level - 1];
    if (get(u, b, u->bsize, pos, "an indirect block", e) != 0 || add_range(u, pos, u->bsize, e) != 0) return -1;
    uint64_t sub = span / u->nindir;
    for (uint32_t i = 0; i < u->nindir && lbn + i * sub < m->nblocks; i++)
        if (map_ptr(u, m, rd64(b + 8ull * i), level - 1, lbn + i * sub, e) != 0) return -1;
    return 0;
}

// The runs of the file or folder whose inode is di.
static int map_inode(ufs *u, const uint8_t *di, uint64_t size, const char *path, mp_extent **ex,
                     size_t *nex, mp_err *e) {
    *ex = NULL;
    *nex = 0;
    map m = {path, size, (size + u->bsize - 1) / u->bsize, NULL, 0, 0};
    uint64_t addr[NDADDR + NIADDR];
    for (int k = 0; k < NDADDR + NIADDR; k++) addr[k] = rd64(di + DI_DB + 8 * k);   // db then ib
    int rc = 0;
    for (uint64_t k = 0; rc == 0 && k < NDADDR; k++) rc = map_ptr(u, &m, addr[k], 0, k, e);
    uint64_t lbn = NDADDR, span = u->nindir;
    for (int l = 1; rc == 0 && l <= NIADDR && lbn < m.nblocks; l++) {
        rc = map_ptr(u, &m, addr[NDADDR + l - 1], l, lbn, e);
        lbn += span;
        span *= u->nindir;
    }
    if (rc == 0 && lbn < m.nblocks) {
        mp_err_set(e, "damaged UFS2 image: %s is larger than an inode can address", shown(path));
        rc = -1;
    }
    if (rc != 0) {
        free(m.ex);
        return -1;
    }
    *ex = m.ex;
    *nex = m.nex;
    return 0;
}

static int read_runs(ufs *u, const mp_extent *ex, size_t nex, uint64_t off, uint8_t *p, size_t len,
                     const char *path, mp_err *e) {
    while (len) {
        uint64_t img = 0, run = 0;
        long k = mp_extent_find(ex, nex, off, &img, &run);
        if (k < 0) {
            mp_err_set(e, "%s: offset without a block", path);
            return -1;
        }
        size_t take = len < run ? len : (size_t)run, got = 0;
        if (ex[k].img_off == HOLE) {
            memset(p, 0, take);
        } else {
            if (mp_image_pread(u->img, p, take, img, &got, e) != 0) return -1;
            if (got != take) {
                mp_err_set(e, "truncated UFS2 image: the content of %s is missing", path);
                return -1;
            }
        }
        p += take;
        off += take;
        len -= take;
    }
    return 0;
}

static int push(ufs *u, rec v, mp_err *e) {
    if (u->n == u->cap) {
        size_t cap = u->cap ? u->cap * 2 : 256;
        rec *g = realloc(u->v, cap * sizeof *g);
        if (!g) return oom(e);
        u->v = g;
        u->cap = cap;
    }
    u->v[u->n++] = v;
    return 0;
}

static int parse_dir(ufs *u, uint32_t dino, const char *prefix, int depth, mp_err *e);

// One entry of a folder: a file is listed, a folder is walked.
static int child(ufs *u, uint32_t ino, unsigned type, const char *path, int depth, mp_err *e) {
    if (++u->entries > MP_IMAGE_MAX_ENTRIES) {
        mp_err_set(e, "damaged UFS2 image: more than %u files and folders", MP_IMAGE_MAX_ENTRIES);
        return -1;
    }
    const uint8_t *di = inode(u, ino, path, e);
    if (!di) return -1;
    unsigned fmt = rd16(di + DI_MODE) & IFMT;
    if ((fmt == IFDIR && type != DT_UNKNOWN && type != DT_DIR) ||
        (fmt == IFREG && type != DT_UNKNOWN && type != DT_REG)) {
        mp_err_set(e, "damaged UFS2 image: the entry of %s does not match its inode", path);
        return -1;
    }
    if (fmt == IFDIR) {
        u->ndirs++;
        return parse_dir(u, ino, path, depth + 1, e);
    }
    if (fmt == IFLNK) {
        mp_err_set(e, "%s is a symbolic link: a game image has none, so MaxPack does not take it", path);
        return -1;
    }
    if (fmt != IFREG) {
        mp_err_set(e, "%s is a device, a pipe or a socket: a game image has none, so MaxPack does not take it",
                   path);
        return -1;
    }
    int r = see(u, ino, e);
    if (r != 0) {
        if (r > 0) mp_err_set(e, "%s is a hard link to another file: a game image has none", path);
        return -1;
    }
    uint64_t size = rd64(di + DI_SIZE);
    int64_t mtime = (int64_t)rd64(di + DI_MTIME);
    if (size > u->vol) {
        mp_err_set(e, "damaged UFS2 image: %s is larger than the volume", path);
        return -1;
    }
    rec v;
    memset(&v, 0, sizeof v);
    if (map_inode(u, di, size, path, &v.f.ex, &v.f.nex, e) != 0) return -1;
    if (!(v.en.path = mp_strdup(path))) {
        free(v.f.ex);
        return oom(e);
    }
    v.en.size = size;
    v.en.mtime = mtime;
    if (push(u, v, e) != 0) {
        free(v.en.path);
        free(v.f.ex);
        return -1;
    }
    return 0;
}

static int bad_dir(const char *prefix, mp_err *e) {
    mp_err_set(e, "damaged UFS2 image: invalid entry in the folder %s", shown(prefix));
    return -1;
}

static int parse_dir(ufs *u, uint32_t dino, const char *prefix, int depth, mp_err *e) {
    if (depth > MP_IMAGE_MAX_LEVELS) {
        mp_err_set(e, "too many folder levels in %s", shown(prefix));
        return -1;
    }
    int r = see(u, dino, e);
    if (r != 0) {
        if (r > 0) mp_err_set(e, "damaged UFS2 image: the folder %s is reached twice (a loop)", shown(prefix));
        return -1;
    }
    const uint8_t *di = inode(u, dino, prefix, e);
    if (!di) return -1;
    if ((rd16(di + DI_MODE) & IFMT) != IFDIR) {
        mp_err_set(e, "damaged UFS2 image: %s is not a folder", shown(prefix));
        return -1;
    }
    uint64_t size = rd64(di + DI_SIZE);
    if (size > MAX_DIR || size % DIRBLKSIZ) {
        mp_err_set(e, "damaged UFS2 image: the folder %s has an invalid size", shown(prefix));
        return -1;
    }
    mp_extent *ex = NULL;
    size_t nex = 0;
    if (map_inode(u, di, size, prefix, &ex, &nex, e) != 0) return -1;
    uint8_t *d = malloc(size ? (size_t)size : 1);
    mp_image_name *names = NULL;
    size_t nn = 0, capn = 0;
    int rc = d ? read_runs(u, ex, nex, 0, d, (size_t)size, shown(prefix), e) : oom(e);
    free(ex);
    for (uint64_t chunk = 0; rc == 0 && chunk < size; chunk += DIRBLKSIZ) {
        uint64_t pos = chunk, end = chunk + DIRBLKSIZ;
        while (rc == 0 && pos < end) {
            if (end - pos < 8) {
                rc = bad_dir(prefix, e);
                break;
            }
            uint32_t ino = rd32(d + pos);
            unsigned reclen = rd16(d + pos + 4), type = d[pos + 6], namlen = d[pos + 7];
            if (reclen < 8 || reclen % 4 || reclen > end - pos || (ino && 8u + namlen > reclen)) {
                rc = bad_dir(prefix, e);
                break;
            }
            const uint8_t *name = d + pos + 8;
            pos += reclen;
            if (!ino || (namlen == 1 && name[0] == '.') || (namlen == 2 && name[0] == '.' && name[1] == '.'))
                continue;   // a free entry, or the folder itself or its parent
            if (nn == capn) {
                size_t cap = capn ? capn * 2 : 64;
                mp_image_name *g = realloc(names, cap * sizeof *g);
                if (!g) {
                    rc = oom(e);
                    break;
                }
                names = g;
                capn = cap;
            }
            names[nn].p = name;
            names[nn].len = namlen;
            nn++;
            char *path = mp_image_join(prefix, name, namlen, e);
            rc = path ? child(u, ino, type, path, depth, e) : -1;
            free(path);
        }
    }
    if (rc == 0) rc = mp_image_unique(names, nn, shown(prefix), e);
    free(names);
    free(d);
    return rc;
}

static int ufs_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e) {
    ufs *u = s->impl;
    return read_runs(u, u->files[i].ex, u->files[i].nex, off, buf, len, s->entries[i].path, e);
}

static void ufs_free(ufs *u, size_t nfiles) {
    if (!u) return;
    if (u->files)
        for (size_t i = 0; i < nfiles; i++) free(u->files[i].ex);
    for (size_t i = 0; i < u->n; i++) {
        free(u->v[i].en.path);
        free(u->v[i].f.ex);
    }
    free(u->v);
    free(u->files);
    free(u->rg);
    free(u->seen);
    free(u->iblk);
    for (int k = 0; k < NIADDR; k++) free(u->ind[k]);
    mp_image_close(u->img);
    free(u);
}

static void ufs_close(mp_source *s) { ufs_free(s->impl, s->n); }

static int cmp_rec(const void *a, const void *b) {
    return strcmp(((const rec *)a)->en.path, ((const rec *)b)->en.path);
}

static int pow2(uint64_t v) { return v && !(v & (v - 1)); }

static int load_super(ufs *u, mp_err *e) {
    static const uint64_t at[] = {65536, 8192, 0, 262144};
    uint8_t sb[SBLOCKSIZE];
    int found = 0;
    for (size_t k = 0; k < sizeof at / sizeof at[0] && !found; k++) {
        size_t got = 0;
        if (mp_image_pread(u->img, sb, sizeof sb, at[k], &got, e) != 0) return -1;
        found = got >= SB_MAGIC + 4 && rd32(sb + SB_MAGIC) == FS_UFS2_MAGIC && rd64(sb + SB_SBLOCKLOC) == at[k];
    }
    if (!found) {
        mp_err_set(e, "%s: no valid UFS2 superblock", u->img->name);
        return -1;
    }
    u->bsize = rd32(sb + SB_BSIZE);
    u->fsize = rd32(sb + SB_FSIZE);
    u->frag = rd32(sb + SB_FRAG);
    u->ncg = rd32(sb + SB_NCG);
    u->ipg = rd32(sb + SB_IPG);
    u->fpg = rd32(sb + SB_FPG);
    u->iblkno = rd32(sb + SB_IBLKNO);
    u->nindir = rd32(sb + SB_NINDIR);
    u->inopb = rd32(sb + SB_INOPB);
    u->fs_size = rd64(sb + SB_SIZE);
    const char *why = NULL;
    if (!pow2(u->bsize) || u->bsize < 4096 || u->bsize > 65536)
        why = "block size";
    else if (!pow2(u->fsize) || u->fsize < 512 || u->fsize > u->bsize || u->bsize / u->fsize > 8 ||
             u->frag != u->bsize / u->fsize)
        why = "fragment size";
    else if (u->nindir != u->bsize / 8 || u->inopb != u->bsize / DINODE)
        why = "block pointers or inodes per block";
    else if (u->ncg == 0 || u->ipg == 0 || u->ipg % u->inopb || u->fpg == 0 || u->fpg % u->frag ||
             u->iblkno % u->frag || u->iblkno >= u->fpg ||
             (uint64_t)u->ipg * DINODE / u->fsize > u->fpg - u->iblkno)
        why = "cylinder groups";
    else if ((uint64_t)u->ncg * u->ipg > UINT32_MAX)
        why = "number of inodes";
    else if (u->fs_size == 0 || u->fs_size > (uint64_t)u->ncg * u->fpg ||
             u->fs_size <= (uint64_t)(u->ncg - 1) * u->fpg || u->fs_size > (1ull << 50) / u->fsize)
        why = "size";
    if (why) {
        mp_err_set(e, "%s: invalid UFS2 superblock (%s)", u->img->name, why);
        return -1;
    }
    u->ninodes = (uint64_t)u->ncg * u->ipg;
    u->vol = u->fs_size * u->fsize;
    u->iblk = malloc(u->bsize);
    for (int k = 0; k < NIADDR; k++) u->ind[k] = malloc(u->bsize);
    if (!u->iblk || !u->ind[0] || !u->ind[1] || !u->ind[2]) return oom(e);
    return 0;
}

int mp_ufs2_open_image(mp_image *img, mp_source **out, mp_err *e) {
    *out = NULL;
    ufs *u = calloc(1, sizeof *u);
    if (!u) {
        mp_image_close(img);
        return oom(e);
    }
    u->img = img;
    int rc = load_super(u, e);
    if (rc == 0) rc = parse_dir(u, ROOTINO, "", 0, e);
    if (rc == 0) rc = mp_image_disjoint(u->rg, u->nrg, "UFS2", e);
    mp_source *s = NULL;
    mp_entry *entries = NULL;
    if (rc == 0) {
        s = calloc(1, sizeof *s);
        u->files = malloc((u->n ? u->n : 1) * sizeof *u->files);
        entries = malloc((u->n ? u->n : 1) * sizeof *entries);
        if (!s || !u->files || !entries) rc = oom(e);
    }
    if (rc != 0) {
        free(entries);
        free(s);
        ufs_free(u, 0);
        return -1;
    }
    if (u->n > 1) qsort(u->v, u->n, sizeof *u->v, cmp_rec);
    for (size_t i = 0; i < u->n; i++) {
        entries[i] = u->v[i].en;
        u->files[i] = u->v[i].f;
    }
    s->kind = s->inner = MP_SRC_UFS2;
    s->compressed = img->compressed;
    s->entries = entries;
    s->n = u->n;
    s->ndirs = u->ndirs;
    s->read = ufs_read;
    s->close = ufs_close;
    s->impl = u;
    free(u->v);
    u->v = NULL;
    u->n = 0;
    free(u->rg);
    u->rg = NULL;
    free(u->seen);
    u->seen = NULL;
    *out = s;
    return 0;
}
