// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_describe.h"
#include "mp_p5z_internal.h"
#include "mp_param.h"
#include "mp_source.h"

#define RUNTIME_PATH "fakelib/libSceAmpr.sprx"
#define INDEX_PATH "ampr_zstd.index"
#define PARAM_PATH "sce_sys/param.json"
#define ICON_PATH "sce_sys/icon0.png"
#define RUNTIME_MAX (64u << 20)
#define PARAM_MAX (1u << 20)
#define ICON_MAX (16u << 20)

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

// ---- carpeta e imagen ---------------------------------------------------------------------

static int read_entry(mp_source *s, size_t i, size_t max, uint8_t **out, size_t *len, mp_err *e) {
    const mp_entry *en = mp_source_entry(s, i);
    *out = NULL;
    *len = 0;
    if (en->size > max) {
        mp_err_set(e, "%s es demasiado grande (%llu bytes)", en->path, (unsigned long long)en->size);
        return -1;
    }
    uint8_t *b = malloc((size_t)en->size + 1);
    if (!b) {
        mp_err_set(e, "sin memoria");
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

// Los packs de una carpeta ya empaquetada, como los dice la cabecera de su ampr_zstd.index
// (AMPRZPK1: volúmenes en +20, archivos en +40). Solo se leen los contadores: comprobar el
// índice entero es cosa de verify. Una cabecera que no se puede leer deja los dos a 0.
static void index_counts(mp_source *s, size_t i, mp_describe *d) {
    uint8_t h[MP_ZPK_HEADER];
    if (mp_source_entry(s, i)->size < sizeof h || mp_source_read(s, i, 0, h, sizeof h, NULL) != 0 ||
        memcmp(h, "AMPRZPK1", 8) != 0)
        return;
    d->volumes = rd32(h + 20);
    d->packed_files = rd64(h + 40);
}

static int describe_source(const char *path, mp_describe *d, mp_err *e) {
    mp_source *s = NULL;
    if (mp_source_open(path, &s, e) != 0) return -1;
    int rc = -1;
    snprintf(d->kind, sizeof d->kind, "%s", mp_source_kind_of(s) == MP_SRC_EXFAT ? "exfat" : "folder");
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
        if (read_entry(s, (size_t)pj, PARAM_MAX, &b, &len, NULL) == 0) {   // ilegible: sin nombre
            mp_param_read((const char *)b, len, d->title_id, d->version, d->name);
            free(b);
        }
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
        mp_err_set(e, "archivo demasiado grande");
        return -1;
    }
    if (m->n + n > m->cap) {
        size_t cap = m->cap ? m->cap : 4096;
        while (cap < m->n + n) cap *= 2;
        uint8_t *nb = realloc(m->b, cap);
        if (!nb) {
            mp_err_set(e, "sin memoria");
            return -1;
        }
        m->b = nb;
        m->cap = cap;
    }
    memcpy(m->b + m->n, p, n);
    m->n += n;
    return 0;
}

// Índice en files[] del archivo guardado con esa ruta (no el original reemplazado), o -1.
static long p5z_find(const mp_p5z *p, const char *path) {
    for (size_t i = 0; i < p->nfiles; i++)
        if (p->files[i].role != MP_ROLE_ORIGINAL && ieq(p->files[i].path, path)) return (long)i;
    return -1;
}

// Lee un archivo del .p5z: guardado tal cual o, si va en los packs, descomprimido de ellos.
static int p5z_slurp(mp_p5z *p, const char *path, size_t max, uint8_t **out, size_t *len, mp_err *e) {
    *out = NULL;
    *len = 0;
    long fi = p5z_find(p, path);
    if (fi >= 0) {
        if (p->files[fi].size > max) {
            mp_err_set(e, "%s es demasiado grande", path);
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
            mp_err_set(e, "%s es demasiado grande", path);
            return -1;
        }
        mp_zpk *z = NULL;
        if (mp_p5z_zpk(p, &z, e) != 0) return -1;
        long zi = mp_zpk_find(z, p->man.v[i].path);
        int rc = -1;
        if (zi < 0 || mp_zpk_size(z, (size_t)zi) != p->man.v[i].size) {
            mp_err_set(e, "%s no está en los packs del .p5z", path);
        } else {
            size_t sz = (size_t)p->man.v[i].size;
            uint8_t *b = malloc(sz + 1);
            if (!b) {
                mp_err_set(e, "sin memoria");
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
    mp_err_set(e, "el .p5z no tiene %s", path);
    return -1;
}

static int describe_p5z(const char *path, mp_describe *d, mp_err *e) {
    mp_p5z *p = NULL;
    if (mp_p5z_open(path, &p, e) != 0) return -1;
    int rc = -1;
    snprintf(d->kind, sizeof d->kind, "p5z");
    const mp_pack_manifest *m = &p->man;
    for (size_t i = 0; i < m->n; i++) {
        d->original_size += m->v[i].size;
        if (m->v[i].flags & MP_MANIFEST_PACKED) d->packed_files++;
    }
    for (size_t i = 0; i < p->nfiles; i++) {
        if (p->files[i].role != MP_ROLE_ORIGINAL) d->install_size += p->files[i].size;
        if (p->files[i].role == MP_ROLE_VOLUME) d->volumes++;
    }
    d->files = m->n;
    d->p5z_size = p->size;
    d->packed = 1;   // lo empaquetó MaxPack: lleva ampr_zstd.index
    snprintf(d->title_id, sizeof d->title_id, "%s", m->title_id);
    snprintf(d->version, sizeof d->version, "%s", m->content_version);
    snprintf(d->name, sizeof d->name, "%s", m->title_name);
    if (p5z_find(p, RUNTIME_PATH) >= 0) {
        uint8_t *b = NULL;
        size_t len = 0;
        if (p5z_slurp(p, RUNTIME_PATH, RUNTIME_MAX, &b, &len, e) != 0) goto done;
        d->has_runtime = 1;
        d->has_zstd_runtime = has_marker(b, len);
        free(b);
    }
    rc = 0;
done:
    mp_p5z_close(p);
    return rc;
}

int mp_describe_path(const char *path, mp_describe *d, mp_err *e) {
    memset(d, 0, sizeof *d);
    if (!path || !*path) {
        mp_err_set(e, "falta la ruta");
        return -1;
    }
    return is_p5z_name(path) ? describe_p5z(path, d, e) : describe_source(path, d, e);
}

int mp_describe_icon(const char *path, uint8_t **png, size_t *len, mp_err *e) {
    *png = NULL;
    *len = 0;
    if (!path || !*path) {
        mp_err_set(e, "falta la ruta");
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
    if (mp_source_open(path, &s, e) != 0) return -1;
    long i = find_ci(s, ICON_PATH);
    int rc = -1;
    if (i < 0) mp_err_set(e, "no tiene %s", ICON_PATH);
    else rc = read_entry(s, (size_t)i, ICON_MAX, png, len, e);
    mp_source_close(s);
    return rc;
}
