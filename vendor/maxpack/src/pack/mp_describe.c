// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_describe.h"
#include "mp_p5z_internal.h"
#include "mp_param.h"
#include "mp_playgo.h"
#include "mp_rules.h"
#include "mp_source.h"

#define RUNTIME_PATH "fakelib/libSceAmpr.sprx"
#define INDEX_PATH "ampr_zstd.index"
#define PARAM_PATH "sce_sys/param.json"
#define ICON_PATH "sce_sys/icon0.png"
#define RUNTIME_MAX (64u << 20)
#define PARAM_MAX (1u << 20)
#define ICON_MAX (16u << 20)
#define ZPK_INDEX_MAX (1u << 30)
#define DUMP_INDEX_MAX (512u << 20)

static int ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static int has_marker(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 8 <= n; i++)
        if (p[i] == 'A' && !memcmp(p + i, "AMPRZPK1", 8)) return 1;
    return 0;
}

static int is_p5z_name(const char *path) {
    size_t n = strlen(path);
    return n >= 4 && ieq(path + n - 4, ".p5z");
}

// ---- folder and image ---------------------------------------------------------------------

static int read_entry(mp_source *s, size_t i, size_t max, uint8_t **out, size_t *len, mp_err *e) {
    const mp_entry *en = mp_source_entry(s, i);
    *out = NULL;
    *len = 0;
    if (en->size > max) {
        mp_err_set(e, "%s is too large (%llu bytes)", en->path, (unsigned long long)en->size);
        return -1;
    }
    uint8_t *b = malloc((size_t)en->size + 1);
    if (!b) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    if (mp_source_read(s, i, 0, b, (size_t)en->size, e) != 0) {
        free(b);
        return -1;
    }
    *out = b;
    *len = (size_t)en->size;
    return 0;
}

static long find_ci(mp_source *s, const char *path) {
    long i = mp_source_find(s, path);
    if (i >= 0) return i;
    for (size_t k = 0; k < mp_source_count(s); k++)
        if (ieq(mp_source_entry(s, k)->path, path)) return (long)k;
    return -1;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

// The packs of an already packed folder, as its ampr_zstd.index header says
// (AMPRZPK1: volumes at +20, files at +40). Only the counters are read: checking the
// whole index is verify's job. A header that cannot be read leaves both at 0.
static void index_counts(mp_source *s, size_t i, mp_describe *d) {
    uint8_t h[MP_ZPK_HEADER];
    if (mp_source_entry(s, i)->size < sizeof h || mp_source_read(s, i, 0, h, sizeof h, NULL) != 0 ||
        memcmp(h, "AMPRZPK1", 8) != 0)
        return;
    d->volumes = rd32(h + 20);
    d->packed_files = rd64(h + 40);
}

static int cmp_cfile(const void *a, const void *b) {
    return strcmp(((const mp_cfile *)a)->path, ((const mp_cfile *)b)->path);
}

// What the DLC of dlc hold, from the files of a source.
static int dlc_sizes_of_source(mp_source *s, mp_dlc_list *dlc, mp_err *e) {
    size_t n = mp_source_count(s);
    mp_cfile *files = malloc((n ? n : 1) * sizeof *files);
    if (!files) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        files[i].path = mp_source_entry(s, i)->path;
        files[i].size = mp_source_entry(s, i)->size;
    }
    mp_dlc_sizes(dlc, files, n);
    free(files);
    return 0;
}

// The DLC the dlc_emu.ini of bytes[0..len) lists (none when bytes is NULL). 0, or -1 out of memory.
static int dlc_of_ini(const uint8_t *bytes, size_t len, mp_dlc_list *dlc, mp_err *e) {
    if (!bytes) return 0;
    if (mp_dlc_parse((const char *)bytes, len, dlc) != 0) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    return 0;
}

// A compressed folder against its table: its loose files (not the packs and their index, which
// MaxPack made) and the files in its packs. A table that is not loose may be in the packs. Packs
// that cannot be opened leave it unchecked ("unreadable"). dlc, when not NULL, gets what the folder
// of each DLC holds, in the same files (only the loose ones when the packs cannot be opened).
static int complete_packed(const char *dir, mp_source *s, size_t ix, mp_complete *c, mp_dlc_list *dlc, mp_err *e) {
    memset(c, 0, sizeof *c);
    uint8_t *idx = NULL, *table = NULL;
    size_t idx_len = 0, table_len = 0;
    mp_zpk_volsrc vs = {0};
    mp_zpk *z = NULL;
    mp_err ze = {{0}};
    if (read_entry(s, ix, ZPK_INDEX_MAX, &idx, &idx_len, &ze) != 0 || mp_zpk_volsrc_dir(dir, &vs, &ze) != 0 ||
        mp_zpk_open(idx, idx_len, &vs, &z, &ze) != 0) {
        free(idx);
        mp_zpk_volsrc_free(&vs);
        int rc = mp_complete_check(NULL, 0, NULL, 0, c, NULL);
        c->reason = "unreadable";
        if (rc != 0) mp_err_set(e, "out of memory");
        if (rc == 0 && dlc) rc = dlc_sizes_of_source(s, dlc, e);
        return rc;
    }
    free(idx);
    size_t n = mp_source_count(s), nz = mp_zpk_count(z), k = 0;
    mp_cfile *files = malloc((n + nz + 1) * sizeof *files);
    int rc = -1;
    if (!files) goto oom;
    for (size_t i = 0; i < n; i++) {
        const mp_entry *en = mp_source_entry(s, i);
        if (ieq(en->path, INDEX_PATH) || mp_glob_match("ampr_zstd-*.pak", en->path)) continue;
        files[k].path = en->path;
        files[k++].size = en->size;
    }
    for (size_t i = 0; i < nz; i++) {
        files[k].path = mp_zpk_path(z, i);
        files[k++].size = mp_zpk_size(z, i);
    }
    qsort(files, k, sizeof *files, cmp_cfile);
    if (dlc) mp_dlc_sizes(dlc, files, k);
    long t = find_ci(s, MP_PLAYGO_TABLE_PATH);
    int unreadable = 0;
    if (t >= 0) {
        unreadable = read_entry(s, (size_t)t, MP_PLAYGO_TABLE_MAX, &table, &table_len, NULL) != 0;
    } else {
        for (size_t i = 0; i < nz; i++) {
            if (!ieq(mp_zpk_path(z, i), MP_PLAYGO_TABLE_PATH)) continue;
            uint64_t size = mp_zpk_size(z, i);
            table = size <= MP_PLAYGO_TABLE_MAX ? malloc((size_t)size + 1) : NULL;
            if (!table || mp_zpk_read_file(z, i, 0, table, (size_t)size, NULL) != 0) {
                free(table);
                table = NULL;
                unreadable = 1;
            } else {
                table_len = (size_t)size;
            }
            break;
        }
    }
    if (mp_complete_check(files, k, table, table_len, c, NULL) != 0) goto oom;
    if (unreadable) c->reason = "unreadable";
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "out of memory");
done:
    free(table);
    free(files);
    mp_zpk_close(z);
    mp_zpk_volsrc_free(&vs);
    return rc;
}

static int describe_source(const char *path, mp_describe *d, mp_complete *c, mp_dlc_list *dlc, mp_err *e) {
    mp_source *s = NULL;
    if (mp_source_open_as(path, MP_OPEN_ANY, &s, e) != 0) return -1;
    int rc = -1;
    snprintf(d->kind, sizeof d->kind, "%s", mp_source_kind_name(s));
    snprintf(d->inner, sizeof d->inner, "%s", mp_source_inner_name(s));
    d->compressed = mp_source_compressed(s);
    size_t n = mp_source_count(s);
    for (size_t i = 0; i < n; i++) d->original_size += mp_source_entry(s, i)->size;
    d->install_size = d->original_size;
    d->files = n;
    long ix = find_ci(s, INDEX_PATH);
    d->packed = ix >= 0;
    if (ix >= 0) index_counts(s, (size_t)ix, d);
    long rt = find_ci(s, RUNTIME_PATH);
    if (rt >= 0) {
        uint8_t *b = NULL;
        size_t len = 0;
        if (read_entry(s, (size_t)rt, RUNTIME_MAX, &b, &len, e) != 0) goto done;
        d->has_runtime = 1;
        d->has_zstd_runtime = has_marker(b, len);
        free(b);
    }
    long pj = find_ci(s, PARAM_PATH);
    if (pj >= 0) {
        uint8_t *b = NULL;
        size_t len = 0;
        if (read_entry(s, (size_t)pj, PARAM_MAX, &b, &len, NULL) == 0) {   // unreadable: no name
            mp_param_read((const char *)b, len, d->title_id, d->version, d->name);
            mp_param_content_id((const char *)b, len, d->content_id);
            free(b);
        }
    }
    if (!d->content_id[0]) snprintf(d->content_id, sizeof d->content_id, "%s", mp_source_package_id(s));
    // a DLC: its package's content ID, and the title ID of it (what pack --dlc matches with the game's).
    // A package is what its content type says; a folder or image, one with param.json and no eboot.bin
    d->dlc = mp_source_kind_of(s) == MP_SRC_FPKG ? mp_source_is_dlc_package(s)
                                                 : !d->packed && pj >= 0 && find_ci(s, "eboot.bin") < 0;
    if (d->dlc) {
        if (mp_source_is_dlc_package(s)) snprintf(d->content_id, sizeof d->content_id, "%s", mp_source_package_id(s));
        if (strlen(d->content_id) >= 16 && d->content_id[6] == '-')
            snprintf(d->title_id, sizeof d->title_id, "%.9s", d->content_id + 7);
        d->dlc_data = mp_dlc_has_data(s);
    }
    if (dlc) {
        long ini = find_ci(s, MP_DLC_INI);
        uint8_t *b = NULL;
        size_t len = 0;
        if (ini >= 0 && read_entry(s, (size_t)ini, MP_DLC_INI_MAX, &b, &len, NULL) != 0) b = NULL;
        int ir = dlc_of_ini(b, len, dlc, e);
        free(b);
        if (ir != 0) goto done;
    }
    if (c || dlc) {
        mp_complete scratch, *cc = c ? c : &scratch;
        int crc = 0;
        if (ix >= 0 && mp_source_kind_of(s) == MP_SRC_DIR) {
            crc = complete_packed(path, s, (size_t)ix, cc, dlc, e);
        } else {
            memset(&scratch, 0, sizeof scratch);
            if (c) crc = mp_complete_source(s, c, NULL, e);
            if (crc == 0 && dlc) crc = dlc_sizes_of_source(s, dlc, e);
        }
        if (!c) mp_complete_free(&scratch);
        if (crc != 0) goto done;
    }
    rc = 0;
done:
    mp_source_close(s);
    return rc;
}

// ---- .p5z ---------------------------------------------------------------------------------

typedef struct {
    uint8_t *b;
    size_t n, cap, max;
} membuf;

static int to_mem(void *ctx, const void *p, size_t n, mp_err *e) {
    membuf *m = ctx;
    if (m->n + n > m->max) {
        mp_err_set(e, "file too large");
        return -1;
    }
    if (m->n + n > m->cap) {
        size_t cap = m->cap ? m->cap : 4096;
        while (cap < m->n + n) cap *= 2;
        uint8_t *nb = realloc(m->b, cap);
        if (!nb) {
            mp_err_set(e, "out of memory");
            return -1;
        }
        m->b = nb;
        m->cap = cap;
    }
    memcpy(m->b + m->n, p, n);
    m->n += n;
    return 0;
}

// Index in files[] of the stored file with that path (not the replaced original), or -1.
static long p5z_find(const mp_p5z *p, const char *path) {
    for (size_t i = 0; i < p->nfiles; i++)
        if (p->files[i].role != MP_ROLE_ORIGINAL && ieq(p->files[i].path, path)) return (long)i;
    return -1;
}

// Reads a file from the .p5z: stored as is or, if it is in the packs, decompressed from them.
static int p5z_slurp(mp_p5z *p, const char *path, size_t max, uint8_t **out, size_t *len, mp_err *e) {
    *out = NULL;
    *len = 0;
    long fi = p5z_find(p, path);
    if (fi >= 0) {
        if (p->files[fi].size > max) {
            mp_err_set(e, "%s is too large", path);
            return -1;
        }
        membuf m = {0};
        m.max = max;
        if (p5z_read_file(p, (size_t)fi, to_mem, &m, e) != 0) {
            free(m.b);
            return -1;
        }
        if (!m.b) m.b = malloc(1);
        *out = m.b;
        *len = m.n;
        return 0;
    }
    for (size_t i = 0; i < p->man.n; i++) {
        if (!(p->man.v[i].flags & MP_MANIFEST_PACKED) || !ieq(p->man.v[i].path, path)) continue;
        if (p->man.v[i].size > max) {
            mp_err_set(e, "%s is too large", path);
            return -1;
        }
        mp_zpk *z = NULL;
        if (mp_p5z_zpk(p, &z, e) != 0) return -1;
        long zi = mp_zpk_find(z, p->man.v[i].path);
        int rc = -1;
        if (zi < 0 || mp_zpk_size(z, (size_t)zi) != p->man.v[i].size) {
            mp_err_set(e, "%s is not in the .p5z's packs", path);
        } else {
            size_t sz = (size_t)p->man.v[i].size;
            uint8_t *b = malloc(sz + 1);
            if (!b) {
                mp_err_set(e, "out of memory");
            } else if (mp_zpk_read_file(z, (size_t)zi, 0, b, sz, e) != 0) {
                free(b);
            } else {
                *out = b;
                *len = sz;
                rc = 0;
            }
        }
        mp_zpk_close(z);
        return rc;
    }
    mp_err_set(e, "the .p5z has no %s", path);
    return -1;
}

int mp_complete_p5z(mp_p5z *p, mp_complete *c, mp_err *e) {
    const mp_pack_manifest *m = &p->man;
    mp_cfile *files = malloc((m->n ? m->n : 1) * sizeof *files);
    if (!files) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    int has_table = 0;
    for (size_t i = 0; i < m->n; i++) {
        files[i].path = m->v[i].path;
        files[i].size = m->v[i].size;
        has_table |= ieq(m->v[i].path, MP_PLAYGO_TABLE_PATH);
    }
    uint8_t *table = NULL;
    size_t len = 0;
    int unreadable = has_table && p5z_slurp(p, MP_PLAYGO_TABLE_PATH, MP_PLAYGO_TABLE_MAX, &table, &len, NULL) != 0;
    // the dump tool's list: the original ampr_emu.index, which the .p5z keeps apart (the folder for
    // the console gets MaxPack's own); one that cannot be read is a damaged one
    membuf di = {0};
    int has_index = 0;
    for (size_t i = 0; i < p->nfiles && !has_index; i++) {
        if (p->files[i].role != MP_ROLE_ORIGINAL || !ieq(p->files[i].path, "ampr_emu.index")) continue;
        has_index = 1;
        di.max = DUMP_INDEX_MAX;
        if (p5z_read_file(p, i, to_mem, &di, NULL) != 0) {
            free(di.b);
            di.b = NULL;
            di.n = 0;
        }
        if (!di.b) di.b = malloc(1);
    }
    int rc = mp_complete_check_lists(files, m->n, table, len, has_index ? di.b : NULL, di.n, c, NULL);
    if (rc != 0)
        mp_err_set(e, "out of memory");
    else if (unreadable && !c->checkable && !strcmp(c->reason, "no_table"))
        c->reason = "unreadable";
    free(di.b);
    free(table);
    free(files);
    return rc;
}

static int describe_p5z(const char *path, mp_describe *d, mp_complete *c, mp_dlc_list *dlc, mp_err *e) {
    mp_p5z *p = NULL;
    if (mp_p5z_open(path, &p, e) != 0) return -1;
    int rc = -1;
    snprintf(d->kind, sizeof d->kind, "p5z");
    const mp_pack_manifest *m = &p->man;
    for (size_t i = 0; i < m->n; i++) {
        d->original_size += m->v[i].size;
        if (m->v[i].flags & MP_MANIFEST_PACKED) d->packed_files++;
    }
    d->grown = mp_p5z_grown(p);
    // packs that do not open (damaged) are for verify to tell: the room it does not use is just not said
    int packs_ok = mp_p5z_unused(p, &d->unused_bytes, NULL) == 0;
    if (!packs_ok) d->unused_bytes = 0;
    uint64_t holes = 0;   // a .p5z that grew: room in its volumes that unpack --folder leaves out
    for (size_t i = 0; i < p->nfiles; i++) {
        if (p->files[i].role != MP_ROLE_ORIGINAL) d->install_size += p->files[i].size;
        if (p->files[i].role == MP_ROLE_VOLUME) d->volumes++;
        if (p->files[i].role == MP_ROLE_VOLUME) holes += p->files[i].size;
        if (p->files[i].role == MP_ROLE_LOOSE || p->files[i].role == MP_ROLE_ZPK_INDEX ||
            p->files[i].role == MP_ROLE_IDX3)
            d->loose_bytes += p->files[i].size;
    }
    mp_zpk *z = NULL;
    if (d->grown && d->volumes && packs_ok && mp_p5z_zpk(p, &z, NULL) == 0) {
        holes -= MP_ZPK_VOL_HEADER * d->volumes + mp_zpk_stored_bytes(z);
        d->install_size -= holes;
    }
    mp_zpk_close(z);
    d->files = m->n;
    d->p5z_size = p->size;
    d->packed = 1;   // MaxPack packed it: it carries ampr_zstd.index
    snprintf(d->title_id, sizeof d->title_id, "%s", m->title_id);
    snprintf(d->version, sizeof d->version, "%s", m->content_version);
    snprintf(d->name, sizeof d->name, "%s", m->title_name);
    // the manifest has no content ID: the param.json the .p5z carries gives it (unreadable: none)
    uint8_t *pj = NULL;
    size_t pl = 0;
    if (p5z_slurp(p, PARAM_PATH, PARAM_MAX, &pj, &pl, NULL) == 0) {
        mp_param_content_id((const char *)pj, pl, d->content_id);
        free(pj);
    }
    if (p5z_find(p, RUNTIME_PATH) >= 0) {
        uint8_t *b = NULL;
        size_t len = 0;
        if (p5z_slurp(p, RUNTIME_PATH, RUNTIME_MAX, &b, &len, e) != 0) goto done;
        d->has_runtime = 1;
        d->has_zstd_runtime = has_marker(b, len);
        free(b);
    }
    if (c && mp_complete_p5z(p, c, e) != 0) goto done;
    if (dlc) {
        // its dlc_emu.ini (unreadable or too big: none), and what each folder holds of the dump
        uint8_t *b = NULL;
        size_t len = 0;
        if (p5z_slurp(p, MP_DLC_INI, MP_DLC_INI_MAX, &b, &len, NULL) != 0) b = NULL;
        int ir = dlc_of_ini(b, len, dlc, e);
        free(b);
        if (ir != 0) goto done;
        mp_cfile *files = malloc((m->n ? m->n : 1) * sizeof *files);
        if (!files) {
            mp_err_set(e, "out of memory");
            goto done;
        }
        for (size_t i = 0; i < m->n; i++) {
            files[i].path = m->v[i].path;
            files[i].size = m->v[i].size;
        }
        mp_dlc_sizes(dlc, files, m->n);
        free(files);
    }
    rc = 0;
done:
    mp_p5z_close(p);
    return rc;
}

int mp_describe_full(const char *path, mp_describe *d, mp_complete *c, mp_dlc_list *dlc, mp_err *e) {
    memset(d, 0, sizeof *d);
    if (c) memset(c, 0, sizeof *c);
    if (dlc) memset(dlc, 0, sizeof *dlc);
    if (!path || !*path) {
        mp_err_set(e, "missing path");
        return -1;
    }
    return is_p5z_name(path) ? describe_p5z(path, d, c, dlc, e) : describe_source(path, d, c, dlc, e);
}

int mp_describe_path_ex(const char *path, mp_describe *d, mp_complete *c, mp_err *e) {
    return mp_describe_full(path, d, c, NULL, e);
}

int mp_describe_path(const char *path, mp_describe *d, mp_err *e) { return mp_describe_path_ex(path, d, NULL, e); }

int mp_describe_complete(const char *path, mp_complete *c, mp_err *e) {
    mp_describe d;
    return mp_describe_path_ex(path, &d, c, e);
}

int mp_describe_icon(const char *path, uint8_t **png, size_t *len, mp_err *e) {
    *png = NULL;
    *len = 0;
    if (!path || !*path) {
        mp_err_set(e, "missing path");
        return -1;
    }
    if (is_p5z_name(path)) {
        mp_p5z *p = NULL;
        if (mp_p5z_open(path, &p, e) != 0) return -1;
        int rc = p5z_slurp(p, ICON_PATH, ICON_MAX, png, len, e);
        mp_p5z_close(p);
        return rc;
    }
    mp_source *s = NULL;
    if (mp_source_open_as(path, MP_OPEN_ANY, &s, e) != 0) return -1;   // a DLC's icon too
    long i = find_ci(s, ICON_PATH);
    int rc = -1;
    if (i < 0) mp_err_set(e, "it has no %s", ICON_PATH);
    else rc = read_entry(s, (size_t)i, ICON_MAX, png, len, e);
    mp_source_close(s);
    return rc;
}
