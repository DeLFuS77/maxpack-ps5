// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_rules.h"

#include <stdlib.h>
#include <string.h>

#include "mp_platform.h"

struct mp_rules {
    char **keep;
    size_t n;
};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ieq_n(const char *a, size_t an, const char *b) {
    size_t bn = strlen(b);
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return 0;
    return 1;
}

static int istarts_n(const char *a, size_t an, const char *prefix) {
    size_t pn = strlen(prefix);
    return an >= pn && ieq_n(a, pn, prefix);
}

// Un segmento con '*' y '?' (sin '/').
static int seg_match(const char *p, const char *pe, const char *s, const char *se) {
    while (p < pe) {
        if (*p == '*') {
            while (p < pe && *p == '*') p++;
            if (p == pe) return 1;
            for (const char *t = s; t <= se; t++)
                if (seg_match(p, pe, t, se)) return 1;
            return 0;
        }
        if (s == se) return 0;
        if (*p != '?' && lower((unsigned char)*p) != lower((unsigned char)*s)) return 0;
        p++;
        s++;
    }
    return s == se;
}

static const char *seg_end(const char *s) {
    const char *e = strchr(s, '/');
    return e ? e : s + strlen(s);
}

int mp_glob_match(const char *g, const char *p) {
    if (!*g) return !*p;
    const char *ge = seg_end(g), *gnext = *ge ? ge + 1 : ge;
    if (ge - g == 2 && g[0] == '*' && g[1] == '*') {
        if (mp_glob_match(gnext, p)) return 1;   // '**' = cero segmentos
        if (!*p) return 0;
        const char *pe = seg_end(p);
        return mp_glob_match(g, *pe ? pe + 1 : pe);   // '**' se come un segmento más
    }
    if (!*p) return 0;
    const char *pe = seg_end(p);
    if (!seg_match(g, ge, p, pe)) return 0;
    return mp_glob_match(gnext, *pe ? pe + 1 : pe);
}

mp_rules *mp_rules_new(void) { return calloc(1, sizeof(mp_rules)); }

int mp_rules_add_keep_loose(mp_rules *r, const char *glob, mp_err *e) {
    if (!glob || !*glob) {
        mp_err_set(e, "--keep-loose necesita un patrón");
        return -1;
    }
    if (strchr(glob, '\\')) {
        mp_err_set(e, "--keep-loose usa '/' como separador, no '\\': %s", glob);
        return -1;
    }
    char **v = realloc(r->keep, (r->n + 1) * sizeof *v);
    char *copy = mp_strdup(glob);
    if (!v || !copy) {
        free(copy);
        if (v) r->keep = v;
        mp_err_set(e, "sin memoria");
        return -1;
    }
    r->keep = v;
    r->keep[r->n++] = copy;
    return 0;
}

mp_class mp_rules_classify(const mp_rules *r, const char *path, uint64_t size) {
    if (size == 0) return MP_LOOSE;
    const char *slash = strchr(path, '/');
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t bn = strlen(base);
    if (ieq_n(base, bn, "eboot.bin")) return MP_LOOSE;
    const char *dot = strrchr(base, '.');
    if (dot) {
        size_t en = strlen(dot);
        if (ieq_n(dot, en, ".elf") || ieq_n(dot, en, ".self") || ieq_n(dot, en, ".prx") ||
            ieq_n(dot, en, ".sprx") || ieq_n(dot, en, ".ini"))
            return MP_LOOSE;
    }
    if (slash) {   // carpetas de la raíz que carga el sistema
        size_t fn = (size_t)(slash - path);
        if (ieq_n(path, fn, "sce_sys") || ieq_n(path, fn, "sce_module") ||
            istarts_n(path, fn, "fakelib") || istarts_n(path, fn, "ampr_assets"))
            return MP_LOOSE;
    } else {       // archivos AMPR de la raíz
        if (ieq_n(path, bn, "ampr_emu.index") || ieq_n(path, bn, "ampr_zstd.index") ||
            ieq_n(path, bn, "ampr_commands.bin") || ieq_n(path, bn, "apr_emu.log") ||
            ieq_n(path, bn, "ampr_emu.log") || istarts_n(path, bn, "ampr_assets") ||
            mp_glob_match("ampr_zstd-*.pak", path))
            return MP_LOOSE;
    }
    for (size_t i = 0; r && i < r->n; i++)
        if (mp_glob_match(r->keep[i], path)) return MP_LOOSE;
    return MP_PACKABLE;
}

void mp_rules_free(mp_rules *r) {
    if (!r) return;
    for (size_t i = 0; i < r->n; i++) free(r->keep[i]);
    free(r->keep);
    free(r);
}
