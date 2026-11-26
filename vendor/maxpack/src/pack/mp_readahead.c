// SPDX-License-Identifier: GPL-3.0-or-later
// Pieces are numbered in the order they are handed out (seq) and started strictly in that order, so
// a piece that waits for memory never holds up an earlier one. Their bytes live in one ring of
// `budget` bytes taken and given back in that same order. A piece is hashed once the previous piece of
// its item is: the thread that finishes the previous one goes on with it if it is already read,
// otherwise the thread that reads it does. Everything is protected by one mutex; reading and hashing
// happen outside it.
#include "mp_readahead.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "mp_hash.h"
#include "mp_parallel.h"
#include "mp_platform.h"

#define RING 1024u   // pieces in flight at most (many small files take many pieces for few bytes)

enum { P_READING = 1, P_READ, P_HASHING, P_READY, P_FAILED };

typedef struct {
    uint8_t *buf;
    size_t len, charge;   // charge: len plus the end of the ring skipped to fit it
    size_t item;
    uint64_t off;         // in the item's file
    int state, first, last;
} piece;

struct mp_readahead {
    mp_source *src;
    mp_readahead_range *items;
    size_t n;
    uint8_t (*sha)[32];
    mp_sha256 *ctx;   // per item: its SHA-256 so far
    size_t piece_len;
    // The next piece to start: its item (skipping the empty ones) and how far into it.
    size_t cur;
    uint64_t cur_done;
    uint64_t next, tail;   // sequence numbers: the next to start, the oldest not given back
    piece ring[RING];
    uint8_t *mem;
    size_t cap, head, used;
    mp_mutex *mu;
    mp_cond *work_cv;    // the readers wait: room to start a piece, or stop
    mp_cond *ready_cv;   // the consumer waits: the oldest piece is ready or failed, or cancelled
    mp_thread *t[MP_READAHEAD_MAX_THREADS];
    unsigned started;
    int stop, cancelled, failed, held;
    uint64_t fail_seq;   // the earliest piece that failed
    mp_err fail_err;
    int done_rc;         // once the consumer got -1, it keeps getting it
    mp_err done_err;
};

void mp_readahead_opts_default(mp_readahead_opts *o) {
    o->threads = 4;
    o->piece = 4u << 20;
    o->budget = 256u << 20;
}

static void skip_empty(mp_readahead *r) {
    while (r->cur < r->n && r->items[r->cur].len == 0) r->cur++;
}

// With the mutex held: whether the next piece can start now, and where its bytes go.
static int can_start(mp_readahead *r, size_t *len, size_t *pos, size_t *charge) {
    if (r->stop || r->cancelled || r->failed || r->cur >= r->n || r->next - r->tail >= RING) return 0;
    uint64_t left = r->items[r->cur].len - r->cur_done;
    *len = left < r->piece_len ? (size_t)left : r->piece_len;
    if (r->used == 0) r->head = 0;
    if (r->head + *len <= r->cap) {
        *pos = r->head;
        *charge = *len;
    } else {
        *pos = 0;
        *charge = r->cap - r->head + *len;
    }
    return r->used + *charge <= r->cap;
}

// With the mutex held: hashes piece seq if it is its turn, and then the next ones of its item that
// are already read.
static void hash_from(mp_readahead *r, uint64_t seq) {
    for (;;) {
        piece *p = &r->ring[seq % RING];
        if (r->stop || p->state != P_READ) return;
        if (!p->first && seq - 1 >= r->tail && r->ring[(seq - 1) % RING].state != P_READY) return;
        p->state = P_HASHING;
        mp_mutex_unlock(r->mu);
        mp_sha256 *c = &r->ctx[p->item];
        if (p->first) mp_sha256_init(c);
        mp_sha256_update(c, p->buf, p->len);
        if (p->last) mp_sha256_final(c, r->sha[p->item]);
        mp_mutex_lock(r->mu);
        p->state = P_READY;
        if (seq == r->tail) mp_cond_signal(r->ready_cv);
        if (p->last || ++seq >= r->next) return;
    }
}

static void reader(void *arg) {
    mp_readahead *r = arg;
    mp_mutex_lock(r->mu);
    for (;;) {
        if (r->stop) break;
        if (!r->cancelled && !r->failed && r->cur < r->n && mp_cancelled()) {
            r->cancelled = 1;   // nothing more starts; the consumer learns it when it gets here
            mp_cond_broadcast(r->ready_cv);
        }
        size_t len = 0, pos = 0, charge = 0;
        if (!can_start(r, &len, &pos, &charge)) {
            mp_cond_wait(r->work_cv, r->mu);
            continue;
        }
        uint64_t seq = r->next++;
        piece *p = &r->ring[seq % RING];
        const mp_readahead_range *it = &r->items[r->cur];
        p->buf = r->mem + pos;
        p->len = len;
        p->charge = charge;
        p->item = r->cur;
        p->off = it->off + r->cur_done;
        p->first = r->cur_done == 0;
        r->cur_done += len;
        p->last = r->cur_done == it->len;
        if (p->last) {
            r->cur++;
            r->cur_done = 0;
            skip_empty(r);
        }
        p->state = P_READING;
        r->head = pos + len;
        r->used += charge;
        uint32_t file = it->file;
        mp_mutex_unlock(r->mu);
        mp_err e = {{0}};
        int rc = mp_source_read(r->src, file, p->off, p->buf, p->len, &e);
        mp_mutex_lock(r->mu);
        if (rc != 0) {
            p->state = P_FAILED;
            if (!r->failed || seq < r->fail_seq) {
                r->fail_seq = seq;
                r->fail_err = e;
            }
            r->failed = 1;
            mp_cond_broadcast(r->ready_cv);
            continue;
        }
        p->state = P_READ;
        if (r->sha) {
            hash_from(r, seq);
        } else {
            p->state = P_READY;
            if (seq == r->tail) mp_cond_signal(r->ready_cv);
        }
    }
    mp_mutex_unlock(r->mu);
}

int mp_readahead_open_ranges(mp_source *src, const mp_readahead_range *ranges, size_t n, uint8_t (*sha)[32],
                             const mp_readahead_opts *o, mp_readahead **out, mp_err *e) {
    *out = NULL;
    size_t nfiles = mp_source_count(src);
    uint64_t total = 0, pieces = 0, plen = o->piece ? o->piece : 1;
    for (size_t k = 0; k < n; k++) {
        const mp_entry *en = ranges[k].file < nfiles ? mp_source_entry(src, ranges[k].file) : NULL;
        if (!en || ranges[k].off > en->size || ranges[k].len > en->size - ranges[k].off) {
            mp_err_set(e, "read outside %s (%" PRIu64 " + %" PRIu64 ")", en ? en->path : "the source",
                       ranges[k].off, ranges[k].len);
            return -1;
        }
        total += ranges[k].len;
        pieces += (ranges[k].len + plen - 1) / plen;
    }
    mp_readahead *r = calloc(1, sizeof *r);
    if (!r) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    r->src = src;
    r->n = n;
    r->sha = sha;
    r->piece_len = (size_t)plen;
    // A ring larger than everything to read would only waste memory (small sources, tests).
    uint64_t cap = o->budget > plen ? o->budget : plen;
    if (total + plen < cap) cap = total + plen;
    r->cap = (size_t)cap;
    r->items = malloc((n ? n : 1) * sizeof *r->items);
    r->ctx = sha ? malloc((n ? n : 1) * sizeof *r->ctx) : NULL;
    r->mem = malloc(r->cap);
    r->mu = mp_mutex_new();
    r->work_cv = mp_cond_new();
    r->ready_cv = mp_cond_new();
    if (!r->items || (sha && !r->ctx) || !r->mem || !r->mu || !r->work_cv || !r->ready_cv) {
        mp_readahead_close(r);
        mp_err_set(e, "out of memory");
        return -1;
    }
    if (n) memcpy(r->items, ranges, n * sizeof *ranges);
    for (size_t k = 0; sha && k < n; k++)
        if (ranges[k].len == 0) {
            mp_sha256 s;
            mp_sha256_init(&s);
            mp_sha256_final(&s, sha[k]);
        }
    skip_empty(r);
    unsigned want = o->threads < 1 ? 1 : o->threads;
    if (want > MP_READAHEAD_MAX_THREADS) want = MP_READAHEAD_MAX_THREADS;
    if (pieces < want) want = (unsigned)pieces;
    for (unsigned i = 0; i < want; i++)
        if (mp_thread_start(&r->t[r->started], reader, r) == 0) r->started++;
    if (want && !r->started) {
        mp_readahead_close(r);
        mp_err_set(e, "could not start the reading threads");
        return -1;
    }
    *out = r;
    return 0;
}

int mp_readahead_open(mp_source *src, const uint32_t *files, size_t n, uint8_t (*sha)[32],
                      const mp_readahead_opts *o, mp_readahead **out, mp_err *e) {
    *out = NULL;
    mp_readahead_range *rg = malloc((n ? n : 1) * sizeof *rg);
    if (!rg) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    size_t nfiles = mp_source_count(src);
    for (size_t k = 0; k < n; k++) {
        rg[k].file = files[k];
        rg[k].off = 0;
        rg[k].len = files[k] < nfiles ? mp_source_entry(src, files[k])->size : 1;   // 1: refused as outside
    }
    int rc = mp_readahead_open_ranges(src, rg, n, sha, o, out, e);
    free(rg);
    return rc;
}

int mp_readahead_next(mp_readahead *r, mp_readahead_piece *out, mp_err *e) {
    mp_mutex_lock(r->mu);
    if (r->held) {   // the previous piece: its memory goes back
        r->used -= r->ring[r->tail % RING].charge;
        r->tail++;
        r->held = 0;
        mp_cond_broadcast(r->work_cv);
    }
    int rc;
    for (;;) {
        if (r->done_rc) {
            if (e) *e = r->done_err;
            rc = -1;
            break;
        }
        if (r->tail < r->next) {
            piece *p = &r->ring[r->tail % RING];
            if (p->state == P_READY) {
                out->data = p->buf;
                out->len = p->len;
                out->item = p->item;
                out->off = p->off;
                out->last = p->last;
                r->held = 1;
                rc = 1;
                break;
            }
            if (p->state == P_FAILED) {   // the earliest failure: every piece before it was handed out
                r->done_rc = -1;
                r->done_err = r->fail_err;
                continue;
            }
        } else if (r->cur >= r->n) {
            rc = 0;
            break;
        } else if (r->cancelled) {
            r->done_rc = -1;
            mp_err_set(&r->done_err, "cancelled");
            continue;
        }
        mp_cond_wait(r->ready_cv, r->mu);
    }
    mp_mutex_unlock(r->mu);
    return rc;
}

void mp_readahead_close(mp_readahead *r) {
    if (!r) return;
    if (r->mu && r->work_cv) {
        mp_mutex_lock(r->mu);
        r->stop = 1;
        mp_cond_broadcast(r->work_cv);
        mp_mutex_unlock(r->mu);
    }
    for (unsigned i = 0; i < r->started; i++) mp_thread_join(r->t[i]);
    mp_cond_free(r->work_cv);
    mp_cond_free(r->ready_cv);
    mp_mutex_free(r->mu);
    free(r->mem);
    free(r->ctx);
    free(r->items);
    free(r);
}
