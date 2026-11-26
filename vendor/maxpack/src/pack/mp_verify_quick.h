// SPDX-License-Identifier: GPL-3.0-or-later
// The quick verify of a .p5z: whether its bytes are the ones it was written with, at the speed of the disk,
// without decompressing the packs. It checks the header, the footer and the CRC of the index (mp_p5z_open,
// which reads a .p5z whose append stopped as it was before it, as mp_play_open does), then every record the
// index lists against the SHA-256 it records: each volume over [64, end) and its 64-byte header against its
// own CRC, and each file stored in the .p5z (the loose files, decompressed when they are stored compressed;
// they are small). What it does not check is what only the full verify does (mp_verify_p5z): that each packed
// file decompresses to the bytes of the original dump. It works on .p5z files as packed (index version 1) and
// grown (version 2). Single-threaded, plain C on the engine's platform layer, so the console builds it too.
// Format in docs/p5z-format.md.
#ifndef MP_VERIFY_QUICK_H
#define MP_VERIFY_QUICK_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

// Progress, in bytes of the records' regions read: done of total. Called after each piece read (4 MiB at
// most) and once more with done == total at the end. NULL: no progress.
typedef struct {
    void (*fn)(void *ctx, uint64_t done, uint64_t total);
    void *ctx;
} mp_qv_progress;

// Asked before each piece is read: non-zero stops the verify (mp_p5z_verify_quick returns MP_QV_CANCELLED).
// NULL: it is never cancelled.
typedef struct {
    int (*fn)(void *ctx);
    void *ctx;
} mp_qv_cancel;

#define MP_QV_VOLUME 1   // a volume of the packs: ampr_zstd-NNN.pak
#define MP_QV_FILE 2     // a file stored in the .p5z: a loose file of the game, an index, the runtime...

// What did not match. why says how: "sha256" (its bytes are not those the index records: found is their
// SHA-256), "header" (a volume's header is damaged or not of this .p5z: its SHA-256 matched), "data" (a
// compressed file whose frames do not decompress) or "read" (its bytes could not be read).
typedef struct {
    int kind;               // MP_QV_VOLUME or MP_QV_FILE
    uint32_t volume;        // MP_QV_VOLUME: the NNN of ampr_zstd-NNN.pak
    char path[1024];        // the record's path in the .p5z (for a volume, ampr_zstd-NNN.pak)
    uint8_t expected[32];   // the SHA-256 the index records
    uint8_t found[32];      // the SHA-256 of what is there (when has_found)
    int has_found;          // 0 for "data" and "read": nothing whole to hash
    char why[16];
} mp_qv_bad;

#define MP_QV_MAX_LISTED 64u

typedef struct {
    int ok;              // 1: everything matched (nbad == 0)
    uint64_t bytes;      // bytes of the records' regions read
    uint64_t total;      // what they add up to (bytes == total when it ran to its end)
    size_t nbad;         // how many records did not match
    size_t nlisted;      // how many of them are in bad (the first MP_QV_MAX_LISTED, in the order read)
    mp_qv_bad *bad;      // malloc'd; mp_qv_report_free frees it
    uint32_t version;    // the index version: 1 as packed, 2 grown
    uint32_t volumes, files;   // the records checked: volumes, and files stored in the .p5z
} mp_qv_report;

#define MP_QV_CHECKED 0     // it ran to its end: report->ok says whether everything matched
#define MP_QV_ERROR (-1)    // it could not check: not a .p5z, its header, footer or index damaged, the
                            // file cut short, an I/O error or out of memory (e says which)
#define MP_QV_CANCELLED (-2)

// progress and cancel may be NULL; report is filled in every case (zeroed first) and must be freed with
// mp_qv_report_free. e may be NULL.
int mp_p5z_verify_quick(const char *path, const mp_qv_progress *progress, const mp_qv_cancel *cancel,
                        mp_qv_report *report, mp_err *e);
void mp_qv_report_free(mp_qv_report *report);

#endif
