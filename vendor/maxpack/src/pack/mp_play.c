// SPDX-License-Identifier: GPL-3.0-or-later
// Jugar un .p5z sin instalarlo (ver mp_play.h). No usa mp_cancel_check: el indicador de cancelar
// es de la línea de comandos. Cada mp_play es de un solo hilo.
#include "mp_play.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_p5z_internal.h"

struct mp_play {
    mp_p5z *p5z;
    mp_zpk *zpk;
};

static void put_err(char *err, size_t errlen, const mp_err *e, const char *fallback) {
    if (!err || !errlen) return;
    snprintf(err, errlen, "%s", e && e->msg[0] ? e->msg : fallback);
}

static int is_loose_role(int role) {
    return role == MP_ROLE_LOOSE || role == MP_ROLE_ZPK_INDEX || role == MP_ROLE_IDX3;
}

typedef struct {
    uint8_t *b;
    size_t n, cap;
} membuf;

static int to_mem(void *ctx, const void *p, size_t n, mp_err *e) {
    membuf *m = ctx;
    if (m->n + n > m->cap) {
        size_t cap = (m->n + n) * 2;
        uint8_t *b = realloc(m->b, cap);
        if (!b) {
            mp_err_set(e, "sin memoria");
            return -1;
        }
        m->b = b;
        m->cap = cap;
    }
    memcpy(m->b + m->n, p, n);
    m->n += n;
    return 0;
}

static int to_wfile(void *ctx, const void *p, size_t n, mp_err *e) { return mp_wfile_write(ctx, p, n, e); }

mp_play *mp_play_open(const char *p5z, char *err, size_t errlen) {
    mp_err e = {{0}};
    mp_play *p = calloc(1, sizeof *p);
    if (!p) {
        put_err(err, errlen, NULL, "sin memoria");
        return NULL;
    }
    if (!p5z || !*p5z) {
        put_err(err, errlen, NULL, "falta la ruta del .p5z");
        goto fail;
    }
    if (mp_p5z_open(p5z, &p->p5z, &e) != 0 || mp_p5z_zpk(p->p5z, &p->zpk, &e) != 0) {
        put_err(err, errlen, &e, ".p5z no válido");
        goto fail;
    }
    // El archive_id de la cabecera del .p5z debe ser el de ampr_zstd.index (bytes 24..40).
    size_t i = 0;
    while (i < p->p5z->nfiles && p->p5z->files[i].role != MP_ROLE_ZPK_INDEX) i++;
    membuf m = {0};
    if (i == p->p5z->nfiles || p5z_read_file(p->p5z, i, to_mem, &m, &e) != 0 || m.n < 40) {
        free(m.b);
        put_err(err, errlen, &e, ".p5z no válido: ampr_zstd.index");
        goto fail;
    }
    int same = memcmp(m.b + 24, p->p5z->man.archive_id, 16) == 0;
    free(m.b);
    if (!same) {
        put_err(err, errlen, NULL, ".p5z no válido: el archive_id de la cabecera no es el de ampr_zstd.index");
        goto fail;
    }
    return p;
fail:
    mp_play_close(p);
    return NULL;
}

void mp_play_close(mp_play *p) {
    if (!p) return;
    mp_zpk_close(p->zpk);
    mp_p5z_close(p->p5z);
    free(p);
}

void mp_play_free_str(char *s) { free(s); }

// ---- JSON -----------------------------------------------------------------------------------

typedef struct {
    char *b;
    size_t n, cap;
    int oom;
} sbuf;

static void sb_raw(sbuf *s, const char *t, size_t n) {
    if (s->oom) return;
    if (s->n + n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 512;
        while (cap < s->n + n + 1) cap *= 2;
        char *b = realloc(s->b, cap);
        if (!b) {
            s->oom = 1;
            return;
        }
        s->b = b;
        s->cap = cap;
    }
    memcpy(s->b + s->n, t, n);
    s->n += n;
    s->b[s->n] = 0;
}

static void sb_str(sbuf *s, const char *t) { sb_raw(s, t, strlen(t)); }

static void sb_u64(sbuf *s, uint64_t v) {
    char t[32];
    snprintf(t, sizeof t, "%llu", (unsigned long long)v);
    sb_str(s, t);
}

static void sb_json(sbuf *s, const char *t) {
    sb_raw(s, "\"", 1);
    for (const unsigned char *c = (const unsigned char *)t; *c; c++) {
        if (*c == '"' || *c == '\\') {
            char q[2] = {'\\', (char)*c};
            sb_raw(s, q, 2);
        } else if (*c < 0x20 || *c == 0x7f) {
            char q[8];
            snprintf(q, sizeof q, "\\u%04x", *c);
            sb_str(s, q);
        } else {
            sb_raw(s, (const char *)c, 1);
        }
    }
    sb_raw(s, "\"", 1);
}

typedef struct {
    unsigned id;
    uint64_t off, len;
} vol;

static int vol_cmp(const void *a, const void *b) {
    unsigned x = ((const vol *)a)->id, y = ((const vol *)b)->id;
    return x < y ? -1 : x > y;
}

char *mp_play_json(mp_play *p) {
    const mp_p5z *z = p->p5z;
    sbuf s = {0};
    uint64_t loose_bytes = 0;
    size_t nvol = 0;
    for (size_t i = 0; i < z->nfiles; i++) {
        if (is_loose_role(z->files[i].role)) loose_bytes += z->files[i].size;
        if (z->files[i].role == MP_ROLE_VOLUME) nvol++;
    }
    vol *v = calloc(nvol ? nvol : 1, sizeof *v);
    if (!v) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < z->nfiles; i++) {
        const p5z_file *f = &z->files[i];
        unsigned id = 0;
        if (f->role != MP_ROLE_VOLUME) continue;
        if (sscanf(f->path, "ampr_zstd-%u.pak", &id) != 1) id = (unsigned)k;
        v[k].id = id;
        v[k].off = f->data_off;
        v[k].len = f->size;
        k++;
    }
    qsort(v, nvol, sizeof *v, vol_cmp);

    sb_str(&s, "{\"title_id\":");
    sb_json(&s, z->man.title_id);
    sb_str(&s, ",\"version\":");
    sb_json(&s, z->man.content_version);
    sb_str(&s, ",\"name\":");
    sb_json(&s, z->man.title_name);
    sb_str(&s, ",\"archive_id\":\"");
    for (int i = 0; i < 16; i++) {
        char h[3];
        snprintf(h, sizeof h, "%02x", z->man.archive_id[i]);
        sb_str(&s, h);
    }
    sb_str(&s, "\",\"size\":");
    sb_u64(&s, z->size);
    sb_str(&s, ",\"loose_bytes\":");
    sb_u64(&s, loose_bytes);
    sb_str(&s, ",\"loose\":[");
    int first = 1;
    for (size_t i = 0; i < z->nfiles; i++) {
        if (!is_loose_role(z->files[i].role)) continue;
        if (!first) sb_str(&s, ",");
        first = 0;
        sb_str(&s, "{\"path\":");
        sb_json(&s, z->files[i].path);
        sb_str(&s, ",\"size\":");
        sb_u64(&s, z->files[i].size);
        sb_str(&s, "}");
    }
    sb_str(&s, "],\"volumes\":[");
    for (size_t i = 0; i < nvol; i++) {
        if (i) sb_str(&s, ",");
        sb_str(&s, "{\"id\":");
        sb_u64(&s, v[i].id);
        sb_str(&s, ",\"offset\":");
        sb_u64(&s, v[i].off);
        sb_str(&s, ",\"length\":");
        sb_u64(&s, v[i].len);
        sb_str(&s, "}");
    }
    sb_str(&s, "]}");
    free(v);
    if (s.oom) {
        free(s.b);
        return NULL;
    }
    return s.b;
}

// ---- extracción -----------------------------------------------------------------------------

int mp_play_extract(mp_play *p, const char *rel, const char *dest, char *err, size_t errlen) {
    mp_err e = {{0}};
    mp_p5z *z = p->p5z;
    if (!rel || !dest || !*dest) {
        put_err(err, errlen, NULL, "faltan la ruta o el destino");
        return -1;
    }
    size_t fi = z->nfiles;
    for (size_t i = 0; i < z->nfiles; i++)
        if (is_loose_role(z->files[i].role) && strcmp(z->files[i].path, rel) == 0) {
            fi = i;
            break;
        }
    if (fi == z->nfiles) {
        mp_err_set(&e, "%s no es un archivo suelto del .p5z", rel);
        put_err(err, errlen, &e, "no es un archivo suelto del .p5z");
        return -1;
    }
    char *dir = mp_strdup(dest);
    if (!dir) {
        put_err(err, errlen, NULL, "sin memoria");
        return -1;
    }
    char *sl = strrchr(dir, '/');
    char *bs = strrchr(dir, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
    int rc = 0;
    if (sl && sl > dir) {
        *sl = 0;
        rc = mp_make_dirs(dir, &e);
    }
    free(dir);
    if (rc == 0) rc = mp_remove_tree(dest, &e);
    mp_wfile *w = NULL;
    if (rc == 0) rc = mp_wfile_create(dest, &w, &e);
    if (rc == 0) rc = p5z_read_file(z, fi, to_wfile, w, &e);
    if (w && mp_wfile_close(w, rc == 0 ? &e : NULL) != 0) rc = -1;
    if (rc == 0 && z->files[fi].mtime) rc = mp_set_mtime(dest, z->files[fi].mtime, &e);
    if (rc != 0) {
        put_err(err, errlen, &e, "no se pudo extraer el archivo");
        return -1;
    }
    return 0;
}
