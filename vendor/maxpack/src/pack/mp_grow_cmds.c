// SPDX-License-Identifier: GPL-3.0-or-later
// add and update (mp_grow.h): the new dump each one makes of a .p5z, for the grow engine.
//   add:    the dump the .p5z was made of, its DLC and the new ones (addcontN/ after its last), with the
//           dlc_emu.ini written again; the game's files are taken from the .p5z itself, never read.
//   update: the new dump given, of the same game in another version, with the DLC the .p5z carries.
#include "mp_grow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_dlc.h"
#include "mp_p5z_internal.h"
#include "mp_param.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return 0;
        if (!*a) return 1;
    }
}

static long find_manifest(const mp_p5z *p, const char *path) {
    size_t lo = 0, hi = p->man.n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(p->man.v[mid].path, path);
        if (c == 0) return (long)mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

static int is_ini(void *ctx, const char *path) {
    (void)ctx;
    return ieq(path, MP_DLC_INI);
}

static int not_ini(void *ctx, const char *path) { return !is_ini(ctx, path); }

// A file taken from the .p5z itself is the one of its manifest: its SHA-256 is known, never read.
static int known_old(void *ctx, const char *path, uint64_t size, uint8_t sha[32]) {
    const mp_p5z *p = ctx;
    long k = find_manifest(p, path);
    if (k < 0 || p->man.v[k].size != size || is_ini(NULL, path)) return 0;
    memcpy(sha, p->man.v[k].sha, 32);
    return 1;
}

static int open_recovered(const char *p5z, mp_p5z **p, mp_grow_result *r, mp_err *e) {
    int what = 0;
    if (mp_p5z_recover(p5z, &what, e) != 0) return -1;
    if (what == MP_RECOVER_BUSY) {
        r->code = "busy";
        mp_err_set(e, "another MaxPack is changing %s now", p5z);
        return -1;
    }
    return mp_p5z_open(p5z, p, e);
}

// The DLC a .p5z carries, from its dlc_emu.ini (none when it has none). -1 with *code "dlc_has_ini" when
// the ini is not exactly the one MaxPack writes for them (a game's own, written by hand), which add will
// not write again.
static int carried_dlc(mp_p5z *p, mp_dlc_list *have, char **bytes, size_t *nbytes, const char **code, mp_err *e) {
    memset(have, 0, sizeof *have);
    if (bytes) *bytes = NULL;
    if (nbytes) *nbytes = 0;
    mp_source *s = NULL;
    if (mp_p5z_source(p, is_ini, NULL, &s, e) != 0) return -1;
    int rc = 0, ours = 0;
    if (mp_source_count(s) == 1) {
        uint64_t size = mp_source_entry(s, 0)->size;
        char *b = NULL, *again = NULL;
        size_t len = 0;
        rc = -1;
        if (size <= MP_DLC_INI_MAX) {
            if (!(b = malloc((size_t)size + 1))) {
                mp_err_set(e, "out of memory");
            } else if (mp_source_read(s, 0, 0, b, (size_t)size, e) == 0) {
                if (mp_dlc_parse(b, (size_t)size, have) != 0) {
                    mp_err_set(e, "out of memory");
                } else {
                    again = mp_dlc_ini(have->v, have->n, &len);
                    ours = again && len == size && !memcmp(again, b, len);
                    rc = 0;
                }
            }
        } else {
            rc = 0;   // over what dlc_emu reads: not one MaxPack wrote
        }
        if (!rc && !ours) {
            rc = -1;
            *code = "dlc_has_ini";
            mp_err_set(e, "the game has a dlc_emu.ini that MaxPack did not write: MaxPack writes it again for the DLC "
                          "it adds and does not mix two lists; pack the game again with all its DLC (--dlc)");
        }
        free(again);
        if (!rc && bytes) {
            *bytes = b;
            *nbytes = (size_t)size;
            b = NULL;
        }
        free(b);
        if (rc) mp_dlc_list_free(have);
    }
    mp_source_close(s);
    return rc;
}

// ---- update ---------------------------------------------------------------------------------------

// The first segment of path is name (ASCII case aside), with something after it.
static int in_folder(const char *path, const char *name) {
    size_t k = strlen(name);
    for (size_t i = 0; i < k; i++)
        if (lower((unsigned char)path[i]) != lower((unsigned char)name[i])) return 0;
    return path[k] == '/' && path[k + 1];
}

// The DLC the .p5z carries and keeps through an update: its dlc_emu.ini and the folders it lists.
typedef struct {
    const mp_p5z *p;
    const mp_dlc_list *have;
} carried;

static int in_carried_folder(const carried *c, const char *path) {
    for (size_t k = 0; k < c->have->n; k++)
        if (c->have->v[k].data && c->have->v[k].folder[0] && in_folder(path, c->have->v[k].folder)) return 1;
    return 0;
}

static int is_carried(void *ctx, const char *path) {
    const carried *c = ctx;
    if (is_ini(NULL, path)) return 1;
    for (size_t k = 0; k < c->have->n; k++)
        if (c->have->v[k].data && c->have->v[k].folder[0] && in_folder(path, c->have->v[k].folder)) return 1;
    return 0;
}

static int carried_folder_cb(void *ctx, const char *path) { return in_carried_folder(ctx, path); }

// Only what comes from the .p5z itself has its SHA-256 known: the new dump's own files are read.
static int known_carried(void *ctx, const char *path, uint64_t size, uint8_t sha[32]) {
    const carried *c = ctx;
    if (!is_carried(ctx, path)) return 0;
    long k = find_manifest(c->p, path);
    if (k < 0 || c->p->man.v[k].size != size) return 0;
    memcpy(sha, c->p->man.v[k].sha, 32);
    return 1;
}

static long find_ci(mp_source *s, const char *path) {
    long i = mp_source_find(s, path);
    if (i >= 0) return i;
    for (size_t k = 0; k < mp_source_count(s); k++)
        if (ieq(mp_source_entry(s, k)->path, path)) return (long)k;
    return -1;
}

int mp_grow_update(const char *p5z, const char *dump, const mp_grow_opts *o, mp_grow_result *r, mp_err *e) {
    memset(r, 0, sizeof *r);
    mp_p5z *p = NULL;
    mp_source *d = NULL, *dlc = NULL;
    mp_dlc_list have;
    memset(&have, 0, sizeof have);
    const char *code = NULL;
    char *ini = NULL;   // the .p5z's dlc_emu.ini, when its DLC go on with it
    size_t ini_len = 0;
    int rc = -1;
    if (open_recovered(p5z, &p, r, e) != 0) goto done;
    if (mp_source_open(dump, &d, e) != 0) goto done;
    // the same game (its title ID), in another version
    char id[16] = "", ver[16] = "", name[128] = "";
    long pi = find_ci(d, "sce_sys/param.json");
    if (pi >= 0 && mp_source_entry(d, (size_t)pi)->size <= (1u << 20)) {
        size_t pl = (size_t)mp_source_entry(d, (size_t)pi)->size;
        char *pj = malloc(pl + 1);
        if (pj && mp_source_read(d, (size_t)pi, 0, pj, pl, NULL) == 0) mp_param_read(pj, pl, id, ver, name);
        free(pj);
    }
    if (!p->man.title_id[0] || !id[0]) {
        code = "no_title_id";
        mp_err_set(e, "%s gives no title ID (its sce_sys/param.json has no titleId), so it cannot be told whether "
                      "it is the same game",
                   !p->man.title_id[0] ? p5z : dump);
        goto done;
    }
    if (strcmp(id, p->man.title_id)) {
        code = "other_game";
        mp_err_set(e, "the new dump is another game: it is %s, and the .p5z is %s", id, p->man.title_id);
        goto done;
    }
    if (!strcmp(ver, p->man.content_version)) {
        code = "same_version";
        mp_err_set(e, "the new dump is the version the .p5z has already (%s)", ver[0] ? ver : "no version");
        goto done;
    }
    // its DLC go on with it, unless the new dump brings a dlc_emu.ini of its own (then it says what it has)
    carried c = {p, &have};
    const char *cc = NULL;
    mp_err ce = {{0}};
    int keep_dlc = find_ci(d, MP_DLC_INI) < 0 && carried_dlc(p, &have, &ini, &ini_len, &cc, &ce) == 0 && have.n > 0;
    if (!keep_dlc && !cc && ce.msg[0]) {   // the ini could not be read: not a reason to lose the DLC
        *e = ce;
        goto done;
    }
    if (keep_dlc) {
        for (size_t k = 0; k < mp_source_count(d); k++)
            if (is_carried(&c, mp_source_entry(d, k)->path)) {
                code = "dlc_refused";
                mp_err_set(e, "the new dump has %s, where the .p5z keeps one of its DLC: take it out of the dump",
                           mp_source_entry(d, k)->path);
                goto done;
            }
        if (mp_p5z_source(p, carried_folder_cb, &c, &dlc, e) != 0) goto done;
        // dlc_emu.ini, the same bytes, dated as pack --dlc dates it: the newest of the game's files and the DLC's
        int64_t mtime = 0;
        for (size_t k = 0; k < mp_source_count(d); k++)
            if (mp_source_entry(d, k)->mtime > mtime) mtime = mp_source_entry(d, k)->mtime;
        for (size_t k = 0; k < mp_source_count(dlc); k++)
            if (mp_source_entry(dlc, k)->mtime > mtime) mtime = mp_source_entry(dlc, k)->mtime;
        mp_source *parts[1] = {dlc};
        const char *prefixes[1] = {""};
        mp_source *m = NULL;
        dlc = NULL;   // taken by the merge, with d
        int mr = mp_source_merge(d, parts, prefixes, 1, NULL, MP_DLC_INI, (const uint8_t *)ini, ini_len, mtime, &m, e);
        d = mr == 0 ? m : NULL;
        if (mr != 0) goto done;
    }
    rc = mp_grow_run(p5z, d, keep_dlc ? known_carried : NULL, &c, o, r, e);
    code = r->code;
done:
    if (rc != 0 && code) r->code = code;
    free(ini);
    mp_source_close(dlc);
    mp_source_close(d);
    mp_dlc_list_free(&have);
    mp_p5z_close(p);
    return rc;
}

int mp_grow_add(const char *p5z, const char *const *dlc, size_t n, const mp_grow_opts *o, mp_grow_result *r, mp_err *e) {
    memset(r, 0, sizeof *r);
    if (!n) {
        mp_err_set(e, "no DLC to add");
        return -1;
    }
    mp_p5z *p = NULL;
    mp_source *game = NULL;
    mp_dlc_list have, added;
    memset(&have, 0, sizeof have);
    memset(&added, 0, sizeof added);
    const char *code = NULL;
    int rc = -1;
    if (open_recovered(p5z, &p, r, e) != 0) goto done;
    if (carried_dlc(p, &have, NULL, NULL, &code, e) != 0) goto done;
    if (mp_p5z_source(p, not_ini, NULL, &game, e) != 0) goto done;
    if (mp_dlc_attach_ex(&game, have.v, have.n, dlc, n, &added, &code, e) != 0) goto done;   // takes game
    mp_grow_opts g = *o;
    g.force = 1;                  // however much the DLC take, the game is still the one it was
    g.leave_out_leftovers = 0;    // what the game left out when packed is not in the .p5z either
    rc = mp_grow_run(p5z, game, known_old, p, &g, r, e);
    code = r->code;
    for (size_t k = 0; k < added.n; k++) {
        if (added.v[k].data) {
            r->dlc_data++;
            r->dlc_bytes += added.v[k].bytes;
        } else {
            r->dlc_license++;
        }
    }
done:
    if (rc != 0 && code) r->code = code;
    mp_source_close(game);
    mp_dlc_list_free(&have);
    mp_dlc_list_free(&added);
    mp_p5z_close(p);
    return rc;
}
