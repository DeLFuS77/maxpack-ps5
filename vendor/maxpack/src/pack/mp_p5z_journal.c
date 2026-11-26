// SPDX-License-Identifier: GPL-3.0-or-later
// The journal of an append to a .p5z (<name>.p5z.maxpack-append): the size the .p5z had and its footer
// then, written before the first byte is appended. An append that stops leaves the .p5z longer and
// without a footer at its end; with the journal, any later open that may write takes it back to that
// size, exactly as it was, and a reader meanwhile reads it as it was. An append that had ended (its new
// index and footer written) before its journal went is kept instead: its index says the size the .p5z
// had before it (prev_size), which is the journal's.
//
// Journal, 64 bytes: "P5ZJRNL1", u32 version 1, u32 size 64, u64 the size of the .p5z, [32] its last 32
// bytes then (its footer), u32 0, u32 CRC-32 of bytes 0..60. It is written as <journal>.tmp, flushed to
// disk and renamed (the rename flushed too), so it is whole or not there; a whole .tmp whose rename did not
// reach the disk is the journal as well. A journal that cannot be read is never taken for damaged: it and
// its .p5z are left as they are.
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "mp_hash.h"
#include "mp_p5z_internal.h"

#define JOURNAL_LEN 64u
#define INDEX_MAX (1ull << 31)

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
    mp_remove_file(tmp);   // a .tmp left by an append that stopped before it began (recovered already)
    if (mp_wfile_create(tmp, &f, e) != 0) goto done;
    if (mp_wfile_write(f, j, sizeof j, e) != 0 || mp_wfile_sync_full(f, e) != 0) {
        mp_wfile_close(f, NULL);
        mp_remove_file(tmp);
        goto done;
    }
    // the new name must be on the disk before the first byte is appended
    if (mp_wfile_close(f, e) != 0 || mp_rename_durable(tmp, fin, e) != 0) {
        mp_remove_file(tmp);
        goto done;
    }
    rc = 0;
done:
    free(tmp);
    free(fin);
    return rc;
}

// One journal file: 0 whole (what it says in *old_size and footer), 1 read and damaged, 2 there but it
// could not be read, -1 not there.
static int read_one(const char *path, uint64_t *old_size, uint8_t footer[32]) {
    if (!path || mp_path_kind(path) != 1) return -1;
    mp_file *f = NULL;
    uint8_t j[JOURNAL_LEN];
    uint64_t size = 0;
    size_t got = 0;
    int rc = 2;
    if (mp_file_open_read(path, &f, NULL) != 0 || mp_file_size(f, &size, NULL) != 0) goto done;
    rc = 1;
    if (size != JOURNAL_LEN) goto done;   // a write cut short
    if (mp_file_pread(f, j, sizeof j, 0, &got, NULL) != 0) {
        rc = 2;
        goto done;
    }
    if (got != sizeof j || memcmp(j, "P5ZJRNL1", 8) != 0 || rd32(j + 8) != 1 || rd32(j + 12) != JOURNAL_LEN ||
        rd32(j + 56) != 0 || mp_crc32(j, 60, 0) != rd32(j + 60) || rd64(j + 16) < P5Z_HEADER + P5Z_FOOTER)
        goto done;
    *old_size = rd64(j + 16);
    memcpy(footer, j + 24, 32);
    rc = 0;
done:
    mp_file_close(f);
    return rc;
}

int p5z_journal_read(const char *p5z, uint64_t *old_size, uint8_t footer[32]) {
    char *fin = journal_path(p5z, 0), *tmp = journal_path(p5z, 1);
    int a = read_one(fin, old_size, footer), rc = a;
    if (a != 0 && a != 2) {   // no whole journal under its name: one whose rename was lost is as good
        int b = read_one(tmp, old_size, footer);
        if (b == 0 || b == 2)
            rc = b;
        else if (a == -1)
            rc = b == 1 ? 3 : -1;   // only a .tmp cut short: it was never renamed, so no append began
    }
    free(fin);
    free(tmp);
    return rc;
}

int p5z_journal_remove(const char *p5z) {
    char *tmp = journal_path(p5z, 1), *fin = journal_path(p5z, 0);
    int rc = 0;
    if (fin) mp_remove_file(fin);
    if (tmp) mp_remove_file(tmp);
    if (!fin || !tmp || mp_path_kind(fin) != 0 || mp_path_kind(tmp) != 0) rc = -1;
    free(tmp);
    free(fin);
    return rc;
}

// 1 if the .p5z, size bytes long, has the footer foot ending at old_size, 0 if not, -1 if it could not be read.
static int footer_at(const char *p5z, uint64_t size, uint64_t old_size, const uint8_t foot[32]) {
    if (old_size > size) return 0;
    mp_file *f = NULL;
    uint8_t at[32];
    size_t got = 0;
    int rc = -1;
    if (mp_file_open_read(p5z, &f, NULL) == 0 && mp_file_pread(f, at, 32, old_size - 32, &got, NULL) == 0 && got == 32)
        rc = memcmp(at, foot, 32) == 0;
    mp_file_close(f);
    return rc;
}

int p5z_journal_matches(const char *p5z, uint64_t size, uint64_t *old_size) {
    uint8_t foot[32];
    return p5z_journal_read(p5z, old_size, foot) == 0 && footer_at(p5z, size, *old_size, foot) == 1;
}

int p5z_append_ended(const char *p5z, uint64_t size, uint64_t old_size) {
    if (size < old_size + P5Z_FOOTER + 1) return 0;
    mp_file *f = NULL;
    uint8_t ft[P5Z_FOOTER], *cz = NULL, head[P5Z_INDEX_HEADER_V2];
    size_t got = 0;
    int ok = -1;   // until what it has to read is read
    ZSTD_DCtx *dc = NULL;
    if (mp_file_open_read(p5z, &f, NULL) != 0 || mp_file_pread(f, ft, sizeof ft, size - P5Z_FOOTER, &got, NULL) != 0 ||
        got != sizeof ft)
        goto done;
    // a whole footer at its end, whose index lies past the old end
    ok = 0;
    if (memcmp(ft, "P5ZFOOT1", 8) != 0 || mp_crc32(ft, 28, 0) != rd32(ft + 28)) goto done;
    uint64_t ioff = rd64(ft + 8), ilen = rd64(ft + 16);
    if (ioff < old_size || ilen == 0 || ilen > INDEX_MAX || ioff + ilen + P5Z_FOOTER != size) goto done;
    if (!(cz = malloc((size_t)ilen))) {
        ok = -1;
        goto done;
    }
    if (mp_file_pread(f, cz, (size_t)ilen, ioff, &got, NULL) != 0 || got != ilen) {
        ok = -1;
        goto done;
    }
    if (mp_crc32(cz, (size_t)ilen, 0) != rd32(ft + 24)) goto done;
    // and that index, of version 2, was written by the append that began at the old end
    if (!(dc = ZSTD_createDCtx())) {
        ok = -1;
        goto done;
    }
    ZSTD_inBuffer in = {cz, (size_t)ilen, 0};
    ZSTD_outBuffer out = {head, sizeof head, 0};
    while (out.pos < out.size) {
        size_t r = ZSTD_decompressStream(dc, &out, &in);
        if (ZSTD_isError(r) || (r == 0 && out.pos < out.size) || (in.pos == in.size && out.pos < out.size)) break;
    }
    ok = out.pos == sizeof head && memcmp(head, "P5ZIDX01", 8) == 0 && rd32(head + 8) == 2 && rd64(head + 56) == old_size;
done:
    ZSTD_freeDCtx(dc);
    free(cz);
    mp_file_close(f);
    return ok;
}

int p5z_restore_locked(mp_wfile *f, const char *p5z, int *what, mp_err *e) {
    uint64_t old = 0;
    uint8_t foot[32];
    *what = MP_RECOVER_NONE;
    int jr = p5z_journal_read(p5z, &old, foot);
    if (jr < 0) return 0;   // none
    if (jr == 3) {          // a journal cut short while it was written: nothing to undo
        if (p5z_journal_remove(p5z) != 0) {
            mp_err_set(e, "a journal cut short of %s could not be deleted", p5z);
            return -1;
        }
        return 0;
    }
    if (jr == 2) {
        // it is there and could not be read (another program has it, a drive waking up): it may be the only
        // record of the size the .p5z had, so it and the .p5z are left as they are
        mp_err_set(e, "the journal of an append to %s could not be read (it and the .p5z are left as they are): "
                      "try again",
                   p5z);
        return -1;
    }
    uint64_t size = mp_wfile_size(f);
    int at = jr == 1 ? 0 : footer_at(p5z, size, old, foot);
    if (at < 0) {
        mp_err_set(e, "%s could not be read to see what its journal undoes (both are left as they are): try again",
                   p5z);
        return -1;
    }
    if (!at) {
        // damaged, or not of this file as it is (another one in its place): it cannot say what to undo
        if (p5z_journal_remove(p5z) != 0) {
            mp_err_set(e, "a journal that is not of %s could not be deleted", p5z);
            return -1;
        }
        *what = MP_RECOVER_STALE;
        return 0;
    }
    if (size == old) {   // nothing was appended (it stopped before the first byte)
        if (p5z_journal_remove(p5z) != 0) {
            mp_err_set(e, "the journal of %s could not be deleted", p5z);
            return -1;
        }
        return 0;
    }
    int ended = p5z_append_ended(p5z, size, old);
    if (ended < 0) {
        mp_err_set(e, "the end of %s could not be read to see whether its last append had ended (it and its "
                      "journal are left as they are): try again",
                   p5z);
        return -1;
    }
    if (ended) {   // the append had ended: what grew is kept
        if (p5z_journal_remove(p5z) != 0) {
            mp_err_set(e, "the journal of %s could not be deleted (the .p5z is whole as it grew)", p5z);
            return -1;
        }
        *what = MP_RECOVER_COMMITTED;
        return 0;
    }
    if (mp_wfile_truncate(f, old, e) != 0 || mp_wfile_sync_full(f, e) != 0) return -1;   // the journal stays
    *what = MP_RECOVER_RESTORED;
    if (p5z_journal_remove(p5z) != 0) {
        mp_err_set(e, "%s is as it was before an append that stopped, but its journal could not be deleted", p5z);
        return -1;
    }
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
