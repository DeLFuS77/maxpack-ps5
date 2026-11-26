// SPDX-License-Identifier: GPL-3.0-or-later
// Packing core: decides what goes loose and what into packs, validates everything before writing and
// sends the files to the output in this order: loose files, packs, ampr_emu.index, runtime,
// replaced originals and, last, ampr_zstd.index (without it the runtime mounts nothing). With
// leave_out_leftovers, what dump tools leave behind that the game's own list proves is not the
// game's goes nowhere: not loose, not packed, not in either index nor in the manifest.
#include "mp_pack.h"
#include "mp_param.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_dlc.h"
#include "mp_hash.h"
#include "mp_idx3.h"
#include "mp_platform.h"
#include "mp_rules.h"

#define COPY_PIECE (4u << 20)
#define RUNTIME_MAX (64u << 20)
#define RUNTIME_PATH MP_RUNTIME_PATH

void mp_pack_opts_default(mp_pack_opts *o) {
    memset(o, 0, sizeof *o);
    o->level = 19;
    o->threads = mp_cpu_count();
    o->progress = 1;
    o->volume_limit = MP_ZPK_VOLUME_LIMIT;
}

void mp_pack_manifest_free(mp_pack_manifest *m) {
    if (!m || !m->v) return;
    for (size_t i = 0; i < m->n; i++) free(m->v[i].path);
    free(m->v);
    m->v = NULL;
    m->n = 0;
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

static int has_marker(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 8 <= n; i++)
        if (p[i] == 'A' && !memcmp(p + i, "AMPRZPK1", 8)) return 1;
    return 0;
}

int mp_pack_idx3_skip(const char *path) { return ieq(path, "ampr_commands.bin") || ieq(path, "apr_emu.log"); }

int mp_pack_already_packed(const char *path) {
    return ieq(path, "ampr_zstd.index") || mp_glob_match("ampr_zstd-*.pak", path);
}

int mp_pack_copy_file(mp_source *src, size_t i, const char *rel, int role, mp_pack_out *out, uint8_t *buf,
                      uint8_t sha[32], mp_progress *prog, mp_err *e) {
    const mp_entry *en = mp_source_entry(src, i);
    if (out->file_begin(out->ctx, rel, role, en->size, en->mtime, e) != 0) return -1;
    mp_sha256 s;
    mp_sha256_init(&s);
    for (uint64_t off = 0; off < en->size; off += COPY_PIECE) {
        size_t k = en->size - off < COPY_PIECE ? (size_t)(en->size - off) : COPY_PIECE;
        if (mp_cancel_check(e) != 0) return -1;
        if (mp_source_read(src, i, off, buf, k, e) != 0 || out->file_write(out->ctx, buf, k, e) != 0) return -1;
        mp_sha256_update(&s, buf, k);
        mp_progress_add(prog, k);
        mp_progress_maybe_print(prog);
    }
    mp_sha256_final(&s, sha);
    return out->file_end(out->ctx, sha, e);
}

int mp_pack_put_buffer(mp_pack_out *out, const char *rel, int role, const uint8_t *p, size_t n, int64_t mtime,
                       mp_err *e) {
    uint8_t sha[32];
    mp_sha256 s;
    mp_sha256_init(&s);
    mp_sha256_update(&s, p, n);
    mp_sha256_final(&s, sha);
    if (out->file_begin(out->ctx, rel, role, n, mtime, e) != 0) return -1;
    if (n && out->file_write(out->ctx, p, n, e) != 0) return -1;
    return out->file_end(out->ctx, sha, e);
}

static int read_whole(mp_source *src, size_t i, size_t max, uint8_t **out, size_t *len, mp_err *e) {
    const mp_entry *en = mp_source_entry(src, i);
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
    if (mp_source_read(src, i, 0, b, (size_t)en->size, e) != 0) {
        free(b);
        return -1;
    }
    *out = b;
    *len = (size_t)en->size;
    return 0;
}

static int read_runtime(const char *path, uint8_t **out, size_t *len, int64_t *mtime, mp_err *e) {
    uint64_t size = 0;
    *out = NULL;
    if (mp_stat(path, &size, mtime) != 0 || mp_path_kind(path) != 1) {
        mp_err_set(e, "the runtime does not exist: %s", path);
        return -1;
    }
    if (size > RUNTIME_MAX || size == 0) {
        mp_err_set(e, "the runtime %s does not look like a libSceAmpr.sprx (%llu bytes)", path, (unsigned long long)size);
        return -1;
    }
    mp_file *f = NULL;
    size_t got = 0;
    uint8_t *b = malloc((size_t)size);
    if (!b || mp_file_open_read(path, &f, e) != 0) {
        free(b);
        if (!e || !e->msg[0]) mp_err_set(e, "out of memory");
        return -1;
    }
    int rc = mp_file_pread(f, b, (size_t)size, 0, &got, e);
    mp_file_close(f);
    if (rc != 0 || got != size) {
        free(b);
        if (rc == 0) mp_err_set(e, "could not read the whole runtime %s", path);
        return -1;
    }
    *out = b;
    *len = (size_t)size;
    return 0;
}

int mp_pack_run(const char *src_path, mp_pack_out *out, const mp_pack_opts *o, mp_pack_result *r, mp_err *e) {
    memset(r, 0, sizeof *r);
    mp_source *src = NULL;
    mp_rules *rules = NULL;
    mp_idx3_row *rows = NULL;
    uint32_t *packable = NULL;
    int *role = NULL;
    uint8_t *cls = NULL;
    mp_pack_manifest fin;
    memset(&fin, 0, sizeof fin);
    uint8_t *idx3 = NULL, *zidx = NULL, *rt = NULL, *buf = NULL, (*psha)[32] = NULL;
    size_t idx3_len = 0, zidx_len = 0, rt_len = 0;
    mp_pack_manifest man;
    memset(&man, 0, sizeof man);
    int rc = -1;

    if (mp_source_open(src_path, &src, e) != 0) return -1;
    size_t n = 0;
    // the DLC go in as files of the game: addcont0/... and the dlc_emu.ini MaxPack writes
    if (o->ndlc) {
        mp_dlc_list dl;
        if (mp_dlc_attach(&src, o->dlc, o->ndlc, &dl, &r->dlc_error, e) != 0) goto done;
        for (size_t k = 0; k < dl.n; k++) {
            if (dl.v[k].data) {
                r->dlc_data++;
                r->dlc_bytes += dl.v[k].bytes;
            } else {
                r->dlc_license++;
            }
        }
        mp_dlc_list_free(&dl);
    }
    n = mp_source_count(src);
    if (n == 0) {
        mp_err_set(e, "the source has no files: %s", src_path);
        goto done;
    }
    rules = mp_rules_new();
    rows = calloc(n + 1, sizeof *rows);
    packable = malloc(n * sizeof *packable);
    role = malloc(n * sizeof *role);
    man.v = calloc(n, sizeof *man.v);
    buf = malloc(COPY_PIECE);
    cls = malloc(n);
    if (!rules || !rows || !packable || !role || !man.v || !buf || !cls) goto oom;
    for (size_t k = 0; k < o->nkeep; k++)
        if (mp_rules_add_keep_loose(rules, o->keep_loose[k], e) != 0) goto done;

    // 0. The source against its list of the original package: what to say of it and, when asked,
    // the leftovers that list proves are not the game's.
    if (mp_complete_source(src, &r->complete, cls, e) != 0) goto done;
    mp_complete_counts_only(&r->complete);
    int drop = o->leave_out_leftovers && mp_complete_proves_leftovers(&r->complete);
#define LEFT_OUT(i) (drop && cls[i] == MP_CF_LEFTOVER)

    // 1. Special files in the source.
    size_t orig_idx3 = SIZE_MAX, orig_rt = SIZE_MAX, param = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        const char *p = mp_source_entry(src, i)->path;
        if (mp_pack_already_packed(p)) {
            // A loose volume without its index too: it would clash with the new volumes.
            mp_err_set(e, "%s is already packed (it has %s); repacking will come later", src_path, p);
            goto done;
        }
        if (ieq(p, "ampr_emu.index")) orig_idx3 = i;
        if (ieq(p, RUNTIME_PATH)) orig_rt = i;
        if (ieq(p, "sce_sys/param.json")) param = i;
    }
    int64_t rt_mtime = 0;
    if (o->runtime && read_runtime(o->runtime, &rt, &rt_len, &rt_mtime, e) != 0) goto done;

    // 2. Role of each file and rows of ampr_emu.index (what will be left in the folder).
    size_t nrows = 0, npack = 0;
    for (size_t i = 0; i < n; i++) {
        const mp_entry *en = mp_source_entry(src, i);
        if (LEFT_OUT(i)) {
            role[i] = -1;
            r->left_out_files++;
            r->left_out_bytes += en->size;
            continue;
        }
        man.v[i].path = mp_strdup(en->path);
        if (!man.v[i].path) goto oom;
        man.v[i].size = en->size;
        man.v[i].mtime = en->mtime;
        if (i == orig_idx3 || (rt && i == orig_rt)) {
            role[i] = MP_ROLE_ORIGINAL;
            man.v[i].flags = MP_MANIFEST_REPLACED;
        } else if (mp_rules_classify(rules, en->path, en->size) == MP_LOOSE) {
            role[i] = MP_ROLE_LOOSE;
        } else {
            role[i] = MP_ROLE_VOLUME;
            man.v[i].flags = MP_MANIFEST_PACKED;
            packable[npack++] = (uint32_t)i;
        }
        if (i == orig_idx3 || mp_pack_idx3_skip(en->path)) continue;
        rows[nrows].path = en->path;
        rows[nrows].size = (rt && i == orig_rt) ? rt_len : en->size;
        rows[nrows].mtime = (rt && i == orig_rt) ? rt_mtime : en->mtime;
        nrows++;
    }
    if (rt && orig_rt == SIZE_MAX) {
        rows[nrows].path = RUNTIME_PATH;
        rows[nrows].size = rt_len;
        rows[nrows].mtime = rt_mtime;
        nrows++;
    }
    // Validates paths and case for the whole game before writing anything.
    if (mp_idx3_build(rows, nrows, &idx3, &idx3_len, e) != 0) goto done;

    // 3. Runtime: does it read zstd packs?
    if (rt) {
        r->runtime_without_zstd = !has_marker(rt, rt_len);
    } else if (orig_rt != SIZE_MAX) {
        uint8_t *old = NULL;
        size_t ol = 0;
        if (read_whole(src, orig_rt, RUNTIME_MAX, &old, &ol, e) != 0) goto done;
        r->runtime_without_zstd = !has_marker(old, ol);
        free(old);
    } else {
        r->runtime_without_zstd = 1;
    }
    if (param != SIZE_MAX) {
        uint8_t *pj = NULL;
        size_t pl = 0;
        if (read_whole(src, param, 1u << 20, &pj, &pl, e) == 0) {
            mp_param_read((const char *)pj, pl, man.title_id, man.content_version, man.title_name);
            free(pj);
        }
        memcpy(r->title_id, man.title_id, sizeof r->title_id);
        if (e) e->msg[0] = 0;   // an unreadable param.json does not prevent packing
    }
    int64_t gen_mtime = 0;   // date of the generated files: the game's most recent
    for (size_t i = 0; i < n; i++)
        if (!LEFT_OUT(i) && mp_source_entry(src, i)->mtime > gen_mtime) gen_mtime = mp_source_entry(src, i)->mtime;

    // 4. Loose files.
    mp_progress prog;
    uint64_t loose_total = 0;
    for (size_t i = 0; i < n; i++)
        if (role[i] == MP_ROLE_LOOSE) loose_total += mp_source_entry(src, i)->size;
    mp_progress_init(&prog, "Copying the loose files", loose_total, o->progress);
    for (size_t i = 0; i < n && rc == -1; i++) {
        if (role[i] != MP_ROLE_LOOSE) continue;
        if (mp_cancel_check(e) != 0 ||
            mp_pack_copy_file(src, i, man.v[i].path, MP_ROLE_LOOSE, out, buf, man.v[i].sha, &prog, e) != 0) {
            mp_progress_free(&prog);
            goto done;
        }
        r->loose_files++;
        r->loose_bytes += man.v[i].size;
    }
    if (prog.print && loose_total) fprintf(stderr, "\rCopying the loose files: 100.0 %%\n");
    mp_progress_free(&prog);

    // 5. Packs.
    mp_zpk_stats st;
    memset(&st, 0, sizeof st);
    if (npack) {
        if (out->packs_begin && out->packs_begin(out->ctx, e) != 0) goto done;
        uint64_t ptotal = 0;
        for (size_t k = 0; k < npack; k++) ptotal += mp_source_entry(src, packable[k])->size;
        char phase[64];
        snprintf(phase, sizeof phase, "Packing (zstd level %d)", o->level);
        mp_progress_init(&prog, phase, ptotal, o->progress);
        mp_zpk_opts zo = {o->level, o->threads, o->volume_limit, &prog};
        psha = malloc(npack * sizeof *psha);
        int zr = psha ? mp_zpk_write(src, packable, npack, &zo, &out->zpk, &zidx, &zidx_len, psha, &st, e) : -1;
        if (prog.print) fprintf(stderr, "\r%s: 100.0 %%\n", phase);
        mp_progress_free(&prog);
        if (zr != 0) {
            if (!psha) mp_err_set(e, "out of memory");
            goto done;
        }
        for (size_t k = 0; k < npack; k++) memcpy(man.v[packable[k]].sha, psha[k], 32);
        r->packed_files = npack;
        r->packed_bytes = st.logical;
        r->stored_bytes = st.stored;
        r->volumes = st.volumes;
        memcpy(man.archive_id, st.archive_id, 16);
    }

    // 6. ampr_emu.index, runtime and replaced originals.
    if (mp_cancel_check(e) != 0) goto done;
    if (mp_pack_put_buffer(out, "ampr_emu.index", MP_ROLE_IDX3, idx3, idx3_len, gen_mtime, e) != 0) goto done;
    if (rt && mp_pack_put_buffer(out, RUNTIME_PATH, MP_ROLE_RUNTIME, rt, rt_len, rt_mtime, e) != 0) goto done;
    for (size_t i = 0; i < n; i++)
        if (role[i] == MP_ROLE_ORIGINAL &&
            mp_pack_copy_file(src, i, man.v[i].path, MP_ROLE_ORIGINAL, out, buf, man.v[i].sha, NULL, e) != 0)
            goto done;

    // 7. ampr_zstd.index last.
    if (npack && mp_pack_put_buffer(out, "ampr_zstd.index", MP_ROLE_ZPK_INDEX, zidx, zidx_len, gen_mtime, e) != 0)
        goto done;
    // the manifest has what went, in path order: without what was left out
    fin = man;
    fin.v = malloc((n ? n : 1) * sizeof *fin.v);
    if (!fin.v) goto oom;
    fin.n = 0;
    for (size_t i = 0; i < n; i++)
        if (man.v[i].path) fin.v[fin.n++] = man.v[i];
    fin.level = o->level;
    if (mp_cancel_check(e) != 0 || out->finish(out->ctx, &fin, e) != 0) goto done;
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "out of memory");
done:
#undef LEFT_OUT
    free(fin.v);   // its entries are man's
    free(cls);
    man.n = n;
    mp_pack_manifest_free(&man);
    free(idx3);
    free(zidx);
    free(rt);
    free(buf);
    free(psha);
    free(role);
    free(packable);
    free(rows);
    mp_rules_free(rules);
    mp_source_close(src);
    return rc;
}
