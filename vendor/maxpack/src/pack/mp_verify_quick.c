// SPDX-License-Identifier: GPL-3.0-or-later
// The quick verify of a .p5z (see mp_verify_quick.h): every record's region read once, in the order they lie
// in the file, and hashed. Uses no global state (not mp_cancel_check, which belongs to the command line).
#include "mp_verify_quick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "mp_hash.h"
#include "mp_p5z_internal.h"

#define PIECE (4u << 20)

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

typedef struct {
    mp_p5z *p;
    const mp_qv_progress *prog;
    const mp_qv_cancel *cancel;
    mp_qv_report *r;
    uint8_t *raw;            // PIECE, for the regions stored as they are
    uint8_t *in, *dec;       // a compressed frame and what it gives, for the files stored in frames
    ZSTD_DCtx *dc;
    uint32_t nvol;           // volume records in the index
} qv;

static int cancelled(const qv *q) { return q->cancel && q->cancel->fn && q->cancel->fn(q->cancel->ctx); }

static void advance(qv *q, uint64_t n) {
    q->r->bytes += n;
    if (q->prog && q->prog->fn) q->prog->fn(q->prog->ctx, q->r->bytes, q->r->total);
}

static int read_at(qv *q, void *buf, size_t n, uint64_t off) {
    size_t got = 0;
    return mp_file_pread(q->p->f, buf, n, off, &got, NULL) == 0 && got == n ? 0 : -1;
}

// A volume's 64-byte header: of this .p5z (its archive_id), of this volume and size, with its CRC right. A
// header written before the .p5z grew counts the volumes it had then (ampr-emu-ng's p5z rules).
static int header_ok(const qv *q, const uint8_t h[64], uint32_t id, uint64_t size) {
    uint32_t count = rd32(h + 20);
    int count_ok = q->p->version == 2 ? count <= q->nvol && id < count : count == q->nvol;
    int ok = memcmp(h, "AMPRZDT1", 8) == 0 && rd32(h + 8) == 1 && rd32(h + 12) == 64 && rd32(h + 16) == id &&
             count_ok && memcmp(h + 24, q->p->man.archive_id, 16) == 0 && size >= MP_ZPK_VOL_HEADER &&
             rd64(h + 40) == size - MP_ZPK_VOL_HEADER;
    for (int i = 48; i < 60 && ok; i++) ok = h[i] == 0;
    uint8_t hz[64];
    memcpy(hz, h, 64);
    memset(hz + 60, 0, 4);
    return ok && mp_crc32(hz, 64, 0) == rd32(h + 60);
}

static void add_bad(qv *q, const p5z_file *f, int is_volume, uint32_t id, const uint8_t *found, const char *why) {
    mp_qv_report *r = q->r;
    r->nbad++;
    if (r->nlisted >= MP_QV_MAX_LISTED || !r->bad) return;
    mp_qv_bad *b = &r->bad[r->nlisted++];
    memset(b, 0, sizeof *b);
    b->kind = is_volume ? MP_QV_VOLUME : MP_QV_FILE;
    b->volume = is_volume ? id : 0;
    snprintf(b->path, sizeof b->path, "%s", f->path);
    memcpy(b->expected, f->sha, 32);
    if (found) {
        memcpy(b->found, found, 32);
        b->has_found = 1;
    }
    snprintf(b->why, sizeof b->why, "%s", why);
}

// One record: 0 when it was checked (a mismatch is added to the report), MP_QV_CANCELLED.
static int check_record(qv *q, const p5z_file *f) {
    int is_volume = f->role == MP_ROLE_VOLUME;
    unsigned id = 0;
    if (is_volume && sscanf(f->path, "ampr_zstd-%u.pak", &id) != 1) id = 0xFFFFFFFFu;
    mp_sha256 s;
    mp_sha256_init(&s);
    int header_bad = 0;
    if (f->storage == P5Z_STORE_RAW) {
        for (uint64_t off = 0; off < f->size; off += PIECE) {
            size_t k = f->size - off < PIECE ? (size_t)(f->size - off) : PIECE;
            if (cancelled(q)) return MP_QV_CANCELLED;
            if (read_at(q, q->raw, k, f->data_off + off) != 0) {
                advance(q, f->data_len - off);   // what is left of it counts as gone through
                add_bad(q, f, is_volume, id, NULL, "read");
                return 0;
            }
            if (is_volume && off == 0) {   // the header: outside the SHA-256, it has a CRC of its own
                header_bad = k < MP_ZPK_VOL_HEADER || !header_ok(q, q->raw, id, f->size);
                if (k > MP_ZPK_VOL_HEADER) mp_sha256_update(&s, q->raw + MP_ZPK_VOL_HEADER, k - MP_ZPK_VOL_HEADER);
            } else {
                mp_sha256_update(&s, q->raw, k);
            }
            advance(q, k);
        }
        if (is_volume && f->size == 0) header_bad = 1;
    } else {
        uint64_t left = f->data_len;
        for (uint32_t k = 0; k < f->nframes; k++) {
            const p5z_frame *fr = &q->p->frames[f->first_frame + k];
            if (cancelled(q)) return MP_QV_CANCELLED;
            const char *why = NULL;
            if (fr->csize > ZSTD_COMPRESSBOUND(MP_LONG_FRAME)) {
                why = "data";
            } else if (read_at(q, q->in, fr->csize, fr->off) != 0) {
                why = "read";
            } else {
                size_t got = ZSTD_decompressDCtx(q->dc, q->dec, fr->usize, q->in, fr->csize);
                if (ZSTD_isError(got) || got != fr->usize) why = "data";
                else mp_sha256_update(&s, q->dec, got);
            }
            if (why) {
                advance(q, left);
                add_bad(q, f, 0, 0, NULL, why);
                return 0;
            }
            left -= fr->csize;
            advance(q, fr->csize);
        }
    }
    uint8_t d[32];
    mp_sha256_final(&s, d);
    if (memcmp(d, f->sha, 32) != 0) add_bad(q, f, is_volume, id, d, "sha256");
    else if (header_bad) add_bad(q, f, is_volume, id, d, "header");
    return 0;
}

typedef struct {
    uint64_t off;
    size_t i;
} region;

static int by_offset(const void *a, const void *b) {
    const region *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

int mp_p5z_verify_quick(const char *path, const mp_qv_progress *progress, const mp_qv_cancel *cancel,
                        mp_qv_report *report, mp_err *e) {
    memset(report, 0, sizeof *report);
    qv q;
    memset(&q, 0, sizeof q);
    q.prog = progress;
    q.cancel = cancel;
    q.r = report;
    // header, footer and the index's CRC; a .p5z whose append stopped is read as it was before it
    if (mp_p5z_open(path, &q.p, e) != 0) return MP_QV_ERROR;
    mp_p5z *p = q.p;
    report->version = p->version;
    int frames = 0;
    for (size_t i = 0; i < p->nfiles; i++) {
        report->total += p->files[i].data_len;
        if (p->files[i].role == MP_ROLE_VOLUME) q.nvol++;
        frames |= p->files[i].storage == P5Z_STORE_FRAMES && p->files[i].nframes > 0;
    }
    report->volumes = q.nvol;
    report->files = (uint32_t)(p->nfiles - q.nvol);
    // in the order they lie in the file, so that a disk reads it from start to end
    region *order = malloc((p->nfiles ? p->nfiles : 1) * sizeof *order);
    report->bad = calloc(MP_QV_MAX_LISTED, sizeof *report->bad);
    q.raw = malloc(PIECE);
    if (frames) {
        q.in = malloc(ZSTD_COMPRESSBOUND(MP_LONG_FRAME));
        q.dec = malloc(MP_LONG_FRAME);
        q.dc = ZSTD_createDCtx();
    }
    int rc = MP_QV_CHECKED;
    if (!order || !report->bad || !q.raw || (frames && (!q.in || !q.dec || !q.dc))) {
        mp_err_set(e, "out of memory");
        rc = MP_QV_ERROR;
    }
    if (rc == MP_QV_CHECKED) {
        for (size_t i = 0; i < p->nfiles; i++) {
            order[i].off = p->files[i].data_off;
            order[i].i = i;
        }
        qsort(order, p->nfiles, sizeof *order, by_offset);
        for (size_t i = 0; i < p->nfiles && rc == MP_QV_CHECKED; i++) rc = check_record(&q, &p->files[order[i].i]);
        if (rc == MP_QV_CANCELLED) mp_err_set(e, "cancelled");
    }
    if (rc == MP_QV_CHECKED) {
        report->ok = report->nbad == 0;
        if (progress && progress->fn && p->nfiles == 0) progress->fn(progress->ctx, 0, 0);
    }
    ZSTD_freeDCtx(q.dc);
    free(q.in);
    free(q.dec);
    free(q.raw);
    free(order);
    mp_p5z_close(p);
    return rc;
}

void mp_qv_report_free(mp_qv_report *report) {
    if (!report) return;
    free(report->bad);
    report->bad = NULL;
    report->nlisted = 0;
}
