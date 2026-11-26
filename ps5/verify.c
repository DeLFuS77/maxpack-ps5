/* The console's verify of a .p5z for src/verify.rs and src/receive.rs: the MaxPack engine's own full verify
 * (mp_verify_p5z, what `maxpack verify` runs on the PC) with the engine's progress hook, and its cancel flag.
 * C does the call so Rust never mirrors the layout of mp_err, mp_verify_result or mp_progress_hook. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mp_pack.h"
#include "mp_parallel.h"

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
