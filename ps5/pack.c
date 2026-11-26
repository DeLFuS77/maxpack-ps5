/* The console's test compression for src/pack.rs (POST /api/pack): the MaxPack engine (vendor/maxpack) packs a
 * folder into a .p5z with mp_pack_run and its .p5z output, as `maxpack pack --p5z` does on the PC, and the
 * volumes' records are read back from the file written. C does the setup so Rust never mirrors the layout of
 * mp_pack_opts, mp_pack_out or mp_pack_result. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_p5z_internal.h" /* struct mp_p5z: the volume records' SHA-256, which no public call gives */
#include "mp_pack.h"
#include "mp_parallel.h"

/* What mp_pack_result counts, for the log. */
typedef struct {
    uint64_t loose_files, loose_bytes, packed_files, packed_bytes, stored_bytes, volumes;
} maxpack_pack_counts;

typedef void (*maxpack_pack_progress)(void *ctx, const char *phase, uint64_t done, uint64_t total);

static void set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg && msg[0] ? msg : "the engine gave no reason");
}

/* Packs folder src into the .p5z out, which must not exist: zstd level `level`, `threads` threads for the packs
 * and `loose_threads` for the loose files (mp_pack_out_p5z). progress (NULL: none) gets the engine's phase
 * reports, at most every 100 ms and always at a phase's end, from the engine's threads. 0, or -1 with the reason
 * in err; after -1 the engine has deleted what it wrote of out (its writer removes a .p5z it did not finish).
 * The engine's progress hook and cancel flag are one per process: nothing else of the engine may run a job
 * meanwhile (src/pack.rs refuses the installer's), and the flag is cleared by maxpack_pack_cancel_reset, not here,
 * so a cancel asked before the call is not lost. */
int maxpack_pack_p5z(const char *src, const char *out, int level, unsigned threads, unsigned loose_threads,
                     maxpack_pack_progress progress, void *ctx, maxpack_pack_counts *counts, char *err,
                     size_t errlen) {
    if (counts) memset(counts, 0, sizeof *counts);
    mp_pack_opts o;
    mp_pack_opts_default(&o);
    o.level = level;
    o.threads = threads; /* the default is mp_cpu_count(): 16 on the console, which gives a payload 5 */
    o.progress = 0;      /* no "\r<phase>: NN %" on stderr: the hook reports */
    mp_err e = {{0}};
    if (mp_cancel_check(&e) != 0) {
        set_err(err, errlen, e.msg);
        return -1;
    }
    mp_pack_out po;
    if (mp_pack_out_p5z(out, loose_threads, &po, &e) != 0) {
        set_err(err, errlen, e.msg);
        return -1;
    }
    mp_progress_hook hook = {progress, ctx};
    mp_progress_set_hook(progress ? &hook : NULL);
    mp_pack_result r;
    int rc = mp_pack_run(src, &po, &o, &r, &e);
    mp_progress_set_hook(NULL);
    po.destroy(po.ctx); /* joins its threads; deletes the .p5z when it was not finished */
    if (counts) {
        counts->loose_files = r.loose_files;
        counts->loose_bytes = r.loose_bytes;
        counts->packed_files = r.packed_files;
        counts->packed_bytes = r.packed_bytes;
        counts->stored_bytes = r.stored_bytes;
        counts->volumes = r.volumes;
    }
    if (rc != 0) set_err(err, errlen, e.msg);
    return rc;
}

/* mp_cancel_request: the running pack stops at its next check (-1, "cancelled"). */
void maxpack_pack_cancel(void) { mp_cancel_request(); }

/* mp_cancel_reset, once the pack has returned (or never started): the installer's checks read the same flag. */
void maxpack_pack_cancel_reset(void) { mp_cancel_reset(); }

static void hex(char *dst, const uint8_t *src, size_t n) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        dst[2 * i] = d[src[i] >> 4];
        dst[2 * i + 1] = d[src[i] & 15];
    }
    dst[2 * n] = 0;
}

/* A volume record's name is put in the JSON as it is: only what a volume is ever named. */
static int plain_name(const char *s) {
    if (!s || !*s || strlen(s) > 64) return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.')) return 0;
    return 1;
}

/* What the index of the .p5z at path records of its packs, as JSON: {"archive_id":"<32 hex>","level":N,
 * "volumes":[{"name":"ampr_zstd-000.pak","size":N,"sha256":"<64 hex>"},...]}, the header's archive_id and
 * level and the volume records in their order (each SHA-256 is of the volume's bytes from 64 to its end).
 * malloc'd (maxpack_free frees it); NULL with the reason in err. */
char *maxpack_p5z_sums(const char *path, char *err, size_t errlen) {
    mp_err e = {{0}};
    mp_p5z *p = NULL;
    if (mp_p5z_open(path, &p, &e) != 0) {
        set_err(err, errlen, e.msg);
        return NULL;
    }
    const mp_pack_manifest *m = mp_p5z_manifest(p);
    size_t nvol = 0;
    for (size_t i = 0; i < p->nfiles; i++)
        if (p->files[i].role == MP_ROLE_VOLUME) nvol++;
    size_t cap = 96 + nvol * 192, len = 0;
    char *out = malloc(cap);
    if (!out) {
        mp_p5z_close(p);
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    char aid[33], sha[65];
    hex(aid, m->archive_id, sizeof m->archive_id);
    len += (size_t)snprintf(out + len, cap - len, "{\"archive_id\":\"%s\",\"level\":%d,\"volumes\":[", aid, m->level);
    size_t k = 0;
    for (size_t i = 0; i < p->nfiles; i++) {
        const p5z_file *f = &p->files[i];
        if (f->role != MP_ROLE_VOLUME) continue;
        if (!plain_name(f->path)) {
            free(out);
            mp_p5z_close(p);
            set_err(err, errlen, "a volume record with an unexpected name");
            return NULL;
        }
        hex(sha, f->sha, sizeof f->sha);
        len += (size_t)snprintf(out + len, cap - len, "%s{\"name\":\"%s\",\"size\":%llu,\"sha256\":\"%s\"}", k++ ? "," : "",
                                f->path, (unsigned long long)f->size, sha);
    }
    snprintf(out + len, cap - len, "]}");
    mp_p5z_close(p);
    return out;
}

void maxpack_free(void *p) { free(p); }
