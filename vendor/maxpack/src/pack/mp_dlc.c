// SPDX-License-Identifier: GPL-3.0-or-later
// A game's DLC packed with it (mp_dlc.h).
#include "mp_dlc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_param.h"
#include "mp_platform.h"
#include "mp_rules.h"

#define PARAM_PATH "sce_sys/param.json"
#define PARAM_MAX (1u << 20)

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return 0;
        if (!*a) return 1;
    }
}

// The first segment of path is name (ASCII case aside), with something after it.
static int in_folder(const char *path, const char *name) {
    size_t k = strlen(name);
    for (size_t i = 0; i < k; i++)
        if (lower((unsigned char)path[i]) != lower((unsigned char)name[i])) return 0;
    return path[k] == '/' && path[k + 1];
}

static long find_ci(mp_source *s, const char *path) {
    long i = mp_source_find(s, path);
    if (i >= 0) return i;
    for (size_t k = 0; k < mp_source_count(s); k++)
        if (ieq(mp_source_entry(s, k)->path, path)) return (long)k;
    return -1;
}

void mp_dlc_list_free(mp_dlc_list *l) {
    if (!l) return;
    free(l->v);
    l->v = NULL;
    l->n = 0;
}

int mp_dlc_content_id_ok(const char *cid) {
    if (strlen(cid) != 36 || cid[6] != '-' || cid[16] != '_' || cid[19] != '-') return 0;
    for (int i = 0; i < 36; i++) {
        unsigned char c = (unsigned char)cid[i];
        if (i == 6 || i == 16 || i == 19) continue;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return 0;
    }
    return 1;
}

char *mp_dlc_ini(const mp_dlc *v, size_t n, size_t *len) {
    size_t cap = 1;
    for (size_t i = 0; i < n; i++) cap += 128;
    char *b = malloc(cap);
    if (!b) return NULL;
    size_t at = 0;
    b[0] = 0;
    for (size_t i = 0; i < n; i++) {
        int w = v[i].data ? snprintf(b + at, cap - at, "%s[PSAC]\ncontent_id=%s\nmount_point=/app0/%s\n", i ? "\n" : "",
                                     v[i].content_id, v[i].folder)
                          : snprintf(b + at, cap - at, "%s[PSAL]\ncontent_id=%s\ndownload_status=NO_EXTRA_DATA\n",
                                     i ? "\n" : "", v[i].content_id);
        if (w < 0 || (size_t)w >= cap - at) {
            free(b);
            return NULL;
        }
        at += (size_t)w;
    }
    *len = at;
    return b;
}

// The bytes of a source's param.json (malloc, NUL-terminated), or NULL when it has none or it cannot
// be read.
static char *read_param(mp_source *s, size_t *len) {
    long i = find_ci(s, PARAM_PATH);
    *len = 0;
    if (i < 0) return NULL;
    uint64_t size = mp_source_entry(s, (size_t)i)->size;
    char *b = size <= PARAM_MAX ? malloc((size_t)size + 1) : NULL;
    if (!b || mp_source_read(s, (size_t)i, 0, b, (size_t)size, NULL) != 0) {
        free(b);
        return NULL;
    }
    b[size] = 0;
    *len = (size_t)size;
    return b;
}

// The title part of a content ID ("PPSA00000" of "UP0000-PPSA00000_00-..."), "" when it has none.
static void title_of(const char *cid, char out[16]) {
    out[0] = 0;
    if (strlen(cid) >= 16 && cid[6] == '-') {
        memcpy(out, cid + 7, 9);
        out[9] = 0;
    }
}

// The game's title ID (its param.json's titleId, else the title part of its content ID) and its content
// ID (its param.json's contentId, else its package's; "" when it has none).
static void game_ids(mp_source *game, char title[16], char cid[48]) {
    size_t len = 0;
    char *pj = read_param(game, &len), id[16] = "", ver[16], name[128];
    cid[0] = 0;
    if (pj) {
        mp_param_read(pj, len, id, ver, name);
        mp_param_content_id(pj, len, cid);
        free(pj);
    }
    if (!cid[0]) snprintf(cid, 48, "%s", mp_source_package_id(game));
    if (id[0])
        snprintf(title, 16, "%s", id);
    else
        title_of(cid, title);
}

// What dump tools leave behind and the OS's files (mp_complete.h) are no files of a DLC: they are left
// out of what goes inside the game, and make no license a DLC with data.
static int not_of_dlc(const char *path) { return mp_complete_is_junk(path) || mp_complete_is_leftover(path); }

int mp_dlc_has_data(mp_source *s) {
    for (size_t i = 0; i < mp_source_count(s); i++) {
        const char *p = mp_source_entry(s, i)->path;
        if (!in_folder(p, "sce_sys") && !not_of_dlc(p)) return 1;
    }
    return 0;
}

// Checks one DLC and gives its content ID (cid) and whether it has data: 0, or -1 with e set.
static int check_dlc(mp_source *s, const char *path, const char *title, const char *game_cid, char cid[37], int *data,
                     mp_err *e) {
    if (!mp_source_is_dlc_package(s)) {
        if (find_ci(s, PARAM_PATH) < 0) {
            mp_err_set(e, "%s: not a DLC: it has no sce_sys/param.json", path);
            return -1;
        }
        if (find_ci(s, "eboot.bin") >= 0) {
            mp_err_set(e, "%s: a game, not a DLC: it has eboot.bin", path);
            return -1;
        }
    }
    for (size_t i = 0; i < mp_source_count(s); i++) {
        const char *p = mp_source_entry(s, i)->path;
        if (ieq(p, "ampr_zstd.index") || mp_glob_match("ampr_zstd-*.pak", p)) {
            mp_err_set(e, "%s: this DLC is packed already (it has %s): give it as it was dumped", path, p);
            return -1;
        }
    }
    char found[48] = "";
    if (mp_source_is_dlc_package(s)) {
        snprintf(found, sizeof found, "%s", mp_source_package_id(s));
    } else {
        size_t len = 0;
        char *pj = read_param(s, &len);
        if (pj) mp_param_content_id(pj, len, found);
        free(pj);
        if (!found[0]) {
            mp_err_set(e, "%s: its sce_sys/param.json gives no contentId", path);
            return -1;
        }
    }
    if (!mp_dlc_content_id_ok(found)) {
        mp_err_set(e, "%s: its content ID \"%s\" is not one dlc_emu takes: 36 characters, as "
                      "UP0000-PPSA00000_00-ABCDEFGHIJKLMNOP",
                   path, found);
        return -1;
    }
    if (!strcmp(found, game_cid)) {
        mp_err_set(e, "%s: its content ID %s is the game's own content ID: it is the game, not a DLC of it", path,
                   found);
        return -1;
    }
    char dt[16];
    title_of(found, dt);
    if (strcmp(dt, title)) {
        mp_err_set(e, "%s: a DLC of another game: its content ID %s is of %s, and the game is %s", path, found, dt, title);
        return -1;
    }
    memcpy(cid, found, 37);
    *data = mp_dlc_has_data(s);
    return 0;
}

// ---- reading a dlc_emu.ini --------------------------------------------------------------------

static int blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// [p, q) without the blanks at its ends.
static void trim(const char **p, const char **q) {
    while (*p < *q && blank(**p)) (*p)++;
    while (*q > *p && blank((*q)[-1])) (*q)--;
}

static int is(const char *p, const char *q, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(q - p) != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)p[i]) != lower((unsigned char)word[i])) return 0;
    return 1;
}

typedef struct {
    int kind;   // 0 none (or another kind), 1 PSAC, 2 PSAL
    char cid[37], mount[64];
    int have_cid;
} section;

// The DLC of a section that has ended, when it is one.
static int close_section(section *sec, mp_dlc_list *out, size_t *cap) {
    if (!sec->kind || !sec->have_cid || out->n >= MP_DLC_MAX_ENTRIES) return 0;
    if (out->n == *cap) {
        size_t c = *cap ? *cap * 2 : 8;
        mp_dlc *g = realloc(out->v, c * sizeof *g);
        if (!g) return -1;
        out->v = g;
        *cap = c;
    }
    mp_dlc *d = &out->v[out->n++];
    memset(d, 0, sizeof *d);
    memcpy(d->content_id, sec->cid, sizeof d->content_id);
    d->data = sec->kind == 1;
    // a mount point dlc_emu takes (at most 15 characters) in the game: "/app0/<folder>"
    size_t ml = strlen(sec->mount);
    if (d->data && ml > 6 && ml <= 15 && !strncmp(sec->mount, "/app0/", 6) && !strstr(sec->mount, "..") &&
        sec->mount[ml - 1] != '/')
        snprintf(d->folder, sizeof d->folder, "%s", sec->mount + 6);
    return 0;
}

int mp_dlc_parse(const char *ini, size_t len, mp_dlc_list *out) {
    memset(out, 0, sizeof *out);
    if (len > MP_DLC_INI_MAX) return 0;
    if (len >= 3 && !memcmp(ini, "\xef\xbb\xbf", 3)) {   // a UTF-8 BOM
        ini += 3;
        len -= 3;
    }
    size_t cap = 0;
    section sec;
    memset(&sec, 0, sizeof sec);
    const char *end = ini + len;
    for (const char *line = ini; line < end;) {
        const char *nl = memchr(line, '\n', (size_t)(end - line));
        const char *p = line, *q = nl ? nl : end;
        line = nl ? nl + 1 : end;
        trim(&p, &q);
        if (p == q) continue;
        if (*p == '[' && q[-1] == ']') {
            if (close_section(&sec, out, &cap) != 0) goto oom;
            memset(&sec, 0, sizeof sec);
            sec.kind = is(p + 1, q - 1, "PSAC") ? 1 : is(p + 1, q - 1, "PSAL") ? 2 : 0;
            continue;
        }
        const char *eq = memchr(p, '=', (size_t)(q - p));
        if (!eq || !sec.kind) continue;
        const char *k = p, *ke = eq, *v = eq + 1, *ve = q;
        trim(&k, &ke);
        trim(&v, &ve);
        size_t vl = (size_t)(ve - v);
        if (is(k, ke, "content_id")) {
            int ok = vl == 36;
            for (size_t i = 0; ok && i < vl; i++) ok = (unsigned char)v[i] > 0x20 && (unsigned char)v[i] < 0x7F;
            sec.have_cid = ok;
            if (ok) {
                memcpy(sec.cid, v, 36);
                sec.cid[36] = 0;
            }
        } else if (is(k, ke, "mount_point")) {
            if (vl < sizeof sec.mount) {
                memcpy(sec.mount, v, vl);
                sec.mount[vl] = 0;
            } else {
                sec.mount[0] = 0;
            }
        }
    }
    if (close_section(&sec, out, &cap) != 0) goto oom;
    return 0;
oom:
    mp_dlc_list_free(out);
    return -1;
}

void mp_dlc_sizes(mp_dlc_list *l, const mp_cfile *files, size_t n) {
    for (size_t k = 0; k < l->n; k++) {
        mp_dlc *d = &l->v[k];
        d->files = d->bytes = 0;
        if (!d->folder[0]) continue;
        for (size_t i = 0; i < n; i++)
            if (in_folder(files[i].path, d->folder)) {
                d->files++;
                d->bytes += files[i].size;
            }
    }
}

int mp_dlc_attach(mp_source **game, const char *const *paths, size_t n, mp_dlc_list *out, const char **code,
                  mp_err *e) {
    return mp_dlc_attach_ex(game, NULL, 0, paths, n, out, code, e);
}

int mp_dlc_attach_ex(mp_source **game, const mp_dlc *have, size_t nhave, const char *const *paths, size_t n,
                     mp_dlc_list *out, const char **code, mp_err *e) {
    *code = "dlc_refused";
    if (out) memset(out, 0, sizeof *out);
    mp_source *g = *game;
    *game = NULL;
    mp_source **src = calloc(n ? n : 1, sizeof *src);
    mp_dlc *v = calloc(n ? n : 1, sizeof *v);
    mp_source **parts = calloc(n ? n : 1, sizeof *parts);
    char (*folders)[16] = calloc(n ? n : 1, sizeof *folders);
    const char **prefixes = calloc(n ? n : 1, sizeof *prefixes);
    mp_dlc *all = calloc(nhave + n ? nhave + n : 1, sizeof *all);
    char *ini = NULL;
    size_t ini_len = 0, nparts = 0;
    int rc = -1;
    if (!src || !v || !parts || !folders || !prefixes || !all) {
        mp_err_set(e, "out of memory");
        goto done;
    }
    // the game: a list of its own is not mixed with MaxPack's (dlc_emu shows only the DLC of its file)
    for (size_t i = 0; i < mp_source_count(g); i++)
        if (ieq(mp_source_entry(g, i)->path, MP_DLC_INI)) {
            *code = "dlc_has_ini";
            mp_err_set(e, "the game already has a dlc_emu.ini: MaxPack writes it for the DLC it packs with the game "
                          "and does not mix two lists (dlc_emu shows only the DLC of its file); take it out of the "
                          "dump, or pack it without --dlc");
            goto done;
        }
    char title[16], game_cid[48];
    game_ids(g, title, game_cid);
    if (!title[0]) {
        mp_err_set(e, "the game gives no title ID (its sce_sys/param.json has neither titleId nor contentId), so "
                      "its DLC cannot be told");
        goto done;
    }
    // the DLC the game carries already keep their folders: the new ones go after the last of them
    size_t ndata = 0;
    for (size_t j = 0; j < nhave; j++) {
        unsigned id = 0;
        if (have[j].data && sscanf(have[j].folder, "addcont%u", &id) == 1 && id + 1 > ndata) ndata = id + 1;
    }
    for (size_t k = 0; k < n; k++) {
        if (mp_source_open_as(paths[k], MP_OPEN_DLC, &src[k], e) != 0) goto done;
        if (check_dlc(src[k], paths[k], title, game_cid, v[k].content_id, &v[k].data, e) != 0) goto done;
        for (size_t j = 0; j < nhave; j++) {
            if (!strcmp(have[j].content_id, v[k].content_id)) {
                mp_err_set(e, "%s: the game has this DLC already (%s)", paths[k], v[k].content_id);
                goto done;
            }
            if (!strcmp(have[j].content_id + 20, v[k].content_id + 20)) {
                mp_err_set(e, "%s: the game has a DLC with its label %s already (the end of its content ID), which "
                              "dlc_emu takes once",
                           paths[k], v[k].content_id + 20);
                goto done;
            }
        }
        for (size_t j = 0; j < k; j++) {
            if (!strcmp(v[j].content_id, v[k].content_id)) {
                mp_err_set(e, "%s and %s: the same content ID (%s): give each DLC once", paths[j], paths[k], v[k].content_id);
                goto done;
            }
            if (!strcmp(v[j].content_id + 20, v[k].content_id + 20)) {
                mp_err_set(e, "%s and %s: the same label %s (the end of their content IDs), which dlc_emu takes once",
                           paths[j], paths[k], v[k].content_id + 20);
                goto done;
            }
        }
        if (v[k].data) {
            if (ndata == MP_DLC_MAX_DATA) {
                *code = "dlc_too_many";
                mp_err_set(e, "more than %d DLC with files of their own: dlc_emu takes at most %d (addcont0 to addcont%d)",
                           MP_DLC_MAX_DATA, MP_DLC_MAX_DATA, MP_DLC_MAX_DATA - 1);
                goto done;
            }
            snprintf(v[k].folder, sizeof v[k].folder, "addcont%u", (unsigned)ndata);
            for (size_t i = 0; i < mp_source_count(src[k]); i++) {
                if (not_of_dlc(mp_source_entry(src[k], i)->path)) continue;
                v[k].files++;
                v[k].bytes += mp_source_entry(src[k], i)->size;
            }
            ndata++;
        }
    }
    // the ini lists the DLC it had and then the new ones
    if (nhave) memcpy(all, have, nhave * sizeof *all);
    if (n) memcpy(all + nhave, v, n * sizeof *all);
    size_t nall = nhave + n;
    if (nall > MP_DLC_MAX_ENTRIES || !(ini = mp_dlc_ini(all, nall, &ini_len)) || ini_len > MP_DLC_INI_MAX) {
        if (nall <= MP_DLC_MAX_ENTRIES && !ini) {
            mp_err_set(e, "out of memory");
        } else {
            *code = "dlc_too_many";
            mp_err_set(e, "too many DLC: their dlc_emu.ini would take %zu bytes, and dlc_emu reads at most 32 KiB",
                       nall > MP_DLC_MAX_ENTRIES ? nall * 80 : ini_len);
        }
        goto done;
    }
    // where their folders go must be free in the game: no folder, nor a file, of that name
    for (size_t i = 0; i < mp_source_count(g); i++) {
        const char *p = mp_source_entry(g, i)->path;
        for (size_t k = 0; k < n; k++) {
            if (!v[k].data) continue;
            int file = ieq(p, v[k].folder);
            if (!file && !in_folder(p, v[k].folder)) continue;
            mp_err_set(e, "the game already has %.*s%s, where its DLC %s would go", (int)strlen(v[k].folder), p,
                       file ? " (a file)" : "/", v[k].content_id);
            goto done;
        }
    }
    int64_t mtime = 0;
    for (size_t i = 0; i < mp_source_count(g); i++)
        if (mp_source_entry(g, i)->mtime > mtime) mtime = mp_source_entry(g, i)->mtime;
    for (size_t k = 0; k < n; k++)
        for (size_t i = 0; i < mp_source_count(src[k]); i++)
            if (!not_of_dlc(mp_source_entry(src[k], i)->path) && mp_source_entry(src[k], i)->mtime > mtime)
                mtime = mp_source_entry(src[k], i)->mtime;
    for (size_t k = 0; k < n; k++) {
        if (v[k].data) {
            memcpy(folders[nparts], v[k].folder, sizeof folders[nparts]);
            prefixes[nparts] = folders[nparts];
            parts[nparts++] = src[k];
        } else {
            mp_source_close(src[k]);
        }
        src[k] = NULL;
    }
    mp_source *m = NULL;
    rc = mp_source_merge(g, parts, prefixes, nparts, not_of_dlc, MP_DLC_INI, (const uint8_t *)ini, ini_len, mtime, &m,
                         e);
    g = NULL;   // taken, with the parts
    nparts = 0;
    if (rc == 0) {
        *game = m;
        *code = NULL;
        if (out) {
            out->v = v;
            out->n = n;
            v = NULL;
        }
    }
done:
    mp_source_close(g);
    for (size_t k = 0; src && k < n; k++) mp_source_close(src[k]);
    free(src);
    free(v);
    free(parts);
    free(folders);
    free(prefixes);
    free(all);
    free(ini);
    return rc;
}
