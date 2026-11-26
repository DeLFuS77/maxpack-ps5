// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 debug package (FPKG, .pkg) as a source: the files of its inner PFS's uroot folder, plus the
// sce_sys files the package keeps in its CNT (param.json, the images, ...) where a dump has them.
// See mp_fpkg.h; the inner PFS is read as pearlxcore/ProsperoPkgTool's
// Containers/ProsperoInnerPfsReader.cs (MIT) does, with fthdgn-gh/Alembic's src/core/fpkg_inner.cpp
// (GPL-3.0-or-later) as the second reading: 64 KiB blocks, the superblock where the FIH says
// (version 2, the PFS signature, the block size at 0x20, the inode count at 0x30), the inode table in
// the next block (390 inodes of 0xA8 bytes per block: mode at 0, size at 8, the times at 0x18, the
// data's offset in the mount at 0x60), inode 0 the super-root whose "uroot" folder is the game.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_fpkg.h"
#include "mp_platform.h"
#include "mp_source_impl.h"

#define IBS 0x10000u
#define INODE 0xA8u
#define IPB (IBS / INODE)
#define MAX_DIR (256ull << 20)
#define PFS_MAGIC 20130315ull
#define DT_FILE 2
#define DT_DIR 3
#define DT_DOT 4
#define DT_DOTDOT 5

typedef struct {
    int where;      // 0 the mount, 1 the package file, 2 the bytes in data
    uint64_t off;
    uint8_t *data;
} ffile;

typedef struct {
    mp_entry en;
    ffile f;
} frec;

typedef struct {
    uint16_t mode;
    uint64_t size, off;
    int64_t mtime;
} inode_t;

typedef struct {
    mp_fpkg *pkg;
    mp_image *mount;
    ffile *files;   // parallel to the source's entries
    // while listing
    inode_t *ino;
    uint64_t ninodes;
    uint8_t *seen;
    frec *v;
    size_t n, cap, ndirs, entries;
    char **dirs;    // the folders' paths
    size_t nd, capd;
} fpkg_src;

static uint64_t le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int oom(mp_err *e) {
    mp_err_set(e, "out of memory");
    return -1;
}

static const char *shown(const char *path) { return path[0] ? path : "/"; }

static int mread(mp_image *m, void *buf, size_t len, uint64_t off, mp_err *e) {
    size_t got = 0;
    if (mp_image_pread(m, buf, len, off, &got, e) != 0) return -1;
    if (got != len) {
        mp_err_set(e, "damaged package: its PFS goes past the end of its mount");
        return -1;
    }
    return 0;
}

// The inner PFS's superblock: where the FIH says, else the first 64 KiB block that is one.
static int find_super(mp_image *mount, uint64_t hint_blocks, uint64_t *super, mp_err *e) {
    uint8_t h[0x28];
    uint64_t hint = hint_blocks * IBS;
    if (hint_blocks && hint_blocks < mount->size / IBS && hint + IBS <= mount->size) {
        if (mread(mount, h, sizeof h, hint, e) != 0) return -1;
        if (le64(h) == 2 && le64(h + 8) == PFS_MAGIC && le32(h + 0x20) == IBS) {
            *super = hint;
            return 0;
        }
    }
    for (uint64_t off = 0; off + IBS <= mount->size; off += IBS) {
        if (mread(mount, h, sizeof h, off, e) != 0) return -1;
        if (le64(h) == 2 && le64(h + 8) == PFS_MAGIC && le32(h + 0x20) == IBS) {
            *super = off;
            return 0;
        }
    }
    mp_err_set(e, "damaged package: its mount has no PFS superblock");
    return -1;
}

// The whole inode table, compact.
static int load_inodes(fpkg_src *x, uint64_t super, mp_err *e) {
    uint8_t h[0x40];
    if (mread(x->mount, h, sizeof h, super, e) != 0) return -1;
    uint64_t n = le64(h + 0x30);
    uint64_t blocks = (n + IPB - 1) / IPB, table = super + IBS;
    if (n < 1 || n > MP_IMAGE_MAX_ENTRIES + 64 || table > x->mount->size || blocks > (x->mount->size - table) / IBS) {
        mp_err_set(e, "damaged package: the inode table of its PFS is out of range");
        return -1;
    }
    x->ino = malloc((size_t)n * sizeof *x->ino);
    x->seen = calloc((size_t)(n / 8 + 1), 1);
    uint8_t *blk = malloc(IBS);
    if (!x->ino || !x->seen || !blk) {
        free(blk);
        return oom(e);
    }
    x->ninodes = n;
    for (uint64_t b = 0, i = 0; b < blocks; b++) {
        if (mread(x->mount, blk, IBS, table + b * IBS, e) != 0) {
            free(blk);
            return -1;
        }
        for (unsigned k = 0; k < IPB && i < n; k++, i++) {
            const uint8_t *d = blk + k * INODE;
            x->ino[i].mode = (uint16_t)(d[0] | d[1] << 8);
            x->ino[i].size = le64(d + 8);
            x->ino[i].mtime = (int64_t)le64(d + 0x20);
            x->ino[i].off = le64(d + 0x60);
        }
    }
    free(blk);
    return 0;
}

static int is_dir(const inode_t *n) { return (n->mode & 0xF000) == 0x4000; }
static int is_file(const inode_t *n) { return (n->mode & 0xF000) == 0x8000; }

// A folder's content (malloc'd).
static int read_dir(fpkg_src *x, uint64_t ino, const char *path, uint8_t **out, uint64_t *len, mp_err *e) {
    *out = NULL;
    const inode_t *d = &x->ino[ino];
    if (!is_dir(d)) {
        mp_err_set(e, "damaged package: %s is not a folder", shown(path));
        return -1;
    }
    if (d->size > MAX_DIR || d->off > x->mount->size || d->size > x->mount->size - d->off) {
        mp_err_set(e, "damaged package: the folder %s is outside its mount", shown(path));
        return -1;
    }
    uint8_t *b = malloc(d->size ? (size_t)d->size : 1);
    if (!b) return oom(e);
    if (mread(x->mount, b, (size_t)d->size, d->off, e) != 0) {
        free(b);
        return -1;
    }
    *out = b;
    *len = d->size;
    return 0;
}

typedef struct {
    uint32_t ino, type;
    const uint8_t *name;
    size_t namelen;
} dirent;

// The next entry of a folder: 0 with it, 1 at the end, -1 if damaged. An empty header ends the 64 KiB
// block it is in (the rest of it is free).
static int next_entry(const uint8_t *d, uint64_t len, uint64_t *pos, dirent *de, const char *path, mp_err *e) {
    for (;;) {
        if (*pos + 16 > len) return 1;
        const uint8_t *p = d + *pos;
        uint32_t ino = le32(p), type = le32(p + 4), namelen = le32(p + 8), reclen = le32(p + 12);
        if (reclen == 0) {
            if (ino || type || namelen) break;
            *pos = (*pos / IBS + 1) * IBS;
            continue;
        }
        if (reclen < 17 || reclen > len - *pos || namelen < 1 || namelen > reclen - 16) break;
        de->ino = ino;
        de->type = type;
        de->name = p + 16;
        de->namelen = namelen;
        *pos += reclen;
        return 0;
    }
    mp_err_set(e, "damaged package: invalid entry in the folder %s", shown(path));
    return -1;
}

static int seen(fpkg_src *x, uint64_t ino) {
    int was = (x->seen[ino >> 3] >> (ino & 7)) & 1;
    x->seen[ino >> 3] |= (uint8_t)(1u << (ino & 7));
    return was;
}

static int push(fpkg_src *x, frec r, mp_err *e) {
    if (x->n == x->cap) {
        size_t cap = x->cap ? x->cap * 2 : 256;
        frec *g = realloc(x->v, cap * sizeof *g);
        if (!g) return oom(e);
        x->v = g;
        x->cap = cap;
    }
    x->v[x->n++] = r;
    return 0;
}

static int push_dir(fpkg_src *x, const char *path, mp_err *e) {
    if (x->nd == x->capd) {
        size_t cap = x->capd ? x->capd * 2 : 64;
        char **g = realloc(x->dirs, cap * sizeof *g);
        if (!g) return oom(e);
        x->dirs = g;
        x->capd = cap;
    }
    if (!(x->dirs[x->nd] = mp_strdup(path))) return oom(e);
    x->nd++;
    x->ndirs++;
    return 0;
}

static int walk(fpkg_src *x, uint64_t dino, const char *prefix, int depth, mp_err *e) {
    if (depth > MP_IMAGE_MAX_LEVELS) {
        mp_err_set(e, "too many folder levels in %s", shown(prefix));
        return -1;
    }
    if (seen(x, dino)) {
        mp_err_set(e, "damaged package: the folder %s is reached twice (a loop)", shown(prefix));
        return -1;
    }
    uint8_t *d = NULL;
    uint64_t len = 0, pos = 0;
    if (read_dir(x, dino, prefix, &d, &len, e) != 0) return -1;
    mp_image_name *names = NULL;
    size_t nn = 0, capn = 0;
    dirent de;
    int r;
    while ((r = next_entry(d, len, &pos, &de, prefix, e)) == 0) {
        if (de.type == DT_DOT || de.type == DT_DOTDOT) continue;   // the folder itself and its parent
        if ((de.namelen == 1 && de.name[0] == '.') || (de.namelen == 2 && de.name[0] == '.' && de.name[1] == '.'))
            continue;
        if (++x->entries > MP_IMAGE_MAX_ENTRIES) {
            mp_err_set(e, "damaged package: more than %u files and folders", MP_IMAGE_MAX_ENTRIES);
            r = -1;
            break;
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
        if (!path) {
            r = -1;
            break;
        }
        if (de.ino >= x->ninodes) {
            mp_err_set(e, "damaged package: %s points to inode %" PRIu32 ", out of range", path, de.ino);
            free(path);
            r = -1;
            break;
        }
        const inode_t *n = &x->ino[de.ino];
        if (de.type == DT_DIR) {
            if (!is_dir(n)) {
                mp_err_set(e, "damaged package: the entry of %s does not match its inode", path);
                r = -1;
            } else {
                r = push_dir(x, path, e);
                if (r == 0) r = walk(x, de.ino, path, depth + 1, e);
            }
        } else if (de.type == DT_FILE) {
            if (!is_file(n)) {
                mp_err_set(e, "damaged package: the entry of %s does not match its inode", path);
                r = -1;
            } else if (seen(x, de.ino)) {
                mp_err_set(e, "%s is a hard link to another file: a game's package has none", path);
                r = -1;
            } else if (n->off > x->mount->size || n->size > x->mount->size - n->off) {
                mp_err_set(e, "damaged package: %s is outside its mount", path);
                r = -1;
            } else {
                frec v;
                memset(&v, 0, sizeof v);
                v.en.path = path;
                v.en.size = n->size;
                v.en.mtime = n->mtime;
                v.f.where = 0;
                v.f.off = n->off;
                path = NULL;
                r = push(x, v, e);
                if (r != 0) free(v.en.path);
            }
        } else {
            mp_err_set(e, "%s: an entry of type %" PRIu32 " (neither a file nor a folder): not supported", path, de.type);
            r = -1;
        }
        free(path);
        if (r != 0) break;
    }
    if (r == 1) r = mp_image_unique(names, nn, shown(prefix), e);
    free(names);
    free(d);
    return r;
}

// The uroot folder of the super-root (inode 0).
static int find_uroot(fpkg_src *x, uint64_t *uroot, mp_err *e) {
    if (!is_dir(&x->ino[0])) {
        mp_err_set(e, "damaged package: the root of its PFS is not a folder");
        return -1;
    }
    uint8_t *d = NULL;
    uint64_t len = 0, pos = 0;
    if (read_dir(x, 0, "(super-root)", &d, &len, e) != 0) return -1;
    dirent de;
    int r, found = 0;
    while (!found && (r = next_entry(d, len, &pos, &de, "(super-root)", e)) == 0)
        if (de.type == DT_DIR && de.namelen == 5 && !memcmp(de.name, "uroot", 5) && de.ino < x->ninodes &&
            is_dir(&x->ino[de.ino])) {
            *uroot = de.ino;
            found = 1;
        }
    free(d);
    if (!found) {
        if (r != -1) mp_err_set(e, "damaged package: its PFS has no uroot folder");
        return -1;
    }
    return 0;
}

// For a block map read in more than one way: 0 if this reading holds a PFS with uroot.
typedef struct {
    mp_image *inner;
    uint64_t hint;
} probe_ctx;

static int probe(void *ctx, const mp_naps_plan *plan, mp_err *e) {
    probe_ctx *c = ctx;
    mp_image *mount = NULL;
    mp_naps_plan copy = *plan;
    if (mp_naps_mount(c->inner, &copy, 0, "(probe)", &mount, e) != 0) return -1;
    fpkg_src x;
    memset(&x, 0, sizeof x);
    x.mount = mount;
    uint64_t super = 0, uroot = 0;
    int rc = find_super(mount, c->hint, &super, e);
    if (rc == 0) rc = load_inodes(&x, super, e);
    if (rc == 0) rc = find_uroot(&x, &uroot, e);
    free(x.ino);
    free(x.seen);
    mp_image_close(mount);
    return rc;
}

static int ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        int p = (unsigned char)*a, q = (unsigned char)*b;
        if (p >= 'A' && p <= 'Z') p += 32;
        if (q >= 'A' && q <= 'Z') q += 32;
        if (p != q) return 0;
        if (!p) return 1;
    }
}

// "sce_sys/<name>" for a CNT entry's name, each part checked as an image's names are; NULL with e set.
static char *sce_path(const char *name, mp_err *e) {
    char *path = mp_strdup("sce_sys");
    if (!path) {
        oom(e);
        return NULL;
    }
    const char *s = name;
    for (;;) {
        const char *slash = strchr(s, '/');
        size_t n = slash ? (size_t)(slash - s) : strlen(s);
        char *next = mp_image_join(path, (const uint8_t *)s, n, e);
        free(path);
        if (!next) return NULL;
        path = next;
        if (!slash) return path;
        s = slash + 1;
    }
}

// The CNT's sce_sys files a dump has, unless the inner PFS has them too (then its own wins).
static int add_entries(fpkg_src *x, int64_t mtime, mp_err *e) {
    mp_fpkg *p = x->pkg;
    size_t inner_n = x->n;
    for (size_t i = 0; i < p->nentries; i++) {
        const char *name = mp_fpkg_entry_path(p, i);
        if (!name) continue;
        mp_err pe = {{0}};
        char *path = sce_path(name, &pe);
        if (!path) {
            mp_err_set(e, "damaged package: an entry has an invalid name (%s)", pe.msg);
            return -1;
        }
        int dup = 0;
        for (size_t k = 0; k < x->n && !dup; k++) dup = ieq(x->v[k].en.path, path);
        for (size_t k = 0; k < x->nd && !dup; k++) dup = ieq(x->dirs[k], path);
        (void)inner_n;
        if (dup) {
            free(path);
            continue;
        }
        const mp_cnt_entry *en = &p->entries[i];
        frec v;
        memset(&v, 0, sizeof v);
        v.en.path = path;
        v.en.size = en->data_size;
        v.en.mtime = mtime;
        if (en->flags1 & 0x80000000u) {
            size_t len = 0;
            if (mp_fpkg_entry(p, i, &v.f.data, &len, e) != 0) {
                free(path);
                return -1;
            }
            v.f.where = 2;
        } else {
            uint64_t at = p->cnt_off + en->data_off;
            if (at < p->cnt_off || at > p->file->size || en->data_size > p->file->size - at) {
                mp_err_set(e, "damaged package: the entry of %s is outside it", path);
                free(path);
                return -1;
            }
            v.f.where = 1;
            v.f.off = at;
        }
        // its folders (sce_sys, trophy2, ...) if the inner PFS does not have them
        for (char *s = strchr(path, '/'); s; s = strchr(s + 1, '/')) {
            *s = 0;
            int have = 0, file = 0;
            for (size_t k = 0; k < x->nd && !have; k++) have = !strcmp(x->dirs[k], path);
            for (size_t k = 0; k < x->n && !have && !file; k++) file = ieq(x->v[k].en.path, path);
            int rc = 0;
            if (file) {
                mp_err_set(e, "damaged package: %s is a file and the folder of an entry", path);
                rc = -1;
            } else if (!have) {
                rc = push_dir(x, path, e);
            }
            *s = '/';
            if (rc != 0) {
                free(v.f.data);
                free(path);
                return -1;
            }
        }
        if (push(x, v, e) != 0) {
            free(v.f.data);
            free(path);
            return -1;
        }
    }
    return 0;
}

static int fpkg_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e) {
    fpkg_src *x = s->impl;
    const ffile *f = &x->files[i];
    if (f->where == 2) {
        memcpy(buf, f->data + off, len);
        return 0;
    }
    mp_image *m = f->where == 0 ? x->mount : x->pkg->file;
    size_t got = 0;
    if (mp_image_pread(m, buf, len, f->off + off, &got, e) != 0) return -1;
    if (got != len) {
        mp_err_set(e, "truncated package: the content of %s is missing", s->entries[i].path);
        return -1;
    }
    return 0;
}

static void src_free(fpkg_src *x, size_t nfiles) {
    if (!x) return;
    if (x->files)
        for (size_t i = 0; i < nfiles; i++) free(x->files[i].data);
    for (size_t i = 0; i < x->n; i++) {
        free(x->v[i].en.path);
        free(x->v[i].f.data);
    }
    for (size_t i = 0; i < x->nd; i++) free(x->dirs[i]);
    free(x->dirs);
    free(x->v);
    free(x->files);
    free(x->ino);
    free(x->seen);
    mp_image_close(x->mount);
    mp_fpkg_close(x->pkg);
    free(x);
}

static void fpkg_close(mp_source *s) { src_free(s->impl, s->n); }

static const char *fpkg_id(const mp_source *s) { return ((const fpkg_src *)s->impl)->pkg->content_id; }

static int cmp_rec(const void *a, const void *b) {
    return strcmp(((const frec *)a)->en.path, ((const frec *)b)->en.path);
}

int mp_fpkg_open_source(mp_image *img, const char *passcode, int accept, mp_source **out, mp_err *e) {
    *out = NULL;
    fpkg_src *x = calloc(1, sizeof *x);
    if (!x) {
        mp_image_close(img);
        return oom(e);
    }
    if (mp_fpkg_open(img, passcode, accept, &x->pkg, e) != 0) {
        free(x);
        return -1;
    }
    mp_fpkg *p = x->pkg;
    mp_image *inner = NULL;
    mp_naps_plan plan;
    memset(&plan, 0, sizeof plan);
    int rc = mp_fpkg_inner_image(p, &inner, e);
    if (rc == 0) {
        probe_ctx pc = {inner, p->inner_meta_base};
        mp_err pe = {{0}};
        rc = mp_naps_plan_of(p->naps, p->naps_len, inner->size, probe, &pc, &plan, &pe);
        if (rc != 0) mp_err_set(e, "%s: %s", p->file->name, pe.msg);
    }
    if (rc == 0) {
        char *name = malloc(strlen(p->file->name) + 16);
        if (!name) {
            rc = oom(e);
        } else {
            sprintf(name, "%s (mount)", p->file->name);
            rc = mp_naps_mount(inner, &plan, 1, name, &x->mount, e);
            free(name);
            inner = NULL;
        }
    }
    mp_image_close(inner);
    mp_naps_plan_free(&plan);
    uint64_t super = 0, uroot = 0;
    if (rc == 0) rc = find_super(x->mount, p->inner_meta_base, &super, e);
    if (rc == 0) rc = load_inodes(x, super, e);
    if (rc == 0) rc = find_uroot(x, &uroot, e);
    if (rc == 0) rc = walk(x, uroot, "", 0, e);
    if (rc == 0) rc = add_entries(x, x->ino[uroot].mtime, e);
    mp_source *s = NULL;
    mp_entry *entries = NULL;
    if (rc == 0) {
        s = calloc(1, sizeof *s);
        x->files = malloc((x->n ? x->n : 1) * sizeof *x->files);
        entries = malloc((x->n ? x->n : 1) * sizeof *entries);
        if (!s || !x->files || !entries) rc = oom(e);
    }
    if (rc != 0) {
        free(s);
        free(entries);
        src_free(x, 0);
        return -1;
    }
    if (x->n > 1) qsort(x->v, x->n, sizeof *x->v, cmp_rec);
    for (size_t i = 0; i < x->n; i++) {
        entries[i] = x->v[i].en;
        x->files[i] = x->v[i].f;
    }
    s->kind = s->inner = MP_SRC_FPKG;
    s->compressed = x->mount->compressed;
    s->dlc_package = p->content_type == 0x21;
    s->package_id = fpkg_id;
    s->entries = entries;
    s->n = x->n;
    s->ndirs = x->ndirs;
    s->read = fpkg_read;
    s->close = fpkg_close;
    s->impl = x;
    free(x->v);
    x->v = NULL;
    x->n = 0;
    free(x->ino);
    x->ino = NULL;
    free(x->seen);
    x->seen = NULL;
    for (size_t i = 0; i < x->nd; i++) free(x->dirs[i]);
    free(x->dirs);
    x->dirs = NULL;
    x->nd = 0;
    *out = s;
    return 0;
}
