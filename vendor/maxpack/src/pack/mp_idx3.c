// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_idx3.h"

#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_platform.h"

#define IDX3_HEADER 48u
#define IDX3_RECORD 24u
#define IDX3_SLOT 16u
#define IDX3_SLOT_ALIGN 8u   // lo que exige leer un slot: mp_idx3_build alinea a 16, otros generadores a 8
#define APP0 "/app0/"
#define APP0_LEN 6u

static int fold(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int fold_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = fold((unsigned char)*a), y = fold((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

int mp_pack_path_check(const char *rel, mp_err *e) {
    size_t len = strlen(rel);
    if (len == 0 || rel[0] == '/') {
        mp_err_set(e, "ruta no válida (vacía o absoluta): %s", rel);
        return -1;
    }
    if (APP0_LEN + len >= 1024) {
        mp_err_set(e, "ruta demasiado larga para la PS5 (1024 bytes con /app0/): %s", rel);
        return -1;
    }
    const char *part = rel;
    for (const char *p = rel;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || c == '\t' || c == '\n' || c == '\r') {
            mp_err_set(e, "ruta con un carácter no admitido (\\, tabulador o salto de línea): %s", rel);
            return -1;
        }
        if (c == '/' || c == 0) {
            size_t k = (size_t)(p - part);
            if (k == 0 || (k == 1 && part[0] == '.') || (k == 2 && part[0] == '.' && part[1] == '.')) {
                mp_err_set(e, "ruta con una parte vacía, '.' o '..': %s", rel);
                return -1;
            }
            if (k > 255) {
                mp_err_set(e, "nombre de más de 255 bytes en: %s", rel);
                return -1;
            }
            if (!c) break;
            part = p + 1;
        }
    }
    return 0;
}

typedef struct {
    const mp_idx3_row *r;
} srow;

static int cmp_row(const void *a, const void *b) {
    return fold_cmp(((const srow *)a)->r->path, ((const srow *)b)->r->path);
}

static uint64_t slot_count(size_t n) {
    uint64_t s = 2;
    while (s < 2 * (uint64_t)n) s <<= 1;
    return s;
}

static uint64_t row_hash(const char *rel) {
    // Hash de "/app0/" + rel sin construir la cadena: misma cuenta byte a byte.
    uint64_t h = MP_FNV_BASIS_IDX3;
    for (const char *p = APP0; *p; p++) {
        h ^= (unsigned char)fold((unsigned char)*p);
        h *= 1099511628211ULL;
    }
    for (const unsigned char *p = (const unsigned char *)rel; *p; p++) {
        h ^= (unsigned char)fold(*p);
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

int mp_idx3_build(const mp_idx3_row *rows, size_t n, uint8_t **out, size_t *out_len, mp_err *e) {
    *out = NULL;
    *out_len = 0;
    if (n == 0 || n > 0xFFFFFFFEu) {
        mp_err_set(e, "ampr_emu.index: número de archivos no válido (%zu)", n);
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        if (mp_pack_path_check(rows[i].path, e) != 0) return -1;
    srow *v = malloc(n * sizeof *v);
    if (!v) {
        mp_err_set(e, "sin memoria");
        return -1;
    }
    for (size_t i = 0; i < n; i++) v[i].r = &rows[i];
    qsort(v, n, sizeof *v, cmp_row);
    uint64_t blob = 0;
    for (size_t i = 0; i < n; i++) {
        if (i && fold_cmp(v[i - 1].r->path, v[i].r->path) == 0) {
            mp_err_set(e, "dos rutas que solo se diferencian en mayúsculas: %s y %s (el runtime no "
                          "las distingue)", v[i - 1].r->path, v[i].r->path);
            free(v);
            return -1;
        }
        blob += APP0_LEN + strlen(v[i].r->path) + 1;
    }
    uint64_t path_end = IDX3_HEADER + (uint64_t)IDX3_RECORD * n + blob;
    uint64_t hash_off = (path_end + IDX3_SLOT - 1) & ~(uint64_t)(IDX3_SLOT - 1);
    uint64_t slots = slot_count(n), total = hash_off + IDX3_SLOT * slots;
    if (blob > 0xFFFFFFFFu || total > ((uint64_t)SIZE_MAX >> 1)) {
        mp_err_set(e, "ampr_emu.index demasiado grande");
        free(v);
        return -1;
    }
    uint8_t *d = calloc(1, (size_t)total);
    if (!d) {
        free(v);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    memcpy(d, "AMPRIDX3", 8);
    wr32(d + 8, 3);
    wr32(d + 12, IDX3_RECORD);
    wr64(d + 16, n);
    wr64(d + 24, blob);
    wr64(d + 32, hash_off);
    wr32(d + 40, IDX3_SLOT);
    wr32(d + 44, (uint32_t)slots);
    uint8_t *rec = d + IDX3_HEADER, *bp = d + IDX3_HEADER + (size_t)IDX3_RECORD * n;
    uint32_t off = 0;
    for (size_t i = 0; i < n; i++) {
        size_t pl = strlen(v[i].r->path);
        wr32(rec, off);
        wr32(rec + 4, (uint32_t)(APP0_LEN + pl));
        wr64(rec + 8, v[i].r->size);
        wr64(rec + 16, (uint64_t)v[i].r->mtime);
        memcpy(bp + off, APP0, APP0_LEN);
        memcpy(bp + off + APP0_LEN, v[i].r->path, pl);
        off += (uint32_t)(APP0_LEN + pl + 1);   // el NUL ya está por calloc
        rec += IDX3_RECORD;
    }
    // Direccionamiento abierto lineal, como build_hash_slots: un hash repetido marca flag 1 en el
    // slot que ya estaba y también en el nuevo.
    uint8_t *sl = d + hash_off;
    uint64_t mask = slots - 1;
    for (size_t i = 0; i < n; i++) {
        uint64_t h = row_hash(v[i].r->path), pos = h & mask;
        int dup = 0;
        while (rd32(sl + IDX3_SLOT * pos + 8) != 0) {
            uint8_t *s = sl + IDX3_SLOT * pos;
            if (rd64(s) == h) {
                wr32(s + 12, rd32(s + 12) | 1u);
                dup = 1;
            }
            pos = (pos + 1) & mask;
        }
        uint8_t *s = sl + IDX3_SLOT * pos;
        wr64(s, h);
        wr32(s + 8, (uint32_t)i + 1);
        wr32(s + 12, dup ? 1u : 0u);
    }
    free(v);
    *out = d;
    *out_len = (size_t)total;
    return 0;
}

static int bad(mp_err *e, const char *why) {
    mp_err_set(e, "ampr_emu.index no válido: %s", why);
    return -1;
}

long mp_idx3_find(const mp_idx3_entry *v, size_t n, const char *rel) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *p = v[mid].app0_path;
        int c = strncmp(p, APP0, APP0_LEN) ? strcmp(p, APP0) : fold_cmp(p + APP0_LEN, rel);
        if (c == 0) return (long)mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

int mp_idx3_set_row(const uint8_t *idx, size_t n, const char *rel, uint64_t size, int64_t mtime,
                    uint8_t **out, size_t *out_len, mp_err *e) {
    *out = NULL;
    *out_len = 0;
    mp_idx3_entry *v = NULL;
    size_t m = 0;
    if (mp_idx3_parse(idx, n, &v, &m, e) != 0) return -1;
    mp_idx3_row *rows = malloc((m + 1) * sizeof *rows);
    if (!rows) {
        mp_idx3_entries_free(v, m);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    size_t k = 0;
    int found = 0;
    for (size_t i = 0; i < m; i++) {
        rows[k].path = v[i].app0_path + APP0_LEN;
        rows[k].size = v[i].size;
        rows[k].mtime = v[i].mtime;
        if (fold_cmp(rows[k].path, rel) == 0) {   // el runtime no distingue mayúsculas
            rows[k].path = rel;
            rows[k].size = size;
            rows[k].mtime = mtime;
            found = 1;
        }
        k++;
    }
    if (!found) {
        rows[k].path = rel;
        rows[k].size = size;
        rows[k].mtime = mtime;
        k++;
    }
    int rc = mp_idx3_build(rows, k, out, out_len, e);
    free(rows);
    mp_idx3_entries_free(v, m);
    return rc;
}

void mp_idx3_entries_free(mp_idx3_entry *v, size_t n) {
    if (!v) return;
    for (size_t i = 0; i < n; i++) free(v[i].app0_path);
    free(v);
}

int mp_idx3_parse(const uint8_t *d, size_t n, mp_idx3_entry **out, size_t *count, mp_err *e) {
    *out = NULL;
    *count = 0;
    if (n < IDX3_HEADER || memcmp(d, "AMPRIDX3", 8) != 0) return bad(e, "cabecera");
    if (rd32(d + 8) != 3 || rd32(d + 12) != IDX3_RECORD || rd32(d + 40) != IDX3_SLOT)
        return bad(e, "versión o tamaños de registro");
    uint64_t rows = rd64(d + 16), blob = rd64(d + 24), hash_off = rd64(d + 32), slots = rd32(d + 44);
    if (rows == 0 || rows > 0xFFFFFFFEu || rows > n / IDX3_RECORD) return bad(e, "número de filas");
    uint64_t path_end = IDX3_HEADER + IDX3_RECORD * rows + blob;
    if (blob > n || path_end > hash_off || hash_off % IDX3_SLOT_ALIGN || slots < 2 || (slots & (slots - 1)) ||
        slots < 2 * rows || hash_off > n || n - hash_off != IDX3_SLOT * slots)   // sin dar la vuelta
        return bad(e, "posición o tamaño de las tablas");
    const uint8_t *bp = d + IDX3_HEADER + IDX3_RECORD * rows;
    mp_idx3_entry *v = calloc((size_t)rows, sizeof *v);
    if (!v) {
        mp_err_set(e, "sin memoria");
        return -1;
    }
    int rc = 0;
    for (uint64_t i = 0; i < rows && !rc; i++) {
        const uint8_t *r = d + IDX3_HEADER + IDX3_RECORD * i;
        uint32_t off = rd32(r), len = rd32(r + 4);
        if ((uint64_t)off + len >= blob || bp[off + len] != 0 || memchr(bp + off, 0, len)) {
            rc = bad(e, "ruta fuera del blob o sin NUL");
            break;
        }
        v[i].app0_path = malloc(len + 1u);
        if (!v[i].app0_path) {
            mp_err_set(e, "sin memoria");
            rc = -1;
            break;
        }
        memcpy(v[i].app0_path, bp + off, len);
        v[i].app0_path[len] = 0;
        v[i].size = rd64(r + 8);
        v[i].mtime = (int64_t)rd64(r + 16);
        if (len <= APP0_LEN || memcmp(v[i].app0_path, APP0, APP0_LEN) != 0 ||
            mp_pack_path_check(v[i].app0_path + APP0_LEN, e) != 0) {
            rc = -1;
            if (!e || strncmp(e->msg, "ampr_emu", 8)) bad(e, "ruta que no empieza por /app0/ o no válida");
            break;
        }
        if (i && fold_cmp(v[i - 1].app0_path, v[i].app0_path) >= 0) {
            rc = bad(e, "filas desordenadas o repetidas sin mayúsculas");
            break;
        }
    }
    // Cada fila tiene que estar en la tabla hash.
    const uint8_t *sl = d + hash_off;
    for (uint64_t i = 0; i < rows && !rc; i++) {
        uint64_t h = mp_idx3_path_hash(v[i].app0_path), pos = h & (slots - 1);
        int found = 0;
        for (uint64_t k = 0; k < slots && !found; k++, pos = (pos + 1) & (slots - 1)) {
            const uint8_t *s = sl + IDX3_SLOT * pos;
            if (rd32(s + 8) == 0) break;
            found = rd64(s) == h && rd32(s + 8) == i + 1;
        }
        if (!found) rc = bad(e, "una fila no está en la tabla hash");
    }
    if (rc) {
        mp_idx3_entries_free(v, (size_t)rows);
        return -1;
    }
    *out = v;
    *count = (size_t)rows;
    return 0;
}
