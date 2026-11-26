// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_complete.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_platform.h"
#include "mp_playgo.h"

#define RIGHT_SPRX "sce_sys/about/right.sprx"
#define DUMP_INDEX "ampr_emu.index"
#define DUMP_INDEX_MAX (512u << 20)
#define DUMP_INDEX_ROWS 2000000u   // the most ampr-emu-ng takes

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// a[0..n) equals the NUL-terminated b, apart from ASCII case
static int ieq_n(const char *a, size_t n, const char *b) {
    for (size_t i = 0; i < n; i++, b++)
        if (!*b || lower((unsigned char)a[i]) != lower((unsigned char)*b)) return 0;
    return !*b;
}

static int ends_ci(const char *s, size_t n, const char *tail) {
    size_t k = strlen(tail);
    return n >= k && ieq_n(s + n - k, k, tail);
}

// The first segment of path is name, and something follows it (a folder at the root).
static int in_root_folder(const char *path, const char *name) {
    const char *slash = strchr(path, '/');
    return slash && slash[1] && ieq_n(path, (size_t)(slash - path), name);
}

int mp_complete_is_junk(const char *path) {
    static const char *const folders[] = {"__macosx", ".spotlight-v100", ".fseventsd", ".trashes", ".temporaryitems",
                                          ".appledouble", "$recycle.bin", "system volume information"};
    static const char *const files[] = {".ds_store", "thumbs.db", "desktop.ini", "ehthumbs.db", "ehthumbs_vista.db"};
    const char *seg = path;
    for (;;) {
        const char *slash = strchr(seg, '/');
        size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (slash) {
            for (size_t i = 0; i < sizeof folders / sizeof *folders; i++)
                if (ieq_n(seg, len, folders[i])) return 1;
            seg = slash + 1;
            continue;
        }
        if (len >= 2 && seg[0] == '.' && seg[1] == '_') return 1;   // AppleDouble
        for (size_t i = 0; i < sizeof files / sizeof *files; i++)
            if (ieq_n(seg, len, files[i])) return 1;
        return 0;
    }
}

int mp_complete_is_leftover(const char *path) {
    size_t n = strlen(path);
    if (in_root_folder(path, "decrypted")) return 1;
    if (ends_ci(path, n, ".auth_info")) return 1;
    return ieq_n(path, n, "entitlements.txt") || ieq_n(path, n, "entitlement_key.dat");
}

int mp_complete_is_dlc(const char *path) {
    size_t n = strlen(path);
    if (ieq_n(path, n, "dlc_emu.ini")) return 1;
    // addcont0/ to addcont99/ at the root, as MaxPack names them (no leading zeros), with something in it
    if (n < 9 || !ieq_n(path, 7, "addcont")) return 0;
    const char *d = path + 7;
    size_t digits = 0;
    while (digits < 3 && d[digits] >= '0' && d[digits] <= '9') digits++;
    if (digits == 0 || digits > 2 || (digits == 2 && d[0] == '0')) return 0;
    return d[digits] == '/' && d[digits + 1] != 0;
}

static long find_hash(const uint64_t *h, size_t n, uint64_t want) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (h[mid] < want)
            lo = mid + 1;
        else if (h[mid] > want)
            hi = mid;
        else
            return (long)mid;
    }
    return -1;
}

void mp_complete_free(mp_complete *c) {
    if (!c) return;
    for (size_t i = 0; i < c->npaths; i++) free(c->paths[i]);
    c->npaths = 0;
}

void mp_complete_counts_only(mp_complete *c) { mp_complete_free(c); }

int mp_complete_proves_leftovers(const mp_complete *c) {
    return c->checkable && c->source && !strcmp(c->source, "playgo");
}

static int add_path(mp_complete *c, const char *p) {
    if (c->npaths >= MP_COMPLETE_PATHS) return 0;
    char *copy = mp_strdup(p);
    if (!copy) return -1;
    c->paths[c->npaths++] = copy;
    return 0;
}

// Against the PlayGo table's hashes h[0..k) (sorted); with h NULL, nothing is checked and only the
// leftovers by their names are told. c is zeroed, with its reason.
static int check_table(const mp_cfile *files, size_t n, uint64_t *h, size_t k, mp_complete *c, uint8_t *cls) {
    c->checkable = h != NULL;
    c->source = c->checkable ? "playgo" : "";
    c->reason = "";
    uint8_t *seen = NULL;
    long right = -1;
    if (c->checkable) {
        seen = calloc(k, 1);
        if (!seen) return -1;
        right = find_hash(h, k, mp_flt_hash(RIGHT_SPRX));
        c->listed = k - (right >= 0);
    }
    for (size_t i = 0; i < n; i++) {
        const char *p = files[i].path;
        long at = c->checkable ? find_hash(h, k, mp_flt_hash(p)) : -1;
        int cl;
        if (at >= 0) {
            cl = MP_CF_LISTED;
            seen[at] = 1;
        } else if (mp_complete_is_junk(p) || in_root_folder(p, "sce_sys") || mp_complete_is_dlc(p)) {
            cl = MP_CF_IGNORED;
        } else if (mp_complete_is_leftover(p)) {
            cl = MP_CF_LEFTOVER;
        } else {
            cl = c->checkable ? MP_CF_EXTRA : MP_CF_IGNORED;
        }
        if (cls) cls[i] = (uint8_t)cl;
        if (cl == MP_CF_LEFTOVER) {
            c->leftover_count++;
            c->leftover_bytes += files[i].size;
        }
        if (!c->checkable || (cl != MP_CF_EXTRA && cl != MP_CF_LEFTOVER)) continue;
        c->extra_count++;
        c->extra_bytes += files[i].size;
        if (add_path(c, p) != 0) {
            free(seen);
            mp_complete_free(c);
            return -1;
        }
    }
    for (size_t i = 0; c->checkable && i < k; i++)
        if (seen[i] && (long)i != right) c->present++;
    c->missing = c->listed - c->present;
    free(seen);
    return 0;
}

// ---- the dump tool's list ------------------------------------------------------------------------

// ampr_emu.index (AMPRIDX3), as write_index of drakmor/ampr_emu's tools/build_ampr_index.py writes
// it (GPL-3.0; MaxPack's src/pack/mp_idx3.c writes and checks the same): a 48-byte header
// ("AMPRIDX3", version 3 and record size 24 as u32 at 8 and 12, the rows and the bytes of the path
// blob as u64 at 16 and 24), then one 24-byte record per file (offset and length of its path in the
// blob as u32, size as u64, date as i64), then the blob of NUL-terminated "/app0/..." paths. Only the
// rows are read: the hash slots after them are the runtime's.
typedef struct {
    const char *path;   // without "/app0/", NUL-terminated in the list's bytes
    uint64_t size;
    int used;
} drow;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

// 0 with its rows (malloc); -1 when it is not such a list, or is cut or damaged; -2 out of memory.
static int dump_rows(const uint8_t *d, size_t n, drow **out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (n < 48 || memcmp(d, "AMPRIDX3", 8) || rd32(d + 8) != 3 || rd32(d + 12) != 24) return -1;
    uint64_t rows = rd64(d + 16), blob = rd64(d + 24);
    if (rows == 0 || rows > DUMP_INDEX_ROWS || blob > n || 48 + 24 * rows > n - blob) return -1;
    const uint8_t *bp = d + 48 + 24 * rows;
    drow *v = malloc((size_t)rows * sizeof *v);
    if (!v) return -2;
    for (uint64_t i = 0; i < rows; i++) {
        const uint8_t *r = d + 48 + 24 * i;
        uint64_t off = rd32(r), len = rd32(r + 4);
        if (off + len >= blob || bp[off + len] != 0 || len <= 6 || memchr(bp + off, 0, (size_t)len) ||
            memcmp(bp + off, "/app0/", 6)) {
            free(v);
            return -1;
        }
        v[i].path = (const char *)bp + off + 6;
        v[i].size = rd64(r + 8);
        v[i].used = 0;
    }
    *out = v;
    *count = (size_t)rows;
    return 0;
}

// strcmp with ASCII letters in lower case, as the runtime matches paths
static int fold_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}
static int cmp_drow(const void *a, const void *b) { return fold_cmp(((const drow *)a)->path, ((const drow *)b)->path); }

static long find_row(const drow *v, size_t n, const char *path) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = fold_cmp(v[mid].path, path);
        if (c < 0)
            lo = mid + 1;
        else if (c > 0)
            hi = mid;
        else
            return (long)mid;
    }
    return -1;
}

// What a fine dump differs from its list in, which is never counted: info and checksum files that are not the game's, the
// emulators the user changes, what the game writes while it runs, the runtime's files and the list.
static int not_counted(const char *path) {
    size_t n = strlen(path);
    if (ends_ci(path, n, ".nfo") || ends_ci(path, n, ".sfv") || ends_ci(path, n, ".diz") || ends_ci(path, n, ".log"))
        return 1;
    if (in_root_folder(path, "fakelib") || in_root_folder(path, "fakelib2")) return 1;
    if (ieq_n(path, n, "dlc_emu.ini") || ieq_n(path, n, "ampr_commands.bin") || ieq_n(path, n, DUMP_INDEX)) return 1;
    for (const char *seg = path;;) {
        const char *slash = strchr(seg, '/');
        if (!slash) break;
        if (ieq_n(seg, (size_t)(slash - seg), "saved")) return 1;
        seg = slash + 1;
    }
    return mp_complete_is_junk(path);
}

// Against the dump tool's rows v[0..k). 0 checked; 1 the list cannot be trusted (c says why, and the
// caller tells only the leftovers); -1 out of memory.
static int check_dump_index(const mp_cfile *files, size_t n, drow *v, size_t k, mp_complete *c, uint8_t *cls) {
    for (size_t i = 0; i < n; i++)
        if (ieq_n(files[i].path, strlen(files[i].path), "ampr_assets.index") ||
            ieq_n(files[i].path, strlen(files[i].path), "ampr_zstd.index")) {
            c->reason = "dump_index_packed";   // its files are in packs: not there loose
            return 1;
        }
    qsort(v, k, sizeof *v, cmp_drow);
    c->checkable = 1;
    c->source = "dump_index";
    c->reason = "";
    for (size_t i = 0; i < n; i++) {
        const char *p = files[i].path;
        long at = find_row(v, k, p);
        int cl;
        if (at >= 0) {
            v[at].used = 1;
            if (!not_counted(v[at].path)) {
                if (v[at].size == files[i].size)
                    c->present++;
                else
                    c->size_mismatch++;
            }
            cl = MP_CF_LISTED;
        } else if (mp_complete_is_junk(p) || ieq_n(p, strlen(p), DUMP_INDEX) || mp_complete_is_dlc(p)) {
            cl = MP_CF_IGNORED;
        } else {
            cl = MP_CF_EXTRA;
            c->extra_count++;
            c->extra_bytes += files[i].size;
            if (add_path(c, p) != 0) return -1;
        }
        // the dump tool writes its leftovers too: its list does not prove them not the game's
        if (cl != MP_CF_IGNORED && mp_complete_is_leftover(p)) {
            cl = MP_CF_LEFTOVER;
            c->leftover_count++;
            c->leftover_bytes += files[i].size;
        }
        if (cls) cls[i] = (uint8_t)cl;
    }
    for (size_t i = 0; i < k; i++) {
        if (not_counted(v[i].path)) continue;
        c->listed++;
        if (!v[i].used) c->missing++;
    }
    // nothing of it is here, or most of what is here has other sizes: another game, another version
    uint64_t found = c->present + c->size_mismatch;
    if (c->listed == 0 || c->present == 0 || (c->size_mismatch > 8 && c->size_mismatch * 10 > found)) {
        c->reason = "dump_index_mismatch";
        return 1;
    }
    return 0;
}

int mp_complete_check_lists(const mp_cfile *files, size_t n, const uint8_t *table, size_t table_len,
                            const uint8_t *dump_index, size_t dump_index_len, mp_complete *c, uint8_t *cls) {
    memset(c, 0, sizeof *c);
    uint64_t *h = NULL;
    size_t k = 0;
    const char *why = "no_table";
    if (table && mp_playgo_parse(table, table_len, &h, &k, &why) != 0) {
        if (!strcmp(why, "unreadable")) return -1;   // out of memory
        h = NULL;
    }
    if (h || !dump_index) {   // the table wins; without either, why the table could not be used
        int rc = check_table(files, n, h, k, c, cls);
        if (!h) c->reason = why;
        free(h);
        return rc;
    }
    drow *v = NULL;
    size_t rows = 0;
    int pr = dump_rows(dump_index, dump_index_len, &v, &rows);
    if (pr == -2) return -1;
    int dr = pr == 0 ? check_dump_index(files, n, v, rows, c, cls) : 1;
    const char *dump_why = pr != 0 ? "dump_index_damaged" : c->reason;
    free(v);
    if (dr < 0) {
        mp_complete_free(c);
        return -1;
    }
    if (dr == 0) return 0;
    // not usable: only the leftovers by their names, and why
    mp_complete_free(c);
    memset(c, 0, sizeof *c);
    int rc = check_table(files, n, NULL, 0, c, cls);
    c->reason = dump_why;
    return rc;
}

int mp_complete_check(const mp_cfile *files, size_t n, const uint8_t *table, size_t table_len, mp_complete *c,
                      uint8_t *cls) {
    return mp_complete_check_lists(files, n, table, table_len, NULL, 0, c, cls);
}

static long find_ci(mp_source *s, const char *path) {
    long i = mp_source_find(s, path);
    if (i >= 0) return i;
    size_t n = strlen(path);
    for (size_t k = 0; k < mp_source_count(s); k++)
        if (ieq_n(path, n, mp_source_entry(s, k)->path)) return (long)k;
    return -1;
}

// The bytes of the source's file at path (malloc): 0 read, 1 not there, -1 not readable (too big or
// an error); *out is never NULL when it was read, even when empty.
static int read_small(mp_source *s, const char *path, uint64_t max, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    long t = find_ci(s, path);
    if (t < 0) return 1;
    uint64_t size = mp_source_entry(s, (size_t)t)->size;
    uint8_t *b = size <= max ? malloc((size_t)size + 1) : NULL;
    mp_err re = {{0}};
    if (!b || mp_source_read(s, (size_t)t, 0, b, (size_t)size, &re) != 0) {
        free(b);
        return -1;
    }
    *out = b;
    *len = (size_t)size;
    return 0;
}

int mp_complete_source(mp_source *s, mp_complete *c, uint8_t *cls, mp_err *e) {
    size_t n = mp_source_count(s);
    mp_cfile *files = malloc((n ? n : 1) * sizeof *files);
    if (!files) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        files[i].path = mp_source_entry(s, i)->path;
        files[i].size = mp_source_entry(s, i)->size;
    }
    uint8_t *table = NULL, *dindex = NULL;
    size_t len = 0, dlen = 0;
    int unreadable = read_small(s, MP_PLAYGO_TABLE_PATH, MP_PLAYGO_TABLE_MAX, &table, &len) < 0;
    // the dump tool's list at the root; one that cannot be read is a damaged one
    static const uint8_t none[1] = {0};
    int dr = read_small(s, DUMP_INDEX, DUMP_INDEX_MAX, &dindex, &dlen);
    int rc = mp_complete_check_lists(files, n, table, len, dr < 0 ? none : dindex, dr < 0 ? 0 : dlen, c, cls);
    if (rc == 0 && unreadable && !c->checkable && !strcmp(c->reason, "no_table")) c->reason = "unreadable";
    if (rc != 0) mp_err_set(e, "out of memory");
    free(table);
    free(dindex);
    free(files);
    return rc;
}

void mp_complete_json(const mp_complete *c, int with_paths, mp_jbuf *b) {
    mp_jbuf_raw(b, "{\"checkable\":");
    mp_jbuf_raw(b, c->checkable ? "true" : "false");
    mp_jbuf_raw(b, ",\"reason\":");
    mp_jbuf_str(b, c->reason ? c->reason : "");
    mp_jbuf_raw(b, ",\"source\":");
    mp_jbuf_str(b, c->source ? c->source : "");
    static const char *const names[] = {"listed", "present", "missing", "size_mismatch", "extra_count", "extra_bytes",
                                        "leftover_count", "leftover_bytes"};
    const uint64_t values[] = {c->listed, c->present, c->missing, c->size_mismatch, c->extra_count, c->extra_bytes,
                               c->leftover_count, c->leftover_bytes};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        mp_jbuf_raw(b, ",\"");
        mp_jbuf_raw(b, names[i]);
        mp_jbuf_raw(b, "\":");
        mp_jbuf_u64(b, values[i]);
    }
    if (with_paths) {
        mp_jbuf_raw(b, ",\"extra_paths\":[");
        for (size_t i = 0; i < c->npaths; i++) {
            if (i) mp_jbuf_raw(b, ",");
            mp_jbuf_str(b, c->paths[i]);
        }
        mp_jbuf_raw(b, "]");
    }
    mp_jbuf_raw(b, "}");
}

static const char *bytes_text(uint64_t v, char buf[32]) {
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
    double x = (double)v;
    int u = 0;
    while (x >= 1000.0 && u < 4) {
        x /= 1000.0;
        u++;
    }
    if (u == 0)
        snprintf(buf, 32, "%llu B", (unsigned long long)v);
    else
        snprintf(buf, 32, "%.2f %s", x, units[u]);
    return buf;
}

static int from_dump_index(const mp_complete *c) { return c->source && !strcmp(c->source, "dump_index"); }

void mp_complete_summary(const mp_complete *c, char *buf, size_t cap) {
    if (c->checkable && from_dump_index(c)) {
        unsigned long long n = c->listed, m = c->missing, k = c->size_mismatch;
        if (!m && !k)
            snprintf(buf, cap, "all %llu files the dump tool wrote are here (its list does not prove it got every file of the game)", n);
        else if (m && k)
            snprintf(buf, cap, "damaged copy: %llu of the %llu files the dump tool wrote %s missing, %llu %s another size", m, n,
                     m == 1 ? "is" : "are", k, k == 1 ? "has" : "have");
        else if (m)
            snprintf(buf, cap, "damaged copy: %llu of the %llu files the dump tool wrote %s missing", m, n, m == 1 ? "is" : "are");
        else
            snprintf(buf, cap, "damaged copy: %llu of the %llu files the dump tool wrote %s another size", k, n, k == 1 ? "has" : "have");
    } else if (c->checkable && !c->missing) {
        snprintf(buf, cap, "complete: all %llu files of the original package", (unsigned long long)c->listed);
    } else if (c->checkable) {
        snprintf(buf, cap, "incomplete: %llu of the %llu files of the original package %s missing",
                 (unsigned long long)c->missing, (unsigned long long)c->listed, c->missing == 1 ? "is" : "are");
    } else {
        const char *r = c->reason ? c->reason : "";
        const char *why = !strcmp(r, "unknown_format") ? "its " MP_PLAYGO_TABLE_PATH " is of a format MaxPack does not know"
                          : !strcmp(r, "damaged")      ? "its " MP_PLAYGO_TABLE_PATH " is damaged"
                          : !strcmp(r, "unreadable")   ? "its " MP_PLAYGO_TABLE_PATH " could not be read"
                          : !strcmp(r, "dump_index_damaged") ? "its " DUMP_INDEX " (the dump tool's list of its files) is damaged"
                          : !strcmp(r, "dump_index_mismatch")
                              ? "its " DUMP_INDEX " (the dump tool's list of its files) does not match it (another game or version?)"
                          : !strcmp(r, "dump_index_packed")
                              ? "its files are in packs, which the dump tool's list (" DUMP_INDEX ") cannot check"
                              : "it has no list of its files (" MP_PLAYGO_TABLE_PATH ", or the dump tool's " DUMP_INDEX ")";
        snprintf(buf, cap, "not checked: %s", why);
    }
}

void mp_complete_extra_line(const mp_complete *c, char *buf, size_t cap) {
    char a[32], b[32];
    buf[0] = 0;
    if (c->checkable && from_dump_index(c)) {
        int at = 0;
        if (c->extra_count)
            at = snprintf(buf, cap, "%llu %s not in the dump tool's list (%s)", (unsigned long long)c->extra_count,
                          c->extra_count == 1 ? "file is" : "files are", bytes_text(c->extra_bytes, a));
        if (c->leftover_count && at >= 0 && (size_t)at < cap)
            snprintf(buf + at, cap - (size_t)at, "%s%llu %s (%s) %s what dump tools leave behind, which this list does not prove %s not the game's",
                     at ? "; " : "", (unsigned long long)c->leftover_count, c->leftover_count == 1 ? "file" : "files",
                     bytes_text(c->leftover_bytes, b), c->leftover_count == 1 ? "looks like" : "look like",
                     c->leftover_count == 1 ? "is" : "are");
        return;
    }
    if (c->checkable && c->extra_count && c->leftover_count)
        snprintf(buf, cap, "%llu files are not in the original package (%s); %s of them is what dump tools leave behind",
                 (unsigned long long)c->extra_count, bytes_text(c->extra_bytes, a), bytes_text(c->leftover_bytes, b));
    else if (c->checkable && c->extra_count)
        snprintf(buf, cap, "%llu %s not in the original package (%s)", (unsigned long long)c->extra_count,
                 c->extra_count == 1 ? "file is" : "files are", bytes_text(c->extra_bytes, a));
    else if (!c->checkable && c->leftover_count)
        snprintf(buf, cap, "%llu %s (%s) %s what dump tools leave behind, but with no list nothing proves %s not the game's",
                 (unsigned long long)c->leftover_count, c->leftover_count == 1 ? "file" : "files",
                 bytes_text(c->leftover_bytes, a), c->leftover_count == 1 ? "looks like" : "look like",
                 c->leftover_count == 1 ? "it is" : "they are");
}
