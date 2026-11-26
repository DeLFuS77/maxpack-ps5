// SPDX-License-Identifier: GPL-3.0-or-later
// The grow engine (mp_grow.h): a .p5z becomes a new dump by appending only what the dump changes.
//
// 1. The new dump against the manifest: a file with the path, size and SHA-256 of one of it is the same
//    (its SHA-256 read from the dump, or told by the caller); the rest is new or changed; what the dump
//    does not have is removed.
// 2. The same files stay where they are: their records are kept (with the dump's date) and, if packed,
//    their chunks. The others are stored as pack stores them: loose files in frames, the rest in new
//    volumes numbered after the old ones, with the .p5z's archive_id and level; ampr_emu.index and
//    ampr_zstd.index are made again for the whole new dump (kept when they come out the same).
// 3. The append: the .p5z is opened as its only writer, the journal written, the regions, the index
//    version 2 and the footer appended, the data flushed, the journal deleted. Any failure cuts the
//    .p5z back to its size, exactly as it was.
#include "mp_grow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_complete.h"
#include "mp_hash.h"
#include "mp_idx3.h"
#include "mp_p5z_internal.h"
#include "mp_param.h"
#include "mp_rules.h"

#define PIECE (4u << 20)
#define PARAM_PATH "sce_sys/param.json"

static mp_grow_fault_fn g_fault;
static int g_crashed;   // this run stopped as a power cut would (tests)

void mp_grow_set_fault(mp_grow_fault_fn fn) { g_fault = fn; }

static int fault(const char *step, mp_err *e) {
    if (!g_fault) return 0;
    int k = g_fault(step);
    if (!k) return 0;
    if (k == 2) g_crashed = 1;
    mp_err_set(e, "stopped at %s (test)", step);
    return -1;
}

void mp_grow_opts_default(mp_grow_opts *o) {
    memset(o, 0, sizeof *o);
    o->threads = mp_cpu_count();
    o->progress = 1;
    o->volume_limit = MP_ZPK_VOLUME_LIMIT;
}

static int ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

enum { K_SAME, K_NEW, K_CHANGED };

// A file of the new dump.
typedef struct {
    int kind, role;   // role: MP_ROLE_LOOSE, MP_ROLE_VOLUME (packed), MP_ROLE_ORIGINAL; -1 left out
    long old;         // its record in the old manifest, or -1
    int hashed;       // sha is known before it is stored
    int hash;         // its SHA-256 has to be read to compare
    uint8_t sha[32];
} gent;

static long find_old(const mp_pack_manifest *m, const char *path) {
    size_t lo = 0, hi = m->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(m->v[mid].path, path);
        if (c == 0) return (long)mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

// ---- 1. comparing: the SHA-256 of the files that may be the same, in parallel ------------------------

typedef struct {
    mp_source *src;
    gent *g;
    const size_t *todo;
    mp_progress *prog;
    int failed;
    mp_err err;
    mp_mutex *mu;
} hasher;

static int hash_work(void *ctx, size_t k, unsigned tid) {
    hasher *h = ctx;
    (void)tid;
    size_t i = h->todo[k];
    uint64_t size = mp_source_entry(h->src, i)->size;
    uint8_t *buf = malloc(PIECE);
    mp_err e = {{0}};
    int rc = buf ? 0 : -1;
    if (!buf) mp_err_set(&e, "out of memory");
    mp_sha256 s;
    mp_sha256_init(&s);
    for (uint64_t off = 0; !rc && off < size; off += PIECE) {
        size_t n = size - off < PIECE ? (size_t)(size - off) : PIECE;
        rc = mp_cancel_check(&e);
        if (!rc) rc = mp_source_read(h->src, i, off, buf, n, &e);
        if (!rc) {
            mp_sha256_update(&s, buf, n);
            mp_progress_add(h->prog, n);
        }
    }
    free(buf);
    if (rc) {
        mp_mutex_lock(h->mu);
        if (!h->failed) h->err = e;
        h->failed = 1;
        mp_mutex_unlock(h->mu);
        return 1;
    }
    mp_sha256_final(&s, h->g[i].sha);
    h->g[i].hashed = 1;
    return 0;
}

// ---- the volumes' sink, with the test hook and the cap of maxpack.image ------------------------------

typedef struct {
    mp_zpk_sink inner;
    uint32_t max_volumes;
    int too_many;
} gsink;

static int gs_begin(void *ctx, uint32_t vol, mp_err *e) {
    gsink *g = ctx;
    if (vol >= g->max_volumes) {
        g->too_many = 1;
        mp_err_set(e, "the .p5z would have more than %u volumes, the most the console takes: make a compact copy "
                      "of it first (maxpack compact)", g->max_volumes);
        return -1;
    }
    if (fault("volume", e) != 0) return -1;
    return g->inner.vol_begin(g->inner.ctx, vol, e);
}

static int gs_write(void *ctx, const void *p, size_t n, mp_err *e) {
    gsink *g = ctx;
    if (fault("volume data", e) != 0) return -1;
    return g->inner.vol_write(g->inner.ctx, p, n, e);
}

static int gs_end(void *ctx, uint32_t vol, mp_err *e) {
    gsink *g = ctx;
    return g->inner.vol_end(g->inner.ctx, vol, e);
}

static int gs_header(void *ctx, uint32_t vol, const uint8_t hdr[64], mp_err *e) {
    gsink *g = ctx;
    if (fault("volume header", e) != 0) return -1;
    return g->inner.vol_header(g->inner.ctx, vol, hdr, e);
}

// The folder a path is in (malloc), for the free space of its disk.
static char *folder_of(const char *path) {
    char *full = mp_full_path(path);
    if (!full) return NULL;
    char *s = full + strlen(full);
    while (s > full && s[-1] != '/' && s[-1] != '\\') s--;
    if (s > full + 1 && !(s - full == 3 && full[1] == ':')) s--;   // "C:\" keeps its separator
    *s = 0;
    return full;
}

int mp_grow_run(const char *p5z, mp_source *dump, mp_grow_known known, void *kctx, const mp_grow_opts *o,
                mp_grow_result *r, mp_err *e) {
    memset(r, 0, sizeof *r);
    g_crashed = 0;
    mp_p5z *p = NULL;
    mp_zpk *z = NULL;
    mp_rules *rules = NULL;
    gent *g = NULL;
    uint8_t *cls = NULL, *matched = NULL, *buf = NULL, *idx3 = NULL, *zidx = NULL, (*psha)[32] = NULL;
    size_t *todo = NULL, idx3_len = 0, zidx_len = 0;
    uint32_t *pack_new = NULL;
    mp_idx3_row *rows = NULL;
    mp_zpk_entry *ents = NULL;
    uint64_t *vsizes = NULL;
    mp_zpk_chunks ch;
    memset(&ch, 0, sizeof ch);
    mp_pack_manifest fin;
    memset(&fin, 0, sizeof fin);
    mp_complete comp;
    memset(&comp, 0, sizeof comp);
    mp_pack_out out;
    memset(&out, 0, sizeof out);
    mp_wfile *f = NULL;
    int rc = -1, journal = 0;
    uint64_t old_size = 0;
    mp_progress prog;
    int prog_on = 0;

    int what = 0;
    if (mp_p5z_recover(p5z, &what, e) != 0) return -1;
    if (what == MP_RECOVER_BUSY) {
        r->code = "busy";
        mp_err_set(e, "another MaxPack is changing %s now", p5z);
        return -1;
    }
    if (mp_p5z_open(p5z, &p, e) != 0) return -1;
    old_size = p->size;
    memcpy(r->title_id, p->man.title_id, sizeof r->title_id);
    memcpy(r->old_version, p->man.content_version, sizeof r->old_version);
    size_t zrec = SIZE_MAX, xrec = SIZE_MAX, rtrec = SIZE_MAX;
    uint32_t nvol_old = 0;
    for (size_t i = 0; i < p->nfiles; i++) {
        if (p->files[i].role == MP_ROLE_ZPK_INDEX) zrec = i;
        if (p->files[i].role == MP_ROLE_IDX3) xrec = i;
        if (p->files[i].role == MP_ROLE_RUNTIME) rtrec = i;
        nvol_old += p->files[i].role == MP_ROLE_VOLUME;
    }
    if (zrec != SIZE_MAX && mp_p5z_zpk(p, &z, e) != 0) goto done;
    if ((z ? mp_zpk_volume_count(z) : 0) != nvol_old) {
        mp_err_set(e, "invalid .p5z: its volumes are not those of its packs");
        goto done;
    }

    size_t n = mp_source_count(dump);
    if (n == 0) {
        mp_err_set(e, "the new dump has no files");
        goto done;
    }
    g = calloc(n, sizeof *g);
    cls = malloc(n);
    matched = calloc(p->man.n ? p->man.n : 1, 1);
    todo = malloc(n * sizeof *todo);
    pack_new = malloc(n * sizeof *pack_new);
    rows = calloc(n + 1, sizeof *rows);
    fin.v = calloc(n, sizeof *fin.v);
    buf = malloc(PIECE);
    rules = mp_rules_new();
    if (!g || !cls || !matched || !todo || !pack_new || !rows || !fin.v || !buf || !rules) goto oom;
    for (size_t k = 0; k < o->nkeep; k++)
        if (mp_rules_add_keep_loose(rules, o->keep_loose[k], e) != 0) goto done;

    // What dump tools leave behind, as pack --leave-out-leftovers: only when the game's own list proves it.
    int drop = 0;
    if (o->leave_out_leftovers) {
        if (mp_complete_source(dump, &comp, cls, e) != 0) goto done;
        drop = mp_complete_proves_leftovers(&comp);
    }
    size_t orig_idx3 = SIZE_MAX, orig_rt = SIZE_MAX, param = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        const char *path = mp_source_entry(dump, i)->path;
        if (mp_pack_already_packed(path)) {
            r->code = "already_packed";
            mp_err_set(e, "the new dump is packed already (it has %s): give it as it was dumped", path);
            goto done;
        }
        if (ieq(path, "ampr_emu.index")) orig_idx3 = i;
        if (ieq(path, MP_RUNTIME_PATH)) orig_rt = i;
        if (ieq(path, PARAM_PATH)) param = i;
    }

    // 1. Against the manifest.
    size_t ntodo = 0;
    uint64_t hash_total = 0;
    for (size_t i = 0; i < n; i++) {
        const mp_entry *en = mp_source_entry(dump, i);
        gent *x = &g[i];
        x->old = -1;
        if (drop && cls[i] == MP_CF_LEFTOVER) {
            x->role = -1;
            continue;
        }
        r->total_bytes += en->size;
        x->old = find_old(&p->man, en->path);
        x->kind = x->old < 0 ? K_NEW : K_CHANGED;
        if (x->old >= 0) matched[x->old] = 1;
        if (x->old < 0 || p->man.v[x->old].size != en->size) continue;
        if (known && known(kctx, en->path, en->size, x->sha)) {
            x->hashed = 1;
        } else {
            todo[ntodo++] = i;
            hash_total += en->size;
        }
    }
    if (ntodo) {
        mp_progress_init(&prog, "Comparing with the .p5z", hash_total, o->progress);
        prog_on = 1;
        hasher h = {dump, g, todo, &prog, 0, {{0}}, mp_mutex_new()};
        if (!h.mu) goto oom;
        mp_parallel_run(o->threads ? o->threads : 1, ntodo, hash_work, &h, &prog);
        mp_mutex_free(h.mu);
        if (h.failed || mp_cancel_check(e) != 0) {
            if (h.failed && !mp_cancelled() && e) *e = h.err;
            if (mp_cancelled()) mp_cancel_check(e);
            goto done;
        }
        mp_progress_free(&prog);   // mp_parallel_run has printed its end
        prog_on = 0;
    }

    // 2. Where each one goes.
    uint64_t store_bytes = 0, chunks_total = 0, loose_total = 0, pack_total = 0;
    size_t npack_new = 0, npacked = 0;
    int same_dates = 1, packs_changed = 0;
    for (size_t i = 0; i < n; i++) {
        gent *x = &g[i];
        if (x->role < 0) continue;
        const mp_entry *en = mp_source_entry(dump, i);
        const mp_manifest_entry *om = x->old >= 0 ? &p->man.v[x->old] : NULL;
        if (om && x->hashed && om->size == en->size && !memcmp(om->sha, x->sha, 32)) x->kind = K_SAME;
        if (x->kind == K_SAME) {
            x->role = (om->flags & MP_MANIFEST_PACKED) ? MP_ROLE_VOLUME
                      : (om->flags & MP_MANIFEST_REPLACED) ? MP_ROLE_ORIGINAL
                                                           : MP_ROLE_LOOSE;
            r->same_files++;
            if (om->mtime != en->mtime) same_dates = 0;
        } else {
            if (i == orig_idx3 || (i == orig_rt && rtrec != SIZE_MAX))
                x->role = MP_ROLE_ORIGINAL;
            else
                x->role = mp_rules_classify(rules, en->path, en->size) == MP_LOOSE ? MP_ROLE_LOOSE : MP_ROLE_VOLUME;
            if (x->kind == K_NEW)
                r->new_files++;
            else
                r->changed_files++;
            r->stored_files++;
            r->stored_bytes += en->size;
            store_bytes += en->size;
            if (x->role == MP_ROLE_LOOSE) loose_total += en->size;
            if (x->role == MP_ROLE_VOLUME) {
                pack_new[npack_new++] = (uint32_t)i;
                pack_total += en->size;
                packs_changed = 1;
            }
        }
        if (x->role == MP_ROLE_VOLUME) {
            npacked++;
            chunks_total += (en->size + MP_BLOCK_V1 - 1) / MP_BLOCK_V1;
        }
        // the new manifest, in the dump's order (by path)
        mp_manifest_entry *m = &fin.v[fin.n++];
        m->path = en->path;
        m->size = en->size;
        m->mtime = en->mtime;
        m->flags = x->role == MP_ROLE_VOLUME ? MP_MANIFEST_PACKED : x->role == MP_ROLE_ORIGINAL ? MP_MANIFEST_REPLACED : 0;
        if (x->kind == K_SAME) memcpy(m->sha, om->sha, 32);
    }
    for (size_t k = 0; k < p->man.n; k++)
        if (!matched[k]) {
            r->removed_files++;
            if (p->man.v[k].flags & MP_MANIFEST_PACKED) packs_changed = 1;
        }

    // the title, its version and its name, from the new dump's param.json as pack reads them
    memcpy(fin.title_id, p->man.title_id, sizeof fin.title_id);
    fin.title_id[0] = fin.content_version[0] = fin.title_name[0] = 0;
    if (param != SIZE_MAX && g[param].role >= 0 && mp_source_entry(dump, param)->size <= (1u << 20)) {
        size_t pl = (size_t)mp_source_entry(dump, param)->size;
        char *pj = malloc(pl + 1);
        if (pj && mp_source_read(dump, param, 0, pj, pl, NULL) == 0) {
            pj[pl] = 0;
            mp_param_read(pj, pl, fin.title_id, fin.content_version, fin.title_name);
        }
        free(pj);
    }
    memcpy(r->new_version, fin.content_version, sizeof r->new_version);
    fin.level = p->man.level;
    memcpy(fin.archive_id, p->man.archive_id, 16);
    if (!r->new_files && !r->changed_files && !r->removed_files && same_dates &&
        !strcmp(fin.title_id, p->man.title_id) && !strcmp(fin.content_version, p->man.content_version) &&
        !strcmp(fin.title_name, p->man.title_name)) {
        r->nothing = 1;   // the dump is the .p5z's: nothing to write
        r->p5z_size = p->size;
        r->volumes = nvol_old;
        r->grown = p->grown;
        if (mp_p5z_unused(p, &r->unused_bytes, e) != 0) goto done;
        rc = 0;
        goto done;
    }

    // ampr_emu.index: every file of the new dump (the runtime the .p5z carries in fakelib/)
    size_t nrows = 0;
    for (size_t i = 0; i < n; i++) {
        const mp_entry *en = mp_source_entry(dump, i);
        if (g[i].role < 0 || i == orig_idx3 || mp_pack_idx3_skip(en->path)) continue;
        rows[nrows].path = en->path;
        rows[nrows].size = (rtrec != SIZE_MAX && i == orig_rt) ? p->files[rtrec].size : en->size;
        rows[nrows].mtime = (rtrec != SIZE_MAX && i == orig_rt) ? p->files[rtrec].mtime : en->mtime;
        nrows++;
    }
    if (rtrec != SIZE_MAX && orig_rt == SIZE_MAX) {
        rows[nrows].path = MP_RUNTIME_PATH;
        rows[nrows].size = p->files[rtrec].size;
        rows[nrows].mtime = p->files[rtrec].mtime;
        nrows++;
    }
    if (mp_idx3_build(rows, nrows, &idx3, &idx3_len, e) != 0) goto done;
    uint8_t idx3_sha[32];
    {
        mp_sha256 s;
        mp_sha256_init(&s);
        mp_sha256_update(&s, idx3, idx3_len);
        mp_sha256_final(&s, idx3_sha);
    }
    int keep_idx3 = xrec != SIZE_MAX && p->files[xrec].size == idx3_len && !memcmp(p->files[xrec].sha, idx3_sha, 32);
    int64_t gen_mtime = 0;
    for (size_t i = 0; i < n; i++)
        if (g[i].role >= 0 && mp_source_entry(dump, i)->mtime > gen_mtime) gen_mtime = mp_source_entry(dump, i)->mtime;

    // Before writing anything: what would stop it.
    uint32_t max_vol = o->max_volumes ? o->max_volumes : P5Z_PLAY_MAX_VOLUMES;
    if (npack_new && nvol_old >= max_vol) {
        r->code = "too_many_volumes";
        mp_err_set(e, "the .p5z has %u volumes already, the most the console takes: make a compact copy of it "
                      "first (maxpack compact)", nvol_old);
        goto done;
    }
    if (nvol_old && !chunks_total) {
        r->code = "no_packs_left";
        mp_err_set(e, "the new dump leaves nothing in the .p5z's packs: pack it again");
        goto done;
    }
    if (!o->force && r->stored_bytes * 2 > r->total_bytes) {
        r->code = "mostly_changed";
        mp_err_set(e, "more than half of the game changed (%llu of %llu bytes): packing the new dump again gives "
                      "a smaller .p5z; to update it anyway, say so (--force)",
                   (unsigned long long)r->stored_bytes, (unsigned long long)r->total_bytes);
        goto done;
    }
    {
        uint64_t need = store_bytes + idx3_len + 32ull * npacked + 8 * chunks_total + 64ull * n;
        need += need / 100 + (64u << 20);
        char *dir = folder_of(p5z);
        uint64_t avail = 0;
        if (dir && mp_disk_free(dir, &avail, NULL) == 0 && avail < need) {
            free(dir);
            r->code = "no_space";
            mp_err_set(e, "not enough room next to %s: it needs %llu bytes more and the disk has %llu", p5z,
                       (unsigned long long)need, (unsigned long long)avail);
            goto done;
        }
        free(dir);
    }

    // 3. The append.
    int busy = 0;
    if (mp_wfile_reopen_locked(p5z, &f, &busy, e) != 0) {
        if (busy) {
            r->code = "busy";
            mp_err_set(e, "another MaxPack is changing %s now", p5z);
        }
        goto done;
    }
    if (mp_wfile_size(f) != old_size) {
        r->code = "changed_meanwhile";
        mp_err_set(e, "%s changed while it was being compared: try again", p5z);
        goto done;
    }
    if (fault("locked", e) != 0) goto done;
    {
        uint8_t foot[32];
        size_t got = 0;
        if (mp_file_pread(p->f, foot, 32, old_size - 32, &got, e) != 0 || got != 32) {
            if (!e->msg[0]) mp_err_set(e, "cannot read the footer of %s", p5z);
            goto done;
        }
        if (p5z_journal_write(p5z, old_size, foot, e) != 0) goto done;
        journal = 1;
    }
    if (fault("journal", e) != 0) goto done;
    if (p5z_out_append(f, o->threads, o->max_size, p->grown + 1, &out, e) != 0) goto done;
    // the records kept as they are: the old volumes, the runtime, the same loose files and originals
    for (size_t i = 0; i < p->nfiles; i++) {
        int role = p->files[i].role;
        if (role == MP_ROLE_VOLUME || role == MP_ROLE_RUNTIME)
            if (p5z_out_keep(&out, p, i, p->files[i].mtime, e) != 0) goto done;
    }
    for (size_t i = 0; i < n; i++) {
        const gent *x = &g[i];
        if (x->role < 0 || x->kind != K_SAME || x->role == MP_ROLE_VOLUME) continue;
        uint32_t fi = p->man_file[x->old];
        if (p5z_out_keep(&out, p, fi, mp_source_entry(dump, i)->mtime, e) != 0) goto done;
    }
    if (keep_idx3 && p5z_out_keep(&out, p, xrec, gen_mtime, e) != 0) goto done;

    // the new and changed loose files
    mp_progress_init(&prog, "Copying the loose files", loose_total, o->progress);
    prog_on = 1;
    for (size_t i = 0; i < n; i++) {
        gent *x = &g[i];
        if (x->role != MP_ROLE_LOOSE || x->kind == K_SAME) continue;
        uint8_t sha[32];
        if (fault("loose file", e) != 0 || mp_cancel_check(e) != 0 ||
            mp_pack_copy_file(dump, i, mp_source_entry(dump, i)->path, MP_ROLE_LOOSE, &out, buf, sha, &prog, e) != 0)
            goto done;
        if (x->hashed && memcmp(sha, x->sha, 32)) {
            mp_err_set(e, "%s changed while it was being read", mp_source_entry(dump, i)->path);
            goto done;
        }
        memcpy(x->sha, sha, 32);
    }
    if (prog.print && loose_total) fprintf(stderr, "\rCopying the loose files: 100.0 %%\n");
    mp_progress_free(&prog);
    prog_on = 0;

    // the new and changed packed files, in new volumes
    gsink gs = {out.zpk, max_vol, 0};
    mp_zpk_sink sink = {gs_begin, gs_write, gs_end, gs_header, &gs};
    if (npack_new) {
        if (out.packs_begin && out.packs_begin(out.ctx, e) != 0) goto done;
        char phase[64];
        snprintf(phase, sizeof phase, "Packing (zstd level %d)", p->man.level);
        mp_progress_init(&prog, phase, pack_total, o->progress);
        prog_on = 1;
        mp_zpk_opts zo = {p->man.level, o->threads, o->volume_limit, &prog, nvol_old};
        psha = malloc(npack_new * sizeof *psha);
        if (!psha) goto oom;
        if (mp_zpk_write_chunks(dump, pack_new, npack_new, &zo, &sink, &ch, psha, e) != 0) {
            if (gs.too_many) r->code = "too_many_volumes";
            goto done;
        }
        if (prog.print) fprintf(stderr, "\r%s: 100.0 %%\n", phase);
        mp_progress_free(&prog);
        prog_on = 0;
        for (size_t k = 0; k < npack_new; k++) memcpy(g[pack_new[k]].sha, psha[k], 32);
    }

    // ampr_emu.index, the new and changed originals
    if (!keep_idx3 && (fault("ampr_emu.index", e) != 0 ||
                       mp_pack_put_buffer(&out, "ampr_emu.index", MP_ROLE_IDX3, idx3, idx3_len, gen_mtime, e) != 0))
        goto done;
    for (size_t i = 0; i < n; i++) {
        gent *x = &g[i];
        if (x->role != MP_ROLE_ORIGINAL || x->kind == K_SAME) continue;
        if (fault("original", e) != 0 ||
            mp_pack_copy_file(dump, i, mp_source_entry(dump, i)->path, MP_ROLE_ORIGINAL, &out, buf, x->sha, NULL, e) != 0)
            goto done;
    }

    // ampr_zstd.index: the same files' chunks where they were, the new ones' in the new volumes
    uint32_t nvol = nvol_old + ch.nvol;
    if (packs_changed && npacked) {
        ents = malloc(npacked * sizeof *ents);
        vsizes = malloc((nvol ? nvol : 1) * sizeof *vsizes);
        if (!ents || !vsizes) goto oom;
        size_t ne = 0, kn = 0;
        for (size_t i = 0; i < n; i++) {
            const gent *x = &g[i];
            if (x->role != MP_ROLE_VOLUME) continue;
            const mp_entry *en = mp_source_entry(dump, i);
            ents[ne].path = en->path;
            ents[ne].size = en->size;
            if (x->kind == K_SAME) {
                long zi = mp_zpk_find(z, en->path);
                uint64_t cnt = 0;
                if (zi < 0 || mp_zpk_size(z, (size_t)zi) != en->size) {
                    mp_err_set(e, "invalid .p5z: %s is not in its packs", en->path);
                    goto done;
                }
                ents[ne].chunks = mp_zpk_file_chunks(z, (size_t)zi, &cnt);
            } else {
                while (pack_new[kn] != i) kn++;
                ents[ne].chunks = ch.chunks + ch.first[kn];
            }
            ne++;
        }
        for (uint32_t v = 0; v < nvol_old; v++) vsizes[v] = mp_zpk_volume_size(z, v);
        for (uint32_t k = 0; k < ch.nvol; k++) vsizes[nvol_old + k] = ch.vol_size[k];
        uint8_t aid[16];
        if (mp_zpk_build_index(ents, ne, vsizes, nvol, p->man.level, p->man.archive_id, &zidx, &zidx_len, aid, e) != 0)
            goto done;
        // the new volumes' headers count every volume; the old ones' stay as they are
        for (uint32_t k = 0; k < ch.nvol; k++) {
            uint8_t h[64];
            mp_zpk_volume_header(nvol_old + k, nvol, aid, ch.vol_size[k], h);
            if (sink.vol_header(sink.ctx, nvol_old + k, h, e) != 0) goto done;
        }
        if (fault("ampr_zstd.index", e) != 0 ||
            mp_pack_put_buffer(&out, "ampr_zstd.index", MP_ROLE_ZPK_INDEX, zidx, zidx_len, gen_mtime, e) != 0)
            goto done;
    } else if (zrec != SIZE_MAX && p5z_out_keep(&out, p, zrec, gen_mtime, e) != 0) {
        goto done;
    }

    // the manifest, the index version 2 and the footer; then to disk, and the journal goes
    for (size_t i = 0, k = 0; i < n; i++)
        if (g[i].role >= 0) {
            if (g[i].kind != K_SAME) memcpy(fin.v[k].sha, g[i].sha, 32);
            k++;
        }
    if (fault("index", e) != 0 || mp_cancel_check(e) != 0 || out.finish(out.ctx, &fin, e) != 0) goto done;
    if (fault("footer", e) != 0 || mp_wfile_sync(f, e) != 0 || fault("synced", e) != 0) goto done;
    p5z_journal_remove(p5z);
    journal = 0;
    r->p5z_size = mp_wfile_size(f);
    r->added_bytes = r->p5z_size - old_size;
    r->volumes = nvol;
    r->volumes_added = ch.nvol;
    r->grown = p->grown + 1;
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "out of memory");
done:
    if (prog_on) mp_progress_free(&prog);
    if (rc != 0 && mp_pack_out_p5z_too_big(&out)) r->code = "too_big";
    if (out.destroy) out.destroy(out.ctx);
    if (f) {
        // what was appended goes, unless this run stopped as a power cut would (tests): then the journal
        // is what undoes it, at the next open
        if (rc != 0 && journal && !g_crashed) {
            mp_err keep = e ? *e : (mp_err){{0}};
            if (mp_wfile_truncate(f, old_size, NULL) == 0 && mp_wfile_sync(f, NULL) == 0)
                p5z_journal_remove(p5z);
            else if (e)
                mp_err_set(e, "%s; what was added could not be taken away yet: the next time MaxPack opens %s "
                              "it will be",
                           keep.msg, p5z);
            if (e && e->msg[0] == 0) *e = keep;
        }
        mp_wfile_close(f, NULL);
    }
    if (rc == 0 && !r->nothing) {
        mp_p5z *q = NULL;
        if (mp_p5z_open(p5z, &q, NULL) == 0) mp_p5z_unused(q, &r->unused_bytes, NULL);
        mp_p5z_close(q);
    }
    if (rc == 0) {
        char *par = malloc(strlen(p5z) + 6);
        if (par) {
            sprintf(par, "%s.par2", p5z);
            r->par2 = mp_path_kind(par) == 1;
        }
        free(par);
    }
    mp_complete_free(&comp);
    free(fin.v);   // its paths are the dump's
    mp_zpk_chunks_free(&ch);
    free(ents);
    free(vsizes);
    free(psha);
    free(idx3);
    free(zidx);
    free(rows);
    free(pack_new);
    free(todo);
    free(matched);
    free(cls);
    free(g);
    free(buf);
    mp_rules_free(rules);
    mp_zpk_close(z);
    mp_p5z_close(p);
    return rc;
}
