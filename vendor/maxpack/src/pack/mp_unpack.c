// SPDX-License-Identifier: GPL-3.0-or-later
// unpack: folder for the console (--folder) or bit-for-bit original dump (--original).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_p5z_internal.h"

#define PIECE (4u << 20)

static int prepare_out(const char *dir, mp_err *e) {
    int kind = mp_path_kind(dir);
    if (kind == 1) {
        mp_err_set(e, "the output is a file: %s", dir);
        return -1;
    }
    if (kind == 2 && mp_dir_is_empty(dir) != 1) {
        mp_err_set(e, "the output folder is not empty: %s", dir);
        return -1;
    }
    return kind == 0 ? mp_make_dirs(dir, e) : 0;
}

static char *out_path(const char *dir, const char *rel, mp_err *e) {
    size_t a = strlen(dir), b = strlen(rel);
    char *p = malloc(a + b + 2);
    if (!p) {
        mp_err_set(e, "out of memory");
        return NULL;
    }
    memcpy(p, dir, a);
    p[a] = '/';
    memcpy(p + a + 1, rel, b + 1);
    char *slash = strrchr(p, '/');
    if (slash > p + a) {
        *slash = 0;
        int rc = mp_make_dirs(p, e);
        *slash = '/';
        if (rc != 0) {
            free(p);
            return NULL;
        }
    }
    return p;
}

static int to_wfile(void *ctx, const void *p, size_t n, mp_err *e) {
    if (mp_cancel_check(e) != 0) return -1;
    return mp_wfile_write(ctx, p, n, e);
}

static int write_stored(mp_p5z *p, size_t fi, const char *dir, const char *rel, int64_t mtime,
                        mp_progress *prog, mp_err *e) {
    char *path = out_path(dir, rel, e);
    mp_wfile *w = NULL;
    if (!path) return -1;
    int rc = mp_wfile_create(path, &w, e);
    if (rc == 0) rc = p5z_read_file(p, fi, to_wfile, w, e);
    if (w && mp_wfile_close(w, e) != 0) rc = -1;
    if (rc == 0 && mtime) rc = mp_set_mtime(path, mtime, e);
    mp_progress_add(prog, p->files[fi].size);
    mp_progress_maybe_print(prog);
    free(path);
    return rc;
}

int mp_unpack_folder(mp_p5z *p, const char *out_dir, unsigned threads, int progress, mp_err *e) {
    return mp_unpack_folder_ex(p, out_dir, MP_ZPK_VOLUME_LIMIT, threads, progress, e);
}

// A .p5z that grew: its volumes hold the chunks in any order and with room between them, which only
// ampr-emu-ng's p5z mode takes. The folder gets them laid out again, as pack --folder lays them out, with
// the ampr_zstd.index that goes with them (nothing is compressed again). z: its packs; zi: the record of
// its ampr_zstd.index.
static int relayout_to(mp_p5z *p, mp_zpk *z, size_t zi, const char *out_dir, uint64_t limit, mp_progress *prog,
                       mp_err *e) {
    mp_zpk_sink sink;
    memset(&sink, 0, sizeof sink);
    uint8_t *index = NULL, aid[16];
    size_t len = 0;
    int rc = mp_zpk_sink_dir(out_dir, &sink, e);
    if (!rc) rc = mp_zpk_relayout(z, limit, &sink, prog, &index, &len, aid, e);
    mp_zpk_sink_free(&sink);
    char *path = rc ? NULL : out_path(out_dir, p->files[zi].path, e);
    if (!rc && (!path || mp_write_file(path, index, len, e) != 0)) rc = -1;
    if (!rc && p->files[zi].mtime) rc = mp_set_mtime(path, p->files[zi].mtime, e);
    free(path);
    free(index);
    return rc;
}

int mp_unpack_folder_ex(mp_p5z *p, const char *out_dir, uint64_t volume_limit, unsigned threads, int progress,
                        mp_err *e) {
    (void)threads;
    if (prepare_out(out_dir, e) != 0) return -1;
    // a .p5z that grew has its packs laid out again: what is written of them is their chunks, not the room
    // between them, and that is what the progress counts
    int grown = p->version == 2;
    size_t zi = SIZE_MAX;
    mp_zpk *z = NULL;
    uint64_t total = 0;
    for (size_t i = 0; i < p->nfiles; i++) {
        int role = p->files[i].role;
        if (role == MP_ROLE_ZPK_INDEX) zi = i;
        if (role != MP_ROLE_ORIGINAL && !(grown && (role == MP_ROLE_VOLUME || role == MP_ROLE_ZPK_INDEX)))
            total += p->files[i].size;
    }
    if (grown && zi != SIZE_MAX) {
        if (mp_p5z_zpk(p, &z, e) != 0) return -1;
        total += mp_zpk_stored_bytes(z);
    }
    mp_progress prog;
    mp_progress_init(&prog, "Extracting the folder", total, progress);
    int rc = 0;
    for (size_t i = 0; i < p->nfiles && !rc; i++) {
        int role = p->files[i].role;
        if (role == MP_ROLE_ORIGINAL || (grown && (role == MP_ROLE_VOLUME || role == MP_ROLE_ZPK_INDEX))) continue;
        rc = write_stored(p, i, out_dir, p->files[i].path, p->files[i].mtime, &prog, e);
    }
    if (!rc && z) rc = relayout_to(p, z, zi, out_dir, volume_limit, &prog, e);
    if (!rc) rc = mp_cancel_check(e);
    if (!rc && progress) fprintf(stderr, "\rExtracting the folder: 100.0 %%\n");
    mp_progress_free(&prog);
    mp_zpk_close(z);
    return rc;
}

int mp_unpack_original(mp_p5z *p, const char *out_dir, unsigned threads, int progress, mp_err *e) {
    (void)threads;
    if (prepare_out(out_dir, e) != 0) return -1;
    mp_zpk *z = NULL;
    int has_packs = 0;
    for (size_t i = 0; i < p->man.n; i++) has_packs |= (p->man.v[i].flags & MP_MANIFEST_PACKED) != 0;
    if (has_packs && mp_p5z_zpk(p, &z, e) != 0) return -1;
    uint64_t total = 0;
    for (size_t i = 0; i < p->man.n; i++) total += p->man.v[i].size;
    mp_progress prog;
    mp_progress_init(&prog, "Rebuilding the original", total, progress);
    uint8_t *buf = malloc(PIECE);
    int rc = buf ? 0 : -1;
    if (!buf) mp_err_set(e, "out of memory");
    size_t zi = 0;   // the manifest and the packs are in a different order: lookup by path
    for (size_t i = 0; i < p->man.n && !rc; i++) {
        const mp_manifest_entry *m = &p->man.v[i];
        if (mp_cancel_check(e) != 0) {
            rc = -1;
            break;
        }
        if (!(m->flags & MP_MANIFEST_PACKED)) {
            rc = write_stored(p, p->man_file[i], out_dir, m->path, m->mtime, &prog, e);
            continue;
        }
        long found = mp_zpk_find(z, m->path);   // binary search: O(log n) per file
        zi = found < 0 ? mp_zpk_count(z) : (size_t)found;
        if (zi == mp_zpk_count(z) || mp_zpk_size(z, zi) != m->size) {
            mp_err_set(e, "%s is not in the .p5z's packs", m->path);
            rc = -1;
            break;
        }
        char *path = out_path(out_dir, m->path, e);
        mp_wfile *w = NULL;
        if (!path || mp_wfile_create(path, &w, e) != 0) {
            free(path);
            rc = -1;
            break;
        }
        mp_sha256 s;
        mp_sha256_init(&s);
        for (uint64_t off = 0; !rc && off < m->size; off += PIECE) {
            size_t k = m->size - off < PIECE ? (size_t)(m->size - off) : PIECE;
            rc = mp_cancel_check(e);
            if (!rc) rc = mp_zpk_read_file(z, zi, off, buf, k, e);
            if (!rc) rc = mp_wfile_write(w, buf, k, e);
            if (!rc) mp_sha256_update(&s, buf, k);
            mp_progress_add(&prog, k);
            mp_progress_maybe_print(&prog);
        }
        if (mp_wfile_close(w, e) != 0) rc = -1;
        uint8_t d[32];
        mp_sha256_final(&s, d);
        if (!rc && memcmp(d, m->sha, 32) != 0) {
            mp_err_set(e, "%s: the SHA-256 does not match the original dump's", m->path);
            rc = -1;
        }
        if (!rc) rc = mp_set_mtime(path, m->mtime, e);
        free(path);
    }
    if (!rc) rc = mp_cancel_check(e);
    if (!rc && progress) fprintf(stderr, "\rRebuilding the original: 100.0 %%\n");
    mp_progress_free(&prog);
    free(buf);
    mp_zpk_close(z);
    return rc;
}
