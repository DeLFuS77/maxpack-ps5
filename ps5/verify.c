/* The console's verify of a .p5z for src/verify.rs and src/receive.rs: the MaxPack engine's quick verify
 * (mp_p5z_verify_quick, `maxpack verify --quick`), its report as JSON, and its own full verify (mp_verify_p5z,
 * what `maxpack verify` runs on the PC) with the engine's progress hook and cancel flag. C does the calls so Rust
 * never mirrors the layout of mp_err, mp_qv_report, mp_verify_result or mp_progress_hook. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_pack.h"
#include "mp_parallel.h"
#include "mp_verify_quick.h"

typedef void (*maxpack_quick_progress)(void *ctx, uint64_t done, uint64_t total);
typedef int (*maxpack_quick_cancel)(void *ctx);

/* The length of s, at most max (a fixed buffer that may fill without a NUL). */
static size_t blen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

/* n bytes of src as lowercase hex at dst (2n characters and a NUL); the characters written. */
static size_t hex(char *dst, const uint8_t *src, size_t n) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        dst[2 * i] = d[src[i] >> 4];
        dst[2 * i + 1] = d[src[i] & 15];
    }
    dst[2 * n] = 0;
    return 2 * n;
}

/* The engine's quick verify of the .p5z at p5z, as JSON (src/verify.rs reads it: maxpack_logic::verify::parse_quick):
 * {"result":0,"ok":true|false,"bytes":N,"total":N,"nbad":N,"nlisted":N,"version":N,"volumes":N,"files":N,
 *  "bad":[{"kind":1|2,"volume":N,"path_hex":"...","expected":"<64 hex>","found":"<64 hex>"|"","why":"..."}]}
 * when it checked; {"result":-1,"error_hex":"..."} when it could not (the engine's message); {"result":-2} when
 * cancel said stop. Paths and the message are hex, so any bytes in them survive. progress (NULL: none) gets the
 * bytes read after each piece (4 MiB at most) and once at the end; cancel (NULL: never) is asked before each
 * piece; both get ctx. It uses no global of the engine: no cancel flag, no progress hook. malloc'd
 * (maxpack_verify_free), NULL when out of memory. */
char *maxpack_verify_quick(const char *p5z, maxpack_quick_progress progress, maxpack_quick_cancel cancel, void *ctx) {
    mp_qv_progress pr = {progress, ctx};
    mp_qv_cancel ca = {cancel, ctx};
    mp_qv_report r;
    mp_err e = {{0}};
    int rc = mp_p5z_verify_quick(p5z, progress ? &pr : NULL, cancel ? &ca : NULL, &r, &e);
    size_t listed = rc == MP_QV_CHECKED && r.bad ? r.nlisted : 0;
    size_t cap = 512 + 2 * sizeof e.msg + listed * (2 * sizeof r.bad[0].path + 4 * sizeof r.bad[0].expected + 192);
    char *out = malloc(cap);
    if (!out) {
        mp_qv_report_free(&r);
        return NULL;
    }
    size_t len = 0;
    if (rc == MP_QV_CANCELLED) {
        snprintf(out, cap, "{\"result\":-2}");
    } else if (rc != MP_QV_CHECKED) {
        len += (size_t)snprintf(out, cap, "{\"result\":-1,\"error_hex\":\"");
        len += hex(out + len, (const uint8_t *)e.msg, blen(e.msg, sizeof e.msg));
        snprintf(out + len, cap - len, "\"}");
    } else {
        len += (size_t)snprintf(out, cap,
                                "{\"result\":0,\"ok\":%s,\"bytes\":%llu,\"total\":%llu,\"nbad\":%llu,\"nlisted\":%llu,"
                                "\"version\":%u,\"volumes\":%u,\"files\":%u,\"bad\":[",
                                r.ok ? "true" : "false", (unsigned long long)r.bytes, (unsigned long long)r.total,
                                (unsigned long long)r.nbad, (unsigned long long)listed, (unsigned)r.version,
                                (unsigned)r.volumes, (unsigned)r.files);
        for (size_t i = 0; i < listed; i++) {
            const mp_qv_bad *b = &r.bad[i];
            len += (size_t)snprintf(out + len, cap - len, "%s{\"kind\":%d,\"volume\":%u,\"path_hex\":\"", i ? "," : "",
                                    b->kind, (unsigned)b->volume);
            len += hex(out + len, (const uint8_t *)b->path, blen(b->path, sizeof b->path));
            len += (size_t)snprintf(out + len, cap - len, "\",\"expected\":\"");
            len += hex(out + len, b->expected, sizeof b->expected);
            len += (size_t)snprintf(out + len, cap - len, "\",\"found\":\"");
            if (b->has_found) len += hex(out + len, b->found, sizeof b->found);
            len += (size_t)snprintf(out + len, cap - len, "\",\"why\":\"");
            /* The engine's own words ("sha256", "header", "data", "read"): only letters, digits and _ go in. */
            for (size_t k = 0; k < sizeof b->why && b->why[k]; k++) {
                char c = b->why[k];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') out[len++] = c;
            }
            len += (size_t)snprintf(out + len, cap - len, "\"}");
        }
        snprintf(out + len, cap - len, "]}");
    }
    mp_qv_report_free(&r);
    return out;
}

void maxpack_verify_free(char *p) { free(p); }

/* What mp_verify_result counts (its own struct, not the engine's). */
typedef struct {
    uint64_t files_ok, files_bad, bytes_checked;
} maxpack_verify_counts;

typedef void (*maxpack_verify_progress)(void *ctx, const char *phase, uint64_t done, uint64_t total);

static void set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg && msg[0] ? msg : "the engine gave no reason");
}

/* Checks every stored and packed file of the .p5z at p5z against the SHA-256 its manifest records, with
 * `threads` threads; progress (NULL: none) gets the engine's reports of the bytes checked, at most every 100 ms
 * and always at the end, from the engine's threads. 0 when it could check (counts says how many files matched
 * and how many did not; the engine lists the failures on stderr only), or -1 with the reason in err ("cancelled"
 * after maxpack_verify_cancel). The engine's progress hook and cancel flag are one per process: nothing else of
 * the engine may run a job meanwhile (src/jobs.rs), and the flag is cleared by maxpack_verify_cancel_reset, not
 * here, so a cancel asked before the call is not lost. */
int maxpack_verify_full(const char *p5z, unsigned threads, maxpack_verify_progress progress, void *ctx,
                        maxpack_verify_counts *counts, char *err, size_t errlen) {
    if (counts) memset(counts, 0, sizeof *counts);
    mp_err e = {{0}};
    if (mp_cancel_check(&e) != 0) {
        set_err(err, errlen, e.msg);
        return -1;
    }
    mp_progress_hook hook = {progress, ctx};
    mp_progress_set_hook(progress ? &hook : NULL);
    mp_verify_result r;
    memset(&r, 0, sizeof r);
    int rc = mp_verify_p5z(p5z, threads ? threads : 1, 0, &r, &e);
    mp_progress_set_hook(NULL);
    if (counts) {
        counts->files_ok = r.files_ok;
        counts->files_bad = r.files_bad;
        counts->bytes_checked = r.bytes_checked;
    }
    if (rc != 0) set_err(err, errlen, e.msg);
    return rc;
}

/* mp_cancel_request: the running verify stops at its next check (-1, "cancelled"). */
void maxpack_verify_cancel(void) { mp_cancel_request(); }

/* mp_cancel_reset, once the verify has returned (or never started): the installer's checks read the same flag. */
void maxpack_verify_cancel_reset(void) { mp_cancel_reset(); }
