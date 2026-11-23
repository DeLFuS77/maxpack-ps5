// SPDX-License-Identifier: GPL-3.0-or-later
// Lector de imágenes exFAT (solo lectura). Carga la FAT entera y todos los directorios al abrir;
// los archivos se leen después por tramos contiguos (extents).
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "mp_extent.h"
#include "mp_platform.h"
#include "mp_source_impl.h"

#define EXFAT_MAX_DEPTH 64
#define EXFAT_MAX_DIR (256ull << 20)   // límite de exFAT para el tamaño de una carpeta

typedef struct {
    mp_extent *ex;
    size_t nex;
    uint64_t vdl;
} exfat_file;

typedef struct {
    mp_file *img;
    uint64_t img_size, cs, heap_off;
    uint32_t count, root;
    uint32_t *fat;      // count + 2 entradas
    uint8_t *used;      // un bit por clúster: detecta bucles y cruces
    exfat_file *files;  // en paralelo a mp_source.entries
    const char *img_path;
} exfat;

typedef struct {
    mp_entry en;
    exfat_file f;
} rec;

typedef struct {
    rec *v;
    size_t n, cap, ndirs;
} recs;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

// Marca de tiempo exFAT (formato DOS + centésimas + desplazamiento UTC) -> segundos Unix.
static int64_t exfat_time(uint32_t ts, uint8_t ms10, uint8_t utc) {
    unsigned day = (ts >> 16) & 0x1F, mon = (ts >> 21) & 0x0F;
    if (day == 0 || mon == 0 || mon > 12) return 0;
    int64_t t = days_from_civil(1980 + (ts >> 25), mon, day) * 86400 + ((ts >> 11) & 0x1F) * 3600 +
                ((ts >> 5) & 0x3F) * 60 + (ts & 0x1F) * 2 + ms10 / 100;
    if (utc & 0x80) {
        int ofs = utc & 0x7F;
        if (ofs & 0x40) ofs -= 0x80;
        t -= (int64_t)ofs * 15 * 60;   // la hora guardada es local = UTC + desplazamiento
    }
    return t;
}

static char *utf16_to_utf8(const uint16_t *u, size_t n) {
    char *out = malloc(n * 3 + 1), *o = out;
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00u);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            *o++ = (char)c;
        } else if (c < 0x800) {
            *o++ = (char)(0xC0 | c >> 6);
            *o++ = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            *o++ = (char)(0xE0 | c >> 12);
            *o++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *o++ = (char)(0x80 | (c & 0x3F));
        } else {
            *o++ = (char)(0xF0 | c >> 18);
            *o++ = (char)(0x80 | ((c >> 12) & 0x3F));
            *o++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *o++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *o = 0;
    return out;
}

static uint64_t cluster_off(const exfat *x, uint32_t c) { return x->heap_off + (uint64_t)(c - 2) * x->cs; }

static int bad_chain(mp_err *e, const char *path, const char *why) {
    mp_err_set(e, "cadena FAT inválida en %s (%s)", path[0] ? path : "/", why);
    return -1;
}

// Marca un clúster como usado; error si está fuera de rango o ya lo usaba otro archivo.
static int take_cluster(exfat *x, uint32_t c, const char *path, mp_err *e) {
    if (c < 2 || c > x->count + 1) return bad_chain(e, path, "clúster fuera de rango");
    uint32_t k = c - 2;
    if (x->used[k >> 3] & (1u << (k & 7))) return bad_chain(e, path, "clúster repetido o bucle");
    x->used[k >> 3] |= (uint8_t)(1u << (k & 7));
    return 0;
}

// Construye los tramos de una asignación. Con has_len, son exactamente ceil(len / cs) clústeres;
// sin longitud (directorio raíz) se sigue la cadena hasta la marca de fin.
static int build_extents(exfat *x, uint32_t first, int nofat, int has_len, uint64_t len,
                         const char *path, mp_extent **out, size_t *nout, mp_err *e) {
    *out = NULL;
    *nout = 0;
    // Antes de redondear: con un DataLength cercano a 2^64 el redondeo daría la vuelta a 0.
    if (has_len && len > (uint64_t)x->count * x->cs) return bad_chain(e, path, "más grande que el volumen");
    uint64_t need = has_len ? (len + x->cs - 1) / x->cs : 0;
    if (has_len && need == 0) return 0;
    if (has_len && need > x->count) return bad_chain(e, path, "más grande que el volumen");
    size_t cap = 4, n = 0;
    mp_extent *ex = malloc(cap * sizeof *ex);
    if (!ex) {
        mp_err_set(e, "sin memoria");
        return -1;
    }
    uint64_t got = 0;
    uint32_t c = first;
    for (;;) {
        if (take_cluster(x, c, path, e) != 0) goto fail;
        if (n && ex[n - 1].img_off + ex[n - 1].len == cluster_off(x, c)) {
            ex[n - 1].len += x->cs;
        } else {
            if (n == cap) {
                mp_extent *g = realloc(ex, (cap *= 2) * sizeof *ex);
                if (!g) {
                    mp_err_set(e, "sin memoria");
                    goto fail;
                }
                ex = g;
            }
            ex[n].file_off = got * x->cs;
            ex[n].img_off = cluster_off(x, c);
            ex[n].len = x->cs;
            n++;
        }
        got++;
        if (has_len && got == need) break;
        if (!has_len && got * x->cs >= EXFAT_MAX_DIR) {
            mp_err_set(e, "carpeta raíz demasiado grande (cadena de más de 256 MiB)");
            goto fail;
        }
        if (nofat) {
            c++;
            continue;
        }
        uint32_t next = x->fat[c];
        if (next >= 0xFFFFFFF8u) {
            if (has_len) {
                bad_chain(e, path, "termina antes de tiempo");
                goto fail;
            }
            break;
        }
        if (next == 0xFFFFFFF7u) {
            bad_chain(e, path, "clúster marcado como defectuoso");
            goto fail;
        }
        c = next;
    }
    *out = ex;
    *nout = n;
    return 0;
fail:
    free(ex);
    return -1;
}

static int read_alloc(exfat *x, const mp_extent *ex, size_t n, uint64_t len, uint8_t **out,
                      mp_err *e) {
    uint8_t *buf = malloc(len ? (size_t)len : 1);
    if (!buf) {
        mp_err_set(e, "sin memoria");
        return -1;
    }
    uint64_t pos = 0;
    for (size_t i = 0; i < n && pos < len; i++) {
        size_t take = (size_t)(ex[i].len < len - pos ? ex[i].len : len - pos), got = 0;
        if (mp_file_pread(x->img, buf + pos, take, ex[i].img_off, &got, e) != 0) goto fail;
        if (got != take) {
            mp_err_set(e, "imagen exFAT truncada: %s", x->img_path);
            goto fail;
        }
        pos += take;
    }
    *out = buf;
    return 0;
fail:
    free(buf);
    return -1;
}

static int push_rec(recs *r, rec v) {
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 256;
        rec *g = realloc(r->v, cap * sizeof *g);
        if (!g) return -1;
        r->v = g;
        r->cap = cap;
    }
    r->v[r->n++] = v;
    return 0;
}

static char *join(const char *prefix, const char *name) {
    size_t a = strlen(prefix), b = strlen(name);
    char *p = malloc(a + b + 2);
    if (!p) return NULL;
    memcpy(p, prefix, a);
    if (a) p[a++] = '/';
    memcpy(p + a, name, b + 1);
    return p;
}

static int parse_dir(exfat *x, const uint8_t *d, uint64_t len, const char *prefix, int depth,
                     recs *r, mp_err *e) {
    if (depth > EXFAT_MAX_DEPTH) {
        mp_err_set(e, "demasiados niveles de carpetas en %s", prefix);
        return -1;
    }
    uint64_t i = 0;
    while (i + 32 <= len) {
        uint8_t t = d[i];
        if (t == 0x00) break;
        if (t != 0x85) {
            i += 32;
            continue;
        }
        unsigned sec = d[i + 1];
        const uint8_t *st = d + i + 32;
        if (sec < 2 || sec > 18 || i + 32ull * (sec + 1) > len || st[0] != 0xC0) {
            mp_err_set(e, "entrada de directorio no válida en %s", prefix[0] ? prefix : "/");
            return -1;
        }
        unsigned nlen = st[3];
        if (nlen == 0 || (nlen + 14) / 15 > sec - 1) {
            mp_err_set(e, "nombre no válido en %s", prefix[0] ? prefix : "/");
            return -1;
        }
        uint16_t units[255];
        for (unsigned k = 0; k < nlen; k++) {
            const uint8_t *ne = d + i + 64 + 32ull * (k / 15);
            if (ne[0] != 0xC1) {
                mp_err_set(e, "nombre no válido en %s", prefix[0] ? prefix : "/");
                return -1;
            }
            units[k] = rd16(ne + 2 + 2 * (k % 15));
        }
        char *name = utf16_to_utf8(units, nlen);
        char *path = name ? join(prefix, name) : NULL;
        int bad_name = name && (strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, ".."));
        free(name);
        if (!path) {
            mp_err_set(e, "sin memoria");
            return -1;
        }
        if (bad_name) {
            mp_err_set(e, "nombre no válido: %s", path);
            free(path);
            return -1;
        }
        uint16_t attr = rd16(d + i + 4);
        int nofat = (st[1] & 0x02) != 0;
        uint64_t vdl = rd64(st + 8), dl = rd64(st + 24);
        uint32_t first = rd32(st + 20);
        int64_t mtime = exfat_time(rd32(d + i + 12), d[i + 21], d[i + 23]);
        if ((attr & 0x10) && dl > EXFAT_MAX_DIR) {
            mp_err_set(e, "carpeta demasiado grande: %s (más de 256 MiB)", path);
            free(path);
            return -1;
        }
        mp_extent *ex = NULL;
        size_t nex = 0;
        if (build_extents(x, first, nofat, 1, dl, path, &ex, &nex, e) != 0) {
            free(path);
            return -1;
        }
        if (attr & 0x10) {
            r->ndirs++;
            uint8_t *sub = NULL;
            int rc = read_alloc(x, ex, nex, dl, &sub, e);
            free(ex);
            if (rc == 0) rc = parse_dir(x, sub, dl, path, depth + 1, r, e);
            free(sub);
            free(path);
            if (rc != 0) return -1;
        } else {
            rec v = {{path, dl, mtime}, {ex, nex, vdl < dl ? vdl : dl}};
            if (push_rec(r, v) != 0) {
                free(ex);
                free(path);
                mp_err_set(e, "sin memoria");
                return -1;
            }
        }
        i += 32ull * (sec + 1);
    }
    return 0;
}

static int exfat_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e) {
    exfat *x = s->impl;
    const exfat_file *f = &x->files[i];
    uint8_t *p = buf;
    while (len) {
        if (off >= f->vdl) {   // más allá de ValidDataLength el contenido es cero
            memset(p, 0, len);
            return 0;
        }
        uint64_t img = 0, run = 0;
        if (mp_extent_find(f->ex, f->nex, off, &img, &run) < 0) {
            mp_err_set(e, "%s: desplazamiento sin clúster asignado", s->entries[i].path);
            return -1;
        }
        uint64_t lim = f->vdl - off < run ? f->vdl - off : run;
        size_t take = len < lim ? len : (size_t)lim, got = 0;
        if (mp_file_pread(x->img, p, take, img, &got, e) != 0) return -1;
        if (got != take) {
            mp_err_set(e, "imagen exFAT truncada: falta el contenido de %s", s->entries[i].path);
            return -1;
        }
        p += take;
        off += take;
        len -= take;
    }
    return 0;
}

static void exfat_free(exfat *x, size_t nfiles) {
    if (!x) return;
    if (x->files)
        for (size_t i = 0; i < nfiles; i++) free(x->files[i].ex);
    free(x->files);
    free(x->fat);
    free(x->used);
    mp_file_close(x->img);
    free(x);
}

static void exfat_close(mp_source *s) { exfat_free(s->impl, s->n); }

static int cmp_rec(const void *a, const void *b) {
    return strcmp(((const rec *)a)->en.path, ((const rec *)b)->en.path);
}

static int load_boot(exfat *x, const char *path, mp_err *e) {
    uint8_t b[512];
    size_t got = 0;
    if (mp_file_size(x->img, &x->img_size, e) != 0) return -1;
    if (x->img_size < 512) {
        mp_err_set(e, "%s: imagen exFAT truncada (%" PRIu64 " bytes)", path, x->img_size);
        return -1;
    }
    if (mp_file_pread(x->img, b, 512, 0, &got, e) != 0) return -1;
    unsigned bps = b[108], spc = b[109], nfats = b[110];
    uint64_t vol_len = rd64(b + 72);
    uint32_t fat_off = rd32(b + 80), fat_len = rd32(b + 84), heap = rd32(b + 88);
    x->count = rd32(b + 92);
    x->root = rd32(b + 96);
    const char *why = NULL;
    if (b[510] != 0x55 || b[511] != 0xAA || memcmp(b + 3, "EXFAT   ", 8) != 0)
        why = "firma";
    else if (bps < 9 || bps > 12 || bps + spc > 25)
        why = "tamaño de sector o de clúster";
    else if (nfats != 1 && nfats != 2)
        why = "número de FAT";
    else if (x->count == 0 || x->count > 0xFFFFFFF5u)
        why = "número de clústeres";
    else if (fat_off < 24 || (uint64_t)fat_len << bps < ((uint64_t)x->count + 2) * 4 ||
             heap < (uint64_t)fat_off + (uint64_t)fat_len * nfats)
        why = "posición de la FAT o del heap";
    else if (vol_len < heap + ((uint64_t)x->count << spc))
        why = "longitud del volumen";
    else if (x->root < 2 || x->root > x->count + 1)
        why = "clúster del directorio raíz";
    if (why) {
        mp_err_set(e, "%s: cabecera exFAT no válida (%s)", path, why);
        return -1;
    }
    x->cs = 1ull << (bps + spc);
    x->heap_off = (uint64_t)heap << bps;
    uint64_t fat_bytes = ((uint64_t)x->count + 2) * 4, fat_pos = (uint64_t)fat_off << bps;
    if (fat_pos + fat_bytes > x->img_size) {
        mp_err_set(e, "%s: imagen exFAT truncada (la FAT no cabe)", path);
        return -1;
    }
    uint8_t *raw = malloc((size_t)fat_bytes);
    x->fat = malloc((size_t)fat_bytes);
    x->used = calloc(((size_t)x->count + 7) / 8 + 1, 1);
    if (!raw || !x->fat || !x->used) {
        free(raw);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    if (mp_file_pread(x->img, raw, (size_t)fat_bytes, fat_pos, &got, e) != 0 || got != fat_bytes) {
        free(raw);
        if (!e || !e->msg[0]) mp_err_set(e, "%s: imagen exFAT truncada (FAT)", path);
        return -1;
    }
    for (uint64_t i = 0; i < (uint64_t)x->count + 2; i++) x->fat[i] = rd32(raw + 4 * i);
    free(raw);
    return 0;
}

int mp_exfat_open(const char *path, mp_source **out, mp_err *e) {
    *out = NULL;
    exfat *x = calloc(1, sizeof *x);
    recs r = {0};
    mp_source *s = NULL;
    if (!x) {
        mp_err_set(e, "sin memoria");
        return -1;
    }
    x->img_path = path;
    int rc = mp_file_open_read(path, &x->img, e);
    if (rc == 0) rc = load_boot(x, path, e);
    if (rc == 0) {
        mp_extent *ex = NULL;
        size_t nex = 0;
        uint8_t *root = NULL;
        rc = build_extents(x, x->root, 0, 0, 0, "", &ex, &nex, e);
        uint64_t rlen = 0;
        for (size_t i = 0; i < nex; i++) rlen += ex[i].len;
        if (rc == 0) rc = read_alloc(x, ex, nex, rlen, &root, e);
        if (rc == 0) rc = parse_dir(x, root, rlen, "", 0, &r, e);
        free(ex);
        free(root);
    }
    if (rc == 0) {
        s = calloc(1, sizeof *s);
        x->files = malloc((r.n ? r.n : 1) * sizeof *x->files);
        mp_entry *entries = malloc((r.n ? r.n : 1) * sizeof *entries);
        if (!s || !x->files || !entries) {
            free(entries);
            mp_err_set(e, "sin memoria");
            rc = -1;
        } else {
            if (r.n > 1) qsort(r.v, r.n, sizeof *r.v, cmp_rec);
            for (size_t i = 0; i < r.n; i++) {
                entries[i] = r.v[i].en;
                x->files[i] = r.v[i].f;
            }
            s->kind = MP_SRC_EXFAT;
            s->entries = entries;
            s->n = r.n;
            s->ndirs = r.ndirs;
            s->read = exfat_read;
            s->close = exfat_close;
            s->impl = x;
            x->img_path = NULL;   // la ruta del llamante no vive más allá de esta función
            free(r.v);
            *out = s;
            return 0;
        }
    }
    for (size_t i = 0; i < r.n; i++) {
        free(r.v[i].en.path);
        free(r.v[i].f.ex);
    }
    free(r.v);
    free(s);
    exfat_free(x, 0);
    return -1;
}
