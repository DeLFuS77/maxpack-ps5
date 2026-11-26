// SPDX-License-Identifier: GPL-3.0-or-later
// The journal of an append to a .p5z (<name>.p5z.maxpack-append): the size the .p5z had and its footer
// then, written before the first byte is appended. An append that stops leaves the .p5z longer and
// without a footer at its end; with the journal, any later open that may write takes it back to that
// size, exactly as it was, and a reader meanwhile reads it as it was.
//
// Journal, 64 bytes: "P5ZJRNL1", u32 version 1, u32 size 64, u64 the size of the .p5z, [32] its last 32
// bytes then (its footer), u32 0, u32 CRC-32 of bytes 0..60. It is written as <journal>.tmp, flushed to
// disk and renamed, so it is either whole or not there.
#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_p5z_internal.h"

#define JOURNAL_LEN 64u

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static char *journal_path(const char *p5z, int tmp) {
    size_t a = strlen(p5z), b = strlen(MP_P5Z_JOURNAL);
    char *s = malloc(a + b + 5);
    if (!s) return NULL;
    memcpy(s, p5z, a);
    memcpy(s + a, MP_P5Z_JOURNAL, b);
    memcpy(s + a + b, tmp ? ".tmp" : "", tmp ? 5 : 1);
    return s;
}

int p5z_journal_write(const char *p5z, uint64_t old_size, const uint8_t footer[32], mp_err *e) {
    uint8_t j[JOURNAL_LEN] = {0};
    memcpy(j, "P5ZJRNL1", 8);
    wr32(j + 8, 1);
    wr32(j + 12, JOURNAL_LEN);
    wr64(j + 16, old_size);
    memcpy(j + 24, footer, 32);
    wr32(j + 60, mp_crc32(j, 60, 0));
    char *tmp = journal_path(p5z, 1), *fin = journal_path(p5z, 0);
    mp_wfile *f = NULL;
    int rc = -1;
    if (!tmp || !fin) {
        mp_err_set(e, "out of memory");
        goto done;
    }
    mp_remove_file(tmp);   // a .tmp left by an append that stopped before it began
    if (mp_wfile_create(tmp, &f, e) != 0) goto done;
    if (mp_wfile_write(f, j, sizeof j, e) != 0 || mp_wfile_sync(f, e) != 0) {
        mp_wfile_close(f, NULL);
        mp_remove_file(tmp);
        goto done;
    }
    if (mp_wfile_close(f, e) != 0 || mp_rename(tmp, fin, e) != 0) {
        mp_remove_file(tmp);
        goto done;
    }
    rc = 0;
done:
    free(tmp);
    free(fin);
    return rc;
}

int p5z_journal_read(const char *p5z, uint64_t *old_size, uint8_t footer[32]) {
    char *path = journal_path(p5z, 0);
    mp_file *f = NULL;
    uint8_t j[JOURNAL_LEN];
    uint64_t size = 0;
    size_t got = 0;
    int rc = -1;
    if (!path || mp_path_kind(path) != 1) goto done;
    rc = 1;
    if (mp_file_open_read(path, &f, NULL) != 0 || mp_file_size(f, &size, NULL) != 0 || size != JOURNAL_LEN ||
        mp_file_pread(f, j, sizeof j, 0, &got, NULL) != 0 || got != sizeof j)
        goto done;
    if (memcmp(j, "P5ZJRNL1", 8) != 0 || rd32(j + 8) != 1 || rd32(j + 12) != JOURNAL_LEN || rd32(j + 56) != 0 ||
        mp_crc32(j, 60, 0) != rd32(j + 60) || rd64(j + 16) < P5Z_HEADER + P5Z_FOOTER)
        goto done;
    *old_size = rd64(j + 16);
    memcpy(footer, j + 24, 32);
    rc = 0;
done:
    mp_file_close(f);
    free(path);
    return rc;
}

void p5z_journal_remove(const char *p5z) {
    char *tmp = journal_path(p5z, 1), *fin = journal_path(p5z, 0);
    if (fin) mp_remove_file(fin);
    if (tmp) mp_remove_file(tmp);
    free(tmp);
    free(fin);
}

int p5z_journal_matches(const char *p5z, uint64_t size, uint64_t *old_size) {
    uint8_t foot[32], at[32];
    if (p5z_journal_read(p5z, old_size, foot) != 0 || *old_size > size) return 0;
    mp_file *f = NULL;
    size_t got = 0;
    int same = mp_file_open_read(p5z, &f, NULL) == 0 && mp_file_pread(f, at, 32, *old_size - 32, &got, NULL) == 0 &&
               got == 32 && memcmp(at, foot, 32) == 0;
    mp_file_close(f);
    return same;
}

int p5z_restore_locked(mp_wfile *f, const char *p5z, int *what, mp_err *e) {
    uint64_t old = 0;
    uint8_t foot[32];
    *what = MP_RECOVER_NONE;
    int jr = p5z_journal_read(p5z, &old, foot);
    if (jr < 0) {   // none: at most a .tmp of one that never began
        char *tmp = journal_path(p5z, 1);
        if (tmp && mp_path_kind(tmp) == 1) mp_remove_file(tmp);
        free(tmp);
        return 0;
    }
    if (jr > 0 || !p5z_journal_matches(p5z, mp_wfile_size(f), &old)) {
        // damaged, or not of this file as it is (another one in its place): it cannot say what to undo
        p5z_journal_remove(p5z);
        *what = MP_RECOVER_STALE;
        return 0;
    }
    if (mp_wfile_truncate(f, old, e) != 0 || mp_wfile_sync(f, e) != 0) return -1;   // the journal stays
    p5z_journal_remove(p5z);
    *what = MP_RECOVER_RESTORED;
    return 0;
}

int mp_p5z_recover(const char *p5z, int *what, mp_err *e) {
    *what = MP_RECOVER_NONE;
    char *fin = journal_path(p5z, 0), *tmp = journal_path(p5z, 1);
    int has = (fin && mp_path_kind(fin) == 1) || (tmp && mp_path_kind(tmp) == 1);
    free(fin);
    free(tmp);
    if (!has) return 0;   // what nearly every open finds: one stat, two at most
    int kind = mp_path_kind(p5z);
    if (kind == 2) return 0;   // a folder: a file named so next to it is not a journal of ours
    if (kind == 0) {           // the .p5z is gone: nothing to undo
        p5z_journal_remove(p5z);
        *what = MP_RECOVER_STALE;
        return 0;
    }
    mp_wfile *f = NULL;
    int busy = 0;
    if (mp_wfile_reopen_locked(p5z, &f, &busy, e) != 0) {
        if (!busy) return -1;
        if (e) e->msg[0] = 0;
        *what = MP_RECOVER_BUSY;   // another MaxPack is appending to it: its journal is its own
        return 0;
    }
    int rc = p5z_restore_locked(f, p5z, what, e);
    if (mp_wfile_close(f, rc == 0 ? e : NULL) != 0) rc = -1;
    return rc;
}
