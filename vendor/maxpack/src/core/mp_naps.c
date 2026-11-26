// SPDX-License-Identifier: GPL-3.0-or-later
// naps_pkg_layout.dat, the map of a PS5 package's inner image, and the mount it gives (see mp_fpkg.h).
// Ported from pearlxcore/ProsperoPkgTool (MIT): Containers/ProsperoNapsLayout.cs,
// ProsperoPs5InnerImageReader.cs, NapsDialectRegistry.cs, NapsIndexedLogicalMap.cs,
// NapsSparseFidxDialect.cs and ProsperoInnerMountStream.cs; fthdgn-gh/Alembic's src/core/
// fpkg_inner.cpp (GPL-3.0-or-later) as the second reading.
//
// The map: a 16-byte header of counts, the outer blocks' digests, shuffle patterns, "fidx" (the
// mount offset where each file starts, 6 bytes each, the last one the mount's end with 0x40), "u2c"
// (for each group of 8 ublocks of 256 KiB, the first record that starts in each) and the CblockInfo
// records, 9 bytes each: STD records (a block: where it starts in pfs_image.dat and in the mount, both
// modulo 256 KiB, its first sub-chunk's compressed size and its codec) and RUN records (re-base the
// compressed cursor on a 256 KiB page). The writers lay it out differently ("dialects"); as the
// reference, every reading is tried, and when two disagree the one whose mount holds a readable PFS
// wins.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_fpkg.h"
#include "mp_kraken.h"
#include "mp_platform.h"

#define CHUNK128K 0x20000u
#define PAGE 0x40000u   // a 256 KiB ublock
#define MOD 0x3FFFFu
#define SPARSE_MARKER 0x40
#define MAX_CBLOCKS (1u << 23)   // 8 Mi records: a mount of more than 1 TiB in 128 KiB pieces
#define MAX_MOUNT (2ull << 40)

typedef struct {
    uint8_t raw[9];
    uint8_t run;
    uint32_t coff;   // STD: its start modulo 256 KiB; RUN: the end of what came before it
    uint32_t uoff, clen_even_minus1;
    uint8_t even, odd, kde, shuffle;
    uint8_t key_table;
    uint32_t coff_256k;   // RUN: the 256 KiB page the cursor re-bases on
} cbi;

typedef struct {
    int num_files, compression_type, num_ublocks, num_outer, num_cblock, num_shuffle;
    uint8_t *fidx_type;
    uint64_t *fidx;
    uint32_t *u2c_base;
    uint8_t *u2c_delta;   // 7 per group
    int num_u2c;
    cbi *cb;
} layout;

static uint64_t le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static int bad(mp_err *e, const char *why) {
    mp_err_set(e, "%s", why);
    return -1;
}

static void layout_free(layout *l) {
    free(l->fidx_type);
    free(l->fidx);
    free(l->u2c_base);
    free(l->u2c_delta);
    free(l->cb);
}

static int parse(const uint8_t *b, size_t len, layout *l, mp_err *e) {
    memset(l, 0, sizeof *l);
    if (len < 16) return bad(e, "naps_pkg_layout.dat is too small");
    uint64_t w0 = le64(b), w1 = le64(b + 8);
    l->num_files = (int)(w0 & 0xFFFFFF) + 1;
    l->compression_type = (int)((w0 >> 24) & 0x3);
    l->num_shuffle = (int)((w0 >> 28) & 0xF);
    l->num_ublocks = (int)((w0 >> 32) & 0xFFFFFF);
    l->num_outer = (int)(w1 & 0xFFFFFF);
    l->num_cblock = (int)((w1 >> 24) & 0xFFFFFF) + 2;
    l->num_u2c = (l->num_ublocks + 8) >> 3;
    uint64_t fidx_off = 16 + (uint64_t)l->num_outer * 8 + (uint64_t)l->num_shuffle * 8;
    uint64_t u2c_off = fidx_off + (uint64_t)l->num_files * 6, u2c_end = u2c_off + (uint64_t)l->num_u2c * 10;
    uint64_t cbi_off = ((uint64_t)(l->num_shuffle + l->num_outer) * 8 + 10ull * (uint64_t)l->num_u2c +
                        6ull * (uint64_t)(l->num_files - 1) + 29) & ~7ull;
    uint64_t cbi_end = cbi_off + (uint64_t)l->num_cblock * 9;
    if (u2c_end > len || cbi_end > len || cbi_off < u2c_end || (unsigned)l->num_cblock > MAX_CBLOCKS)
        return bad(e, "naps_pkg_layout.dat is shorter than its header says");
    l->fidx_type = malloc((size_t)l->num_files);
    l->fidx = malloc((size_t)l->num_files * sizeof *l->fidx);
    l->u2c_base = malloc((size_t)(l->num_u2c ? l->num_u2c : 1) * sizeof *l->u2c_base);
    l->u2c_delta = malloc((size_t)(l->num_u2c ? l->num_u2c : 1) * 7);
    l->cb = calloc((size_t)l->num_cblock, sizeof *l->cb);
    if (!l->fidx_type || !l->fidx || !l->u2c_base || !l->u2c_delta || !l->cb) {
        layout_free(l);
        return bad(e, "out of memory");
    }
    for (int i = 0; i < l->num_files; i++) {
        const uint8_t *p = b + fidx_off + (size_t)i * 6;
        uint64_t v = 0;
        for (int k = 4; k >= 0; k--) v = v << 8 | p[k];
        l->fidx[i] = v;
        l->fidx_type[i] = p[5];
    }
    for (int i = 0; i < l->num_u2c; i++) {
        const uint8_t *p = b + u2c_off + (size_t)i * 10;
        l->u2c_base[i] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
        memcpy(l->u2c_delta + (size_t)i * 7, p + 3, 7);
    }
    for (int i = 0; i < l->num_cblock; i++) {
        const uint8_t *p = b + cbi_off + (size_t)i * 9;
        cbi *c = &l->cb[i];
        memcpy(c->raw, p, 9);
        uint64_t lo = le64(p), hi = p[8];
        c->run = ((lo >> 18) & 1) != 0;
        c->coff = (uint32_t)(lo & MOD);
        if (!c->run) {
            c->uoff = (uint32_t)((lo >> 20) & MOD);
            c->clen_even_minus1 = (uint32_t)((lo >> 38) & 0x1FFFF);
            c->even = (uint8_t)((lo >> 55) & 7);
            c->odd = (uint8_t)((lo >> 58) & 7);
            c->kde = (uint8_t)(((hi & 7) << 3) | ((lo >> 61) & 7));
            c->shuffle = (uint8_t)((hi >> 4) & 0xF);
        } else {
            c->key_table = (uint8_t)((lo >> 48) & 3);
            c->coff_256k = (uint32_t)(((hi & 0xFF) << 14) | ((lo >> 50) & 0x3FFF));
        }
    }
    return 0;
}

// The exact mount end the last fidx entry gives (with the 0x40 marker), or 0.
static int mount_end(const layout *l, uint64_t *end) {
    if (l->num_files < 1 || l->fidx_type[l->num_files - 1] != SPARSE_MARKER) return 0;
    *end = l->fidx[l->num_files - 1];
    return 1;
}

static int kraken_flags(uint8_t even, uint8_t odd, int multi) {
    int f = 0;
    if (even & 4) f |= MP_KRAKEN_NEWLZ0;
    if (even & 2) f |= MP_KRAKEN_SUB0;
    if (multi) {
        if (odd & 4) f |= MP_KRAKEN_NEWLZ1;
        if (odd & 2) f |= MP_KRAKEN_SUB1;
    }
    return f;
}

// ---- a cursor -> block map (open addressing) ---------------------------------------------------

typedef struct {
    uint64_t *key;
    int64_t *val;
    size_t cap;
} owners;

static int owners_init(owners *o, size_t n) {
    o->cap = 16;
    while (o->cap < 2 * n + 16) o->cap <<= 1;
    o->key = malloc(o->cap * sizeof *o->key);
    o->val = malloc(o->cap * sizeof *o->val);
    if (!o->key || !o->val) return -1;
    for (size_t i = 0; i < o->cap; i++) o->val[i] = -1;
    return 0;
}
static void owners_free(owners *o) {
    free(o->key);
    free(o->val);
}
static size_t owners_slot(const owners *o, uint64_t k) {
    size_t i = (size_t)((k * 0x9E3779B97F4A7C15ull) >> 20) & (o->cap - 1);
    while (o->val[i] >= 0 && o->key[i] != k) i = (i + 1) & (o->cap - 1);
    return i;
}
static void owners_set(owners *o, uint64_t k, int64_t v) {
    size_t i = owners_slot(o, k);
    o->key[i] = k;
    o->val[i] = v;
}
static int64_t owners_get(const owners *o, uint64_t k) { return o->val[owners_slot(o, k)]; }

// ---- the readings -----------------------------------------------------------------------------

typedef struct {
    const cbi *entry, *run;
    int run_index, entry_index;
    uint32_t end_coff;
} stdrec;

static stdrec *collect(const layout *l, size_t *n) {
    stdrec *v = malloc((size_t)l->num_cblock * sizeof *v);
    *n = 0;
    if (!v) return NULL;
    const cbi *active = NULL;
    int run_index = -1;
    for (int i = 0; i < l->num_cblock; i++) {
        const cbi *c = &l->cb[i];
        if (c->run) {
            active = c;
            run_index = i;
            continue;
        }
        // a span ends at the next record, or at a following RUN's end offset
        uint32_t end = i + 1 < l->num_cblock ? l->cb[i + 1].coff : c->coff;
        stdrec r = {c, active, run_index, i, end};
        v[(*n)++] = r;
    }
    return v;
}

static int is_zero_hole(const cbi *c, uint32_t total, uint32_t len) {
    return (c->even == 0 && c->odd == 0 && c->kde == 0) ||
           (c->even == 1 && c->kde == 0 && c->clen_even_minus1 == 7 &&
            ((c->odd == 1 && total == 16 && len > 16) || (c->odd == 0 && total == 8 && len > 8)));
}

typedef struct {
    mp_naps_block *b;
    size_t n;
} blist;

// compatible-coffset-v1 (native = 0) and native-span-v1 (native = 1).
static int walk(const layout *l, uint64_t image_len, int native, blist *out, mp_err *e) {
    out->b = NULL;
    out->n = 0;
    const cbi *cb = l->cb;
    int ncb = l->num_cblock;
    if (native) {
        int fail = ncb < 3 || !cb[0].run || cb[ncb - 1].run || cb[ncb - 1].even != 0 || cb[ncb - 1].odd != 0;
        for (int i = 0; !fail && i < ncb; i++)
            if (!cb[i].run && ((cb[i].raw[8] & 8) != 0 || cb[i].kde != 0)) fail = 1;
        for (int i = 0; !fail && i + 1 < ncb; i++)
            if (cb[i].run && cb[i + 1].run) fail = 1;
        if (fail) return bad(e, "native spans need unmarked KDE=0 records, a first RUN and a terminator");
    }
    size_t ns = 0;
    stdrec *s = collect(l, &ns);
    if (!s) return bad(e, "out of memory");
    if (ns < 2) {
        free(s);
        return bad(e, "no STD record and terminator");
    }
    int marked = 0, high = 0;
    for (size_t i = 0; i < ns; i++) {
        if (s[i].entry->raw[8] & 0x08) marked = 1;
        if (s[i].run && s[i].run->coff_256k > 0) high = 1;
    }
    int legacy = !native && l->compression_type != 0 && !marked && !high;
    mp_naps_block *r = calloc(ns - 1, sizeof *r);
    owners own;
    if (!r || owners_init(&own, ns) != 0) {
        free(r);
        free(s);
        owners_free(&own);
        return bad(e, "out of memory");
    }
    uint64_t abs = 0, cursor = 0;
    int last_run = -1;
    size_t n = 0;
    const char *why = NULL;
    for (size_t i = 0; i + 1 < ns && !why; i++) {
        const cbi *curr = s[i].entry, *next = s[i + 1].entry;
        if (legacy) {
            cursor = curr->coff;
        } else if (s[i].run && s[i].run_index != last_run) {
            cursor = ((uint64_t)s[i].run->coff_256k << 18) | curr->coff;
            last_run = s[i].run_index;
        }
        uint32_t len = ((next->uoff - curr->uoff - 1) & MOD) + 1;
        uint32_t total = legacy ? ((next->coff - curr->coff) & MOD) : ((s[i].end_coff - curr->coff) & MOD);
        uint32_t even = curr->clen_even_minus1 + 1;
        int zero = is_zero_hole(curr, total, len);
        int stored = (curr->raw[8] & 0x08) != 0 && !zero;
        if (native && !zero && curr->even == 1 && curr->odd == (len > CHUNK128K ? 1 : 0) &&
            even == (len < CHUNK128K ? len : CHUNK128K) && total == (len & MOD))
            stored = 1;
        if (stored) total = len;
        int dedup = legacy ? !zero && (total == 0 || (curr->kde == 0 && curr->even == 1 && curr->odd == 1 && total < len))
                           : !stored && !zero && total == 0;
        int kr = !stored && !dedup && !zero && total < len;
        if (native) {
            if ((cursor & MOD) != curr->coff) {
                why = "the source cursor disagrees with an STD record";
                break;
            }
            if (!s[i].run || (s[i].entry_index > 0 && cb[s[i].entry_index - 1].run &&
                              cb[s[i].entry_index - 1].key_table != 0)) {
                why = "an unsupported RUN record";
                break;
            }
            if (!dedup && (cursor > image_len || total > image_len - cursor)) {
                why = "a block outside the inner image";
                break;
            }
            if (!zero && !dedup && !stored && !kr) {
                why = "an invalid stored span";
                break;
            }
            if (kr && (total == 0 || even > total || (len > CHUNK128K && even == total))) {
                why = "an invalid compressed range";
                break;
            }
        }
        int64_t owner = -1;
        if (dedup) {
            owner = owners_get(&own, cursor);
            if (!legacy && (owner < 0 || r[owner].coff != cursor)) {
                why = "an alias of a compressed offset with no block";
                break;
            }
            if (native && r[owner].len < len) {
                why = "an alias longer than its block";
                break;
            }
        }
        if (curr->shuffle != 0 && !zero && !dedup) {
            why = "shuffled blocks (not supported)";
            break;
        }
        mp_naps_block *b = &r[n++];
        b->coff = cursor;
        b->clen = total;
        b->even_len = even;
        b->off = abs;
        b->len = len;
        b->kraken = (uint8_t)kr;
        b->zero = (uint8_t)zero;
        b->alias = (uint8_t)dedup;
        b->source = owner;
        b->kflags = kr ? kraken_flags(curr->even, curr->odd, len > CHUNK128K) : 0;
        if (!zero && !dedup) owners_set(&own, cursor, (int64_t)(n - 1));
        abs += len;
        if ((!zero || native) && !dedup) cursor += total;
    }
    if (!why && native &&
        (abs <= ((uint64_t)l->num_ublocks - 1) * PAGE || abs > (uint64_t)l->num_ublocks * PAGE || !l->num_ublocks))
        why = "the coverage disagrees with the ublock count";
    free(s);
    owners_free(&own);
    if (why) {
        free(r);
        return bad(e, why);
    }
    out->b = r;
    out->n = n;
    return 0;
}

// The sparse-fidx walk (Sony's writer): unmarked full stored spans by their wrapped delta; zero holes
// still take payload.
static int walk_unmarked(const layout *l, uint64_t image_len, blist *out, mp_err *e) {
    out->b = NULL;
    out->n = 0;
    size_t ns = 0;
    stdrec *s = collect(l, &ns);
    if (!s) return bad(e, "out of memory");
    if (ns < 2) {
        free(s);
        return bad(e, "no STD record and terminator");
    }
    mp_naps_block *r = calloc(ns - 1, sizeof *r);
    owners own;
    if (!r || owners_init(&own, ns) != 0) {
        free(r);
        free(s);
        owners_free(&own);
        return bad(e, "out of memory");
    }
    uint64_t abs = 0, cursor = 0;
    int last_run = -1;
    size_t n = 0;
    const char *why = NULL;
    for (size_t i = 0; i + 1 < ns && !why; i++) {
        const cbi *curr = s[i].entry, *next = s[i + 1].entry;
        if (s[i].run && s[i].run_index != last_run) {
            cursor = ((uint64_t)s[i].run->coff_256k << 18) | curr->coff;
            last_run = s[i].run_index;
        }
        uint32_t len = ((next->uoff - curr->uoff - 1) & MOD) + 1;
        uint32_t total = (s[i].end_coff - curr->coff) & MOD;
        uint32_t even = curr->clen_even_minus1 + 1;
        int zero = is_zero_hole(curr, total, len);
        int stored = (curr->raw[8] & 0x08) != 0 && !zero;
        if (!zero && curr->even == 1 && curr->odd == (len > CHUNK128K ? 1 : 0) &&
            even == (len < CHUNK128K ? len : CHUNK128K) && total == (len & MOD))
            stored = 1;
        if (stored) total = len;
        int dedup = !stored && !zero && total == 0;
        int kr = !stored && !dedup && !zero && total < len;
        if (!zero && !dedup && (cursor > image_len || total > image_len - cursor)) {
            why = "a block outside the inner image";
            break;
        }
        if (!zero && !dedup && !stored && !kr) {
            why = "an invalid span";
            break;
        }
        if (kr && (total == 0 || even > total || (len > CHUNK128K && even == total))) {
            why = "an invalid compressed range";
            break;
        }
        int64_t owner = -1;
        if (dedup) {
            owner = owners_get(&own, cursor);
            if (owner < 0 || r[owner].coff != cursor) {
                why = "an alias of a compressed offset with no block";
                break;
            }
        }
        if (curr->shuffle != 0 && !zero && !dedup) {
            why = "shuffled blocks (not supported)";
            break;
        }
        mp_naps_block *b = &r[n++];
        b->coff = cursor;
        b->clen = total;
        b->even_len = even;
        b->off = abs;
        b->len = len;
        b->kraken = (uint8_t)kr;
        b->zero = (uint8_t)zero;
        b->alias = (uint8_t)dedup;
        b->source = owner;
        b->kflags = kr ? kraken_flags(curr->even, curr->odd, len > CHUNK128K) : 0;
        int advances = (!zero || (curr->even == 1 && curr->clen_even_minus1 == 7)) && !dedup;
        if (!zero && !dedup) owners_set(&own, cursor, (int64_t)(n - 1));
        abs += len;
        if (advances) cursor += total;
    }
    free(s);
    owners_free(&own);
    if (why) {
        free(r);
        return bad(e, why);
    }
    out->b = r;
    out->n = n;
    return 0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static int has_u64(const uint64_t *v, size_t n, uint64_t k) { return bsearch(&k, v, n, sizeof k, cmp_u64) != NULL; }

// Inserts the whole 256 KiB pages the STD stream leaves out (the u2c table's anchors say where), then
// checks every fidx boundary. sparse: the sparse-fidx rules (marked fidx entries must start holes).
static int restore_pages(const layout *l, const blist *in, int sparse, blist *out, mp_err *e) {
    out->b = NULL;
    out->n = 0;
    if (!in->n) return bad(e, "no blocks");
    uint64_t walked_end = in->b[in->n - 1].off + in->b[in->n - 1].len, declared = 0;
    if (!mount_end(l, &declared) || declared <= walked_end || (declared - walked_end) % PAGE != 0)
        return bad(e, "the map needs an exact fidx mount end and missing whole pages");
    int *std_idx = malloc((size_t)l->num_cblock * sizeof *std_idx);
    int nstd = 0;
    if (!std_idx) return bad(e, "out of memory");
    for (int i = 0; i < l->num_cblock; i++)
        if (!l->cb[i].run) std_idx[nstd++] = i;
    const char *why = NULL;
    if ((size_t)nstd != in->n + 1) why = "the map needs one STD per block and a terminator";
    else if (l->num_ublocks <= 0 || declared <= ((uint64_t)l->num_ublocks - 1) * PAGE ||
             declared > (uint64_t)l->num_ublocks * PAGE)
        why = "the mount end disagrees with the ublock count";
    if (why) {
        free(std_idx);
        return bad(e, why);
    }
    uint64_t *anchor = malloc((size_t)l->num_cblock * sizeof *anchor);   // UINT64_MAX: none
    int *page_idx = malloc((size_t)l->num_ublocks * sizeof *page_idx);
    uint64_t gap_room = declared - walked_end;
    size_t cap = in->n + (size_t)(gap_room / PAGE);
    mp_naps_block *r = calloc(cap ? cap : 1, sizeof *r);
    uint64_t *starts = malloc((size_t)nstd * sizeof *starts);
    size_t *new_index = malloc((in->n ? in->n : 1) * sizeof *new_index);
    if (!anchor || !page_idx || !r || !starts || !new_index) {
        why = "out of memory";
        goto done;
    }
    for (int i = 0; i < l->num_cblock; i++) anchor[i] = UINT64_MAX;
    int prev = -1;
    for (int page = 0; page < l->num_ublocks; page++) {
        if (page / 8 >= l->num_u2c) {
            why = "the u2c table is too short";
            goto done;
        }
        int64_t ix = (int64_t)l->u2c_base[page / 8] + (page % 8 == 0 ? 0 : l->u2c_delta[(size_t)(page / 8) * 7 + page % 8 - 1]);
        if (ix < prev || ix >= l->num_cblock || l->cb[ix].run) {
            why = "an invalid u2c anchor";
            goto done;
        }
        prev = page_idx[page] = (int)ix;
        anchor[ix] = (uint64_t)page * PAGE;
    }
    size_t n = 0;
    uint64_t shift = 0;
    for (int i = 0; i < nstd; i++) {
        uint64_t logical = ((size_t)i < in->n ? in->b[i].off : walked_end) + shift;
        uint64_t a = anchor[std_idx[i]];
        if (a != UINT64_MAX && logical < a) {
            uint64_t gap = (a - logical + PAGE - 1) / PAGE * PAGE;
            if (gap > declared - walked_end - shift) {
                why = "the anchors demand too many sparse pages";
                goto done;
            }
            for (uint64_t added = 0; added < gap; added += PAGE) {
                if (n >= cap) {
                    why = "the anchors demand too many sparse pages";
                    goto done;
                }
                mp_naps_block *h = &r[n++];
                memset(h, 0, sizeof *h);
                h->off = logical + added;
                h->len = PAGE;
                h->zero = 1;
                h->source = -1;
            }
            shift += gap;
            logical += gap;
        }
        if ((logical & (PAGE - 1)) != l->cb[std_idx[i]].uoff) {
            why = "a logical offset disagrees with an STD record";
            goto done;
        }
        starts[i] = logical;
        if ((size_t)i == in->n) break;
        mp_naps_block b = in->b[i];
        if (b.alias && b.source >= 0) {
            if ((size_t)b.source >= (size_t)i) {
                why = "an alias of a later block";
                goto done;
            }
            b.source = (int64_t)new_index[b.source];
        }
        if (n >= cap) {
            why = "too many blocks";
            goto done;
        }
        new_index[i] = n;
        b.off = logical;
        r[n++] = b;
    }
    if (starts[nstd - 1] != declared) {
        why = "the coverage does not reach the fidx mount end";
        goto done;
    }
    {
        int first = 0;
        for (int page = 0; page < l->num_ublocks; page++) {
            while (first + 1 < nstd && starts[first] < (uint64_t)page * PAGE) ++first;
            if (std_idx[first] != page_idx[page]) {
                why = "a u2c page disagrees with the block map";
                goto done;
            }
        }
    }
    {
        // starts is sorted (offsets only grow); the holes' starts, sorted too
        uint64_t *holes = malloc((n ? n : 1) * sizeof *holes);
        size_t nh = 0;
        if (!holes) {
            why = "out of memory";
            goto done;
        }
        for (size_t i = 0; i < n; i++)
            if (r[i].zero) holes[nh++] = r[i].off;
        uint64_t prev_off = 0;
        int limit = sparse ? l->num_files - 1 : l->num_files;
        for (int i = 0; i < limit; i++) {
            uint64_t f = l->fidx[i];
            int boundary = sparse && l->fidx_type[i] == SPARSE_MARKER ? has_u64(holes, nh, f)
                                                                      : has_u64(starts, (size_t)nstd, f);
            if (f < prev_off || f > declared || !boundary) {
                why = "a fidx entry disagrees with the block boundaries";
                break;
            }
            prev_off = f;
        }
        free(holes);
        if (why) goto done;
    }
    out->b = r;
    out->n = n;
    r = NULL;
done:
    free(std_idx);
    free(anchor);
    free(page_idx);
    free(r);
    free(starts);
    free(new_index);
    return why ? bad(e, why) : 0;
}

static int sparse_fidx(const layout *l, uint64_t image_len, blist *out, mp_err *e) {
    out->b = NULL;
    out->n = 0;
    int markers = 0;
    for (int i = 0; i + 1 < l->num_files; i++)
        if (l->fidx_type[i] == SPARSE_MARKER) markers++;
    uint64_t end;
    if (!markers) return bad(e, "sparse-fidx needs an fidx entry with the 0x40 marker");
    if (!mount_end(l, &end)) return bad(e, "sparse-fidx needs an exact fidx mount end");
    blist w;
    if (walk_unmarked(l, image_len, &w, e) != 0) return -1;
    int rc = restore_pages(l, &w, 1, out, e);
    free(w.b);
    return rc;
}

// What a read gives, regardless of RUN bookkeeping and how zeros are written.
static mp_naps_block norm(const blist *v, size_t i) {
    mp_naps_block x = v->b[i];
    if (x.alias && (x.source < 0 || (size_t)x.source >= i || v->b[x.source].len < x.len)) x.zero = 1;
    if (x.zero) {
        mp_naps_block z;
        memset(&z, 0, sizeof z);
        z.off = x.off;
        z.len = x.len;
        z.zero = 1;
        return z;
    }
    if (!x.kraken) x.even_len = 0;
    if (!x.kraken && !x.alias) x.clen = x.len;
    return x;
}

static int equivalent(const blist *a, const blist *b) {
    if (a->n != b->n) return 0;
    for (size_t i = 0; i < a->n; i++) {
        mp_naps_block x = norm(a, i), y = norm(b, i);
        if (x.coff != y.coff || x.clen != y.clen || x.even_len != y.even_len || x.off != y.off || x.len != y.len ||
            x.kraken != y.kraken || x.zero != y.zero || x.alias != y.alias || x.source != y.source ||
            x.kflags != y.kflags)
            return 0;
    }
    return 1;
}

static uint64_t plan_size(const blist *v) {
    uint64_t end = 0;
    for (size_t i = 0; i < v->n; i++)
        if (v->b[i].off + v->b[i].len > end) end = v->b[i].off + v->b[i].len;
    return (end + 0xFFFF) & ~0xFFFFull;
}

void mp_naps_plan_free(mp_naps_plan *p) {
    if (!p) return;
    free(p->b);
    p->b = NULL;
    p->n = 0;
}

int mp_naps_plan_of(const uint8_t *naps, size_t len, uint64_t image_len, mp_naps_probe probe, void *ctx,
                    mp_naps_plan *out, mp_err *e) {
    memset(out, 0, sizeof *out);
    layout l;
    mp_err pe = {{0}};
    if (parse(naps, len, &l, &pe) != 0) {
        mp_err_set(e, "damaged package (%s)", pe.msg);
        return -1;
    }
    enum { NC = 4 };
    static const char *ids[NC] = {"compatible-coffset-v1", "native-span-v1", "indexed-coffset-v1", "sparse-fidx-v1"};
    blist cand[NC];
    int ok[NC] = {0};
    char diag[512] = "";
    size_t dn = 0;
    uint64_t declared = 0;
    int has_end = mount_end(&l, &declared);
    for (int k = 0; k < NC; k++) {
        cand[k].b = NULL;
        cand[k].n = 0;
        if (k == 3 && (ok[0] || ok[1] || ok[2])) break;   // the last resort
        mp_err ce = {{0}};
        int rc;
        if (k == 0) rc = walk(&l, image_len, 0, &cand[k], &ce);
        else if (k == 1) rc = walk(&l, image_len, 1, &cand[k], &ce);
        else if (k == 2) {
            blist plain;
            rc = walk(&l, image_len, 0, &plain, &ce);
            if (rc == 0) rc = restore_pages(&l, &plain, 0, &cand[k], &ce);
            free(plain.b);
        } else {
            rc = sparse_fidx(&l, image_len, &cand[k], &ce);
        }
        if (rc == 0 && has_end && (!cand[k].n || cand[k].b[cand[k].n - 1].off + cand[k].b[cand[k].n - 1].len != declared)) {
            snprintf(ce.msg, sizeof ce.msg, "the coverage disagrees with the fidx mount end");
            rc = -1;
        }
        if (rc == 0 && plan_size(&cand[k]) > MAX_MOUNT) {
            snprintf(ce.msg, sizeof ce.msg, "an absurd mount size");
            rc = -1;
        }
        if (rc != 0) {
            free(cand[k].b);
            cand[k].b = NULL;
            if (dn < sizeof diag) dn += (size_t)snprintf(diag + dn, sizeof diag - dn, "%s%s: %s", dn ? "; " : "", ids[k], ce.msg);
            continue;
        }
        ok[k] = 1;
        // one reading the same as an earlier one adds nothing: drop it now (less memory)
        for (int j = 0; j < k; j++)
            if (ok[j] && equivalent(&cand[j], &cand[k])) {
                free(cand[k].b);
                cand[k].b = NULL;
                ok[k] = 0;
                break;
            }
    }
    layout_free(&l);
    int count = 0;
    for (int k = 0; k < NC; k++) count += ok[k];
    if (count > 1) {   // they disagree: keep the ones whose mount holds a readable PFS
        for (int k = 0; k < NC; k++) {
            if (!ok[k]) continue;
            mp_naps_plan t = {cand[k].b, cand[k].n, plan_size(&cand[k]), ids[k], 0};
            mp_err ce = {{0}};
            if (!probe || probe(ctx, &t, &ce) != 0) {
                ok[k] = 0;
                free(cand[k].b);
                cand[k].b = NULL;
                count--;
                if (dn < sizeof diag)
                    dn += (size_t)snprintf(diag + dn, sizeof diag - dn, "%s%s: %s", dn ? "; " : "", ids[k],
                                           probe ? ce.msg : "no way to tell");
            }
        }
        if (count > 1) {
            for (int k = 0; k < NC; k++) free(cand[k].b);
            mp_err_set(e, "damaged package (its block map can be read in two ways that both hold a PFS)");
            return -1;
        }
    }
    for (int k = 0; k < NC; k++) {
        if (!ok[k]) continue;
        out->b = cand[k].b;
        out->n = cand[k].n;
        out->mount_size = plan_size(&cand[k]);
        out->dialect = ids[k];
        for (size_t i = 0; i < out->n; i++) out->kraken |= out->b[i].kraken;
        return 0;
    }
    mp_err_set(e, "damaged package, or a block map MaxPack does not know (%s)", diag);
    return -1;
}

// ---- the mount ---------------------------------------------------------------------------------

typedef struct mctx {
    mp_kraken *k;
    uint8_t *comp, *blk;
    struct mctx *next;
} mctx;

typedef struct {
    mp_image *inner;
    int own;
    mp_naps_block *b;
    size_t n;
    uint64_t image_size;
    mp_mutex *mu;
    mctx *idle;
    unsigned nslots;   // decoded Kraken blocks kept: slot i % nslots holds block tag - 1
    uint64_t *tag;
    uint8_t *slots;
} mount;

#define SLOTS 64   // 16 MiB: a reader that takes a file starting or ending inside a block decodes it once

static void ctx_free(mctx *c) {
    if (!c) return;
    mp_kraken_free(c->k);
    free(c->comp);
    free(c->blk);
    free(c);
}

static mctx *ctx_take(mount *m, mp_err *e) {
    mp_mutex_lock(m->mu);
    mctx *c = m->idle;
    if (c) m->idle = c->next;
    mp_mutex_unlock(m->mu);
    if (c) return c;
    c = calloc(1, sizeof *c);
    if (c && (!(c->k = mp_kraken_new()) || !(c->comp = malloc(PAGE)) || !(c->blk = malloc(PAGE)))) {
        ctx_free(c);
        c = NULL;
    }
    if (!c) mp_err_set(e, "out of memory");
    return c;
}

static void ctx_give(mount *m, mctx *c) {
    mp_mutex_lock(m->mu);
    c->next = m->idle;
    m->idle = c;
    mp_mutex_unlock(m->mu);
}

static int read_inner(mount *m, uint8_t *out, size_t n, uint64_t off, mp_err *e) {
    size_t got = 0;
    if (mp_image_pread(m->inner, out, n, off, &got, e) != 0) return -1;
    if (got != n) {
        mp_err_set(e, "damaged package: a block is outside pfs_image.dat");
        return -1;
    }
    return 0;
}

// Kraken block i, whole, into dst (b->len bytes).
static int kdecode(mount *m, mctx *c, size_t i, uint8_t *dst, mp_err *e) {
    const mp_naps_block *b = &m->b[i];
    if (b->clen == 0 || b->clen > PAGE || b->coff > m->image_size || b->clen > m->image_size - b->coff) {
        mp_err_set(e, "damaged package: a compressed block at %" PRIu64 " of the mount is outside pfs_image.dat",
                   b->off);
        return -1;
    }
    if (read_inner(m, c->comp, b->clen, b->coff, e) != 0) return -1;
    int rc = mp_kraken_decode(c->k, c->comp, b->clen, b->kflags, b->len > CHUNK128K ? b->even_len : 0, dst, b->len);
    if (rc == MP_KRAKEN_OK) return 0;
    if (rc == MP_KRAKEN_NOMEM) mp_err_set(e, "out of memory");
    else if (rc == MP_KRAKEN_UNSUPPORTED)
        mp_err_set(e, "the block at %" PRIu64 " of the package's mount uses a Kraken coding MaxPack does not read",
                   b->off);
    else
        mp_err_set(e, "damaged package: the Kraken block at %" PRIu64 " of the mount does not decode", b->off);
    return -1;
}

// Bytes [within, within + n) of block i, decoded.
static int part(mount *m, mctx *c, size_t i, uint32_t within, uint32_t n, uint8_t *out, int depth, mp_err *e) {
    const mp_naps_block *b = &m->b[i];
    if (depth > 32) {
        mp_err_set(e, "damaged package: an alias chain too deep");
        return -1;
    }
    if (b->zero || b->len == 0) {
        memset(out, 0, n);
        return 0;
    }
    if (b->alias) {
        // the source's first bytes, zeros past its end
        uint32_t k = 0;
        if (b->source >= 0 && (size_t)b->source < m->n) {
            uint32_t slen = m->b[b->source].len;
            k = within < slen ? (slen - within < n ? slen - within : n) : 0;
            if (k && part(m, c, (size_t)b->source, within, k, out, depth + 1, e) != 0) return -1;
        }
        memset(out + k, 0, n - k);
        return 0;
    }
    if (!b->kraken) {
        if (b->clen < b->len || b->coff > m->image_size || b->len > m->image_size - b->coff) {
            mp_err_set(e, "damaged package: a stored block at %" PRIu64 " of the mount is outside pfs_image.dat",
                       b->off);
            return -1;
        }
        return read_inner(m, out, n, b->coff + within, e);
    }
    if (within == 0 && n == b->len) return kdecode(m, c, i, out, e);
    unsigned s = (unsigned)(i % m->nslots);
    mp_mutex_lock(m->mu);
    int hit = m->tag[s] == (uint64_t)i + 1;
    if (hit) memcpy(out, m->slots + (size_t)s * PAGE + within, n);
    mp_mutex_unlock(m->mu);
    if (hit) return 0;
    if (kdecode(m, c, i, c->blk, e) != 0) return -1;
    memcpy(out, c->blk + within, n);
    mp_mutex_lock(m->mu);
    memcpy(m->slots + (size_t)s * PAGE, c->blk, b->len);
    m->tag[s] = (uint64_t)i + 1;
    mp_mutex_unlock(m->mu);
    return 0;
}

static int mount_read(mp_image *img, void *buf, size_t len, uint64_t off, mp_err *e) {
    mount *m = img->impl;
    mctx *c = ctx_take(m, e);
    if (!c) return -1;
    uint8_t *o = buf;
    int rc = 0;
    while (rc == 0 && len) {
        // the last block that starts at or before off
        size_t lo = 0, hi = m->n;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (m->b[mid].off <= off)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == 0) {
            mp_err_set(e, "damaged package: a part of its mount maps to no block");
            rc = -1;
            break;
        }
        size_t i = lo - 1;
        const mp_naps_block *b = &m->b[i];
        size_t n;
        if (off >= b->off + b->len) {   // between blocks, or past the last one: zeros
            uint64_t to = i + 1 < m->n ? m->b[i + 1].off : img->size;
            n = to - off < len ? (size_t)(to - off) : len;
            memset(o, 0, n);
        } else {
            uint32_t within = (uint32_t)(off - b->off);
            n = b->len - within < len ? b->len - within : len;
            rc = part(m, c, i, within, (uint32_t)n, o, 0, e);
        }
        o += n;
        off += n;
        len -= n;
    }
    ctx_give(m, c);
    return rc;
}

static void mount_close(mp_image *img) {
    mount *m = img->impl;
    while (m->idle) {
        mctx *c = m->idle;
        m->idle = c->next;
        ctx_free(c);
    }
    mp_mutex_free(m->mu);
    free(m->tag);
    free(m->slots);
    if (m->own) {
        free(m->b);
        mp_image_close(m->inner);
    }
    free(m);
}

int mp_naps_mount(mp_image *inner, mp_naps_plan *plan, int own, const char *name, mp_image **out, mp_err *e) {
    *out = NULL;
    mp_image *img = calloc(1, sizeof *img);
    mount *m = calloc(1, sizeof *m);
    if (img) img->name = mp_strdup(name ? name : "");
    if (m) {
        m->mu = mp_mutex_new();
        m->nslots = SLOTS;
        m->tag = calloc(SLOTS, sizeof *m->tag);
        m->slots = malloc((size_t)SLOTS * PAGE);
    }
    if (!img || !img->name || !m || !m->mu || !m->tag || !m->slots) {
        if (m) {
            mp_mutex_free(m->mu);
            free(m->tag);
            free(m->slots);
        }
        free(m);
        if (img) free(img->name);
        free(img);
        if (own) {
            mp_naps_plan_free(plan);
            mp_image_close(inner);
        }
        mp_err_set(e, "out of memory");
        return -1;
    }
    m->inner = inner;
    m->own = own;
    m->b = plan->b;
    m->n = plan->n;
    m->image_size = inner->size;
    if (own) {
        plan->b = NULL;
        plan->n = 0;
    }
    img->size = plan->mount_size;
    img->compressed = plan->kraken;
    img->impl = m;
    img->read = mount_read;
    img->close = mount_close;
    *out = img;
    return 0;
}
