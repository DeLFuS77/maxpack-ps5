// SPDX-License-Identifier: GPL-3.0-or-later
// Núcleo del empaquetado: decide qué va suelto y qué en packs, valida todo antes de escribir y
// manda los archivos a la salida en este orden: sueltos, packs, ampr_emu.index, runtime,
// originales reemplazados y, el último, ampr_zstd.index (sin él el runtime no monta nada).
#include "mp_pack.h"
#include "mp_param.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_idx3.h"
#include "mp_platform.h"
#include "mp_rules.h"

#define COPY_PIECE (4u << 20)
#define RUNTIME_MAX (64u << 20)
#define RUNTIME_PATH "fakelib/libSceAmpr.sprx"

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

// Copia un archivo del origen a la salida en trozos, calculando su SHA-256.
static int copy_file(mp_source *src, size_t i, const char *rel, int role, mp_pack_out *out, uint8_t *buf,
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

static int put_buffer(mp_pack_out *out, const char *rel, int role, const uint8_t *p, size_t n, int64_t mtime,
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
        mp_err_set(e, "%s es demasiado grande (%llu bytes)", en->path, (unsigned long long)en->size);
        return -1;
    }
    uint8_t *b = malloc((size_t)en->size + 1);
    if (!b) {
        mp_err_set(e, "sin memoria");
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
        mp_err_set(e, "no existe el runtime: %s", path);
        return -1;
    }
    if (size > RUNTIME_MAX || size == 0) {
        mp_err_set(e, "el runtime %s no parece un libSceAmpr.sprx (%llu bytes)", path, (unsigned long long)size);
        return -1;
    }
    mp_file *f = NULL;
    size_t got = 0;
    uint8_t *b = malloc((size_t)size);
    if (!b || mp_file_open_read(path, &f, e) != 0) {
        free(b);
        if (!e || !e->msg[0]) mp_err_set(e, "sin memoria");
        return -1;
    }
    int rc = mp_file_pread(f, b, (size_t)size, 0, &got, e);
    mp_file_close(f);
    if (rc != 0 || got != size) {
        free(b);
        if (rc == 0) mp_err_set(e, "no se pudo leer entero el runtime %s", path);
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
    uint8_t *idx3 = NULL, *zidx = NULL, *rt = NULL, *buf = NULL, (*psha)[32] = NULL;
    size_t idx3_len = 0, zidx_len = 0, rt_len = 0;
    mp_pack_manifest man;
    memset(&man, 0, sizeof man);
    int rc = -1;

    if (mp_source_open(src_path, &src, e) != 0) return -1;
    size_t n = mp_source_count(src);
    if (n == 0) {
        mp_err_set(e, "el origen no tiene archivos: %s", src_path);
        goto done;
    }
    rules = mp_rules_new();
    rows = calloc(n + 1, sizeof *rows);
    packable = malloc(n * sizeof *packable);
    role = malloc(n * sizeof *role);
    man.v = calloc(n, sizeof *man.v);
    buf = malloc(COPY_PIECE);
    if (!rules || !rows || !packable || !role || !man.v || !buf) goto oom;
    for (size_t k = 0; k < o->nkeep; k++)
        if (mp_rules_add_keep_loose(rules, o->keep_loose[k], e) != 0) goto done;

    // 1. Archivos especiales del origen.
    size_t orig_idx3 = SIZE_MAX, orig_rt = SIZE_MAX, param = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        const char *p = mp_source_entry(src, i)->path;
        if (ieq(p, "ampr_zstd.index") || mp_glob_match("ampr_zstd-*.pak", p)) {
            // Un volumen suelto sin su índice también: chocaría con los volúmenes nuevos.
            mp_err_set(e, "%s ya está empaquetado (tiene %s); reempaquetar llegará más adelante", src_path, p);
            goto done;
        }
        if (ieq(p, "ampr_emu.index")) orig_idx3 = i;
        if (ieq(p, RUNTIME_PATH)) orig_rt = i;
        if (ieq(p, "sce_sys/param.json")) param = i;
    }
    int64_t rt_mtime = 0;
    if (o->runtime && read_runtime(o->runtime, &rt, &rt_len, &rt_mtime, e) != 0) goto done;

    // 2. Papel de cada archivo y filas de ampr_emu.index (lo que quedará en la carpeta).
    size_t nrows = 0, npack = 0;
    for (size_t i = 0; i < n; i++) {
        const mp_entry *en = mp_source_entry(src, i);
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
        if (i == orig_idx3 || ieq(en->path, "ampr_commands.bin") || ieq(en->path, "apr_emu.log")) continue;
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
    // Valida rutas y mayúsculas de todo el juego antes de escribir nada.
    if (mp_idx3_build(rows, nrows, &idx3, &idx3_len, e) != 0) goto done;

    // 3. Runtime: ¿lee packs zstd?
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
        if (e) e->msg[0] = 0;   // un param.json ilegible no impide empaquetar
    }
    int64_t gen_mtime = 0;   // fecha de los archivos que se generan: la más reciente del juego
    for (size_t i = 0; i < n; i++)
        if (mp_source_entry(src, i)->mtime > gen_mtime) gen_mtime = mp_source_entry(src, i)->mtime;

    // 4. Sueltos.
    mp_progress prog;
    uint64_t loose_total = 0;
    for (size_t i = 0; i < n; i++)
        if (role[i] == MP_ROLE_LOOSE) loose_total += mp_source_entry(src, i)->size;
    mp_progress_init(&prog, "Copiando los sueltos", loose_total, o->progress);
    for (size_t i = 0; i < n && rc == -1; i++) {
        if (role[i] != MP_ROLE_LOOSE) continue;
        if (mp_cancel_check(e) != 0 ||
            copy_file(src, i, man.v[i].path, MP_ROLE_LOOSE, out, buf, man.v[i].sha, &prog, e) != 0) {
            mp_progress_free(&prog);
            goto done;
        }
        r->loose_files++;
        r->loose_bytes += man.v[i].size;
    }
    if (prog.print && loose_total) fprintf(stderr, "\rCopiando los sueltos: 100.0 %%\n");
    mp_progress_free(&prog);

    // 5. Packs.
    mp_zpk_stats st;
    memset(&st, 0, sizeof st);
    if (npack) {
        uint64_t ptotal = 0;
        for (size_t k = 0; k < npack; k++) ptotal += mp_source_entry(src, packable[k])->size;
        char phase[64];
        snprintf(phase, sizeof phase, "Empaquetando (zstd nivel %d)", o->level);
        mp_progress_init(&prog, phase, ptotal, o->progress);
        mp_zpk_opts zo = {o->level, o->threads, o->volume_limit, &prog};
        psha = malloc(npack * sizeof *psha);
        int zr = psha ? mp_zpk_write(src, packable, npack, &zo, &out->zpk, &zidx, &zidx_len, psha, &st, e) : -1;
        if (prog.print) fprintf(stderr, "\r%s: 100.0 %%\n", phase);
        mp_progress_free(&prog);
        if (zr != 0) {
            if (!psha) mp_err_set(e, "sin memoria");
            goto done;
        }
        for (size_t k = 0; k < npack; k++) memcpy(man.v[packable[k]].sha, psha[k], 32);
        r->packed_files = npack;
        r->packed_bytes = st.logical;
        r->stored_bytes = st.stored;
        r->volumes = st.volumes;
        memcpy(man.archive_id, st.archive_id, 16);
    }

    // 6. ampr_emu.index, runtime y originales reemplazados.
    if (mp_cancel_check(e) != 0) goto done;
    if (put_buffer(out, "ampr_emu.index", MP_ROLE_IDX3, idx3, idx3_len, gen_mtime, e) != 0) goto done;
    if (rt && put_buffer(out, RUNTIME_PATH, MP_ROLE_RUNTIME, rt, rt_len, rt_mtime, e) != 0) goto done;
    for (size_t i = 0; i < n; i++)
        if (role[i] == MP_ROLE_ORIGINAL &&
            copy_file(src, i, man.v[i].path, MP_ROLE_ORIGINAL, out, buf, man.v[i].sha, NULL, e) != 0)
            goto done;

    // 7. ampr_zstd.index el último.
    if (npack && put_buffer(out, "ampr_zstd.index", MP_ROLE_ZPK_INDEX, zidx, zidx_len, gen_mtime, e) != 0)
        goto done;
    man.n = n;
    man.level = o->level;
    if (mp_cancel_check(e) != 0 || out->finish(out->ctx, &man, e) != 0) goto done;
    rc = 0;
    goto done;
oom:
    mp_err_set(e, "sin memoria");
done:
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
