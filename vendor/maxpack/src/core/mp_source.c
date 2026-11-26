// SPDX-License-Identifier: GPL-3.0-or-later
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_fpkg.h"
#include "mp_platform.h"
#include "mp_source_impl.h"

#define PFS_MAGIC 20130315ull
#define UFS1_MAGIC 0x011954u
#define UFS2_MAGIC 0x19540119u
#define UFS2_MAGIC_BE 0x19015419u
#define UFS_MAGIC_AT 1372   // fs_magic inside the superblock

static int cmp_entry(const void *a, const void *b) {
    return strcmp(((const mp_entry *)a)->path, ((const mp_entry *)b)->path);
}

void mp_source_sort(mp_source *s) {
    if (s->n > 1) qsort(s->entries, s->n, sizeof *s->entries, cmp_entry);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

int mp_image_probe(mp_image *img, mp_image_type *type, mp_err *e) {
    *type = MP_IMG_UNKNOWN;
    uint8_t h[16] = {0};
    size_t got = 0;
    if (mp_image_pread(img, h, sizeof h, 0, &got, e) != 0) return -1;
    if (got >= 11 && memcmp(h + 3, "EXFAT   ", 8) == 0) {
        *type = MP_IMG_EXFAT;
        return 0;
    }
    if (got >= 16 && rd64(h + 8) == PFS_MAGIC) {
        *type = MP_IMG_PFS;
        return 0;
    }
    if (got >= 4 && (memcmp(h, "\x7F" "FIH", 4) == 0 || memcmp(h, "\x7F" "CNT", 4) == 0 ||
                     memcmp(h, "\x7F" "LIH", 4) == 0)) {
        *type = MP_IMG_PKG;
        return 0;
    }
    // The places where FreeBSD looks for a superblock, in its order (SBLOCKSEARCH).
    static const uint64_t at[] = {65536, 8192, 0, 262144};
    for (size_t k = 0; k < sizeof at / sizeof at[0]; k++) {
        uint8_t m[4];
        if (mp_image_pread(img, m, 4, at[k] + UFS_MAGIC_AT, &got, e) != 0) return -1;
        if (got != 4) continue;
        uint32_t v = rd32(m);
        if (v == UFS2_MAGIC) *type = MP_IMG_UFS2;
        else if (v == UFS2_MAGIC_BE) *type = MP_IMG_UFS2_BE;
        else if (v == UFS1_MAGIC) *type = MP_IMG_UFS1;
        if (*type) return 0;
    }
    return 0;
}

static int ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

// The passcode for packages (mp_source_set_passcode); "" = the default.
static char g_passcode[MP_FPKG_PASSCODE_LEN + 1];

int mp_source_set_passcode(const char *passcode) {
    if (passcode && passcode[0] && !mp_fpkg_passcode_ok(passcode)) return -1;
    snprintf(g_passcode, sizeof g_passcode, "%s", passcode ? passcode : "");
    return 0;
}

static int open_image(mp_image *img, int depth, const char *ext, const char *passcode, int what, mp_source **out,
                      mp_err *e);

int mp_source_open_image(mp_image *img, int depth, const char *ext, mp_source **out, mp_err *e) {
    return open_image(img, depth, ext, g_passcode, MP_OPEN_GAME, out, e);
}

// what: MP_OPEN_GAME, MP_OPEN_DLC or MP_OPEN_ANY, which are mp_fpkg_open's MP_FPKG_GAME, MP_FPKG_DLC and both
static int open_image(mp_image *img, int depth, const char *ext, const char *passcode, int what, mp_source **out,
                      mp_err *e) {
    *out = NULL;
    if (depth > MP_IMAGE_MAX_DEPTH) {
        mp_err_set(e, "%s: images inside images more than %d deep", img->name, MP_IMAGE_MAX_DEPTH);
        mp_image_close(img);
        return -1;
    }
    mp_image_type t = MP_IMG_UNKNOWN;
    if (mp_image_probe(img, &t, e) != 0) {
        mp_image_close(img);
        return -1;
    }
    switch (t) {
    case MP_IMG_EXFAT: return mp_exfat_open_image(img, out, e);
    case MP_IMG_UFS2: return mp_ufs2_open_image(img, out, e);
    case MP_IMG_PFS: return mp_pfs_open_image(img, depth, out, e);
    case MP_IMG_UFS1:
        mp_err_set(e, "%s: a UFS1 image; MaxPack reads UFS2 images (newfs -O 2), as ShadowMountPlus uses",
                   img->name);
        break;
    case MP_IMG_UFS2_BE:
        mp_err_set(e, "%s: a big-endian UFS2 image; the PS5's are little-endian", img->name);
        break;
    case MP_IMG_PKG:
        if (depth == 1) return mp_fpkg_open_source(img, passcode, what, out, e);
        mp_err_set(e, "%s: a package inside an image: MaxPack reads a package (.pkg) as a file of its own", img->name);
        break;
    default:
        if (ext && ieq(ext, ".pkg"))
            mp_err_set(e, "%s: not a PS5 package (it does not start with an FIH header)", img->name);
        else if (ext && ieq(ext, ".ffpkg"))
            mp_err_set(e, "%s: not a UFS2 image (there is no UFS2 superblock in it)", img->name);
        else if (ext && (ieq(ext, ".ffpfs") || ieq(ext, ".ffpfsc")))
            mp_err_set(e, "%s: not a PFS image (it does not start with a PFS header)", img->name);
        else if (ext && ieq(ext, ".exfat"))
            mp_err_set(e, "%s: not an exFAT image (it does not start with an exFAT boot sector)", img->name);
        else
            mp_err_set(e, "%s: unrecognised format: MaxPack packs a folder, an .exfat, .ffpkg, .ffpfs or "
                          ".ffpfsc image or a PS5 package (.pkg)", img->name);
        break;
    }
    mp_image_close(img);
    return -1;
}

int mp_source_open(const char *path, mp_source **out, mp_err *e) {
    return mp_source_open_passcode(path, g_passcode, out, e);
}

static int open_path(const char *path, const char *passcode, int what, mp_source **out, mp_err *e) {
    *out = NULL;
    int kind = mp_path_kind(path);
    if (kind == 0) {
        mp_err_set(e, "does not exist: %s", path);
        return -1;
    }
    if (kind == 2) return mp_dir_open(path, out, e);
    mp_image *img = NULL;
    if (mp_image_open_file(path, &img, e) != 0) return -1;
    const char *slash = strrchr(path, '/'), *bslash = strrchr(path, '\\'), *dot = strrchr(path, '.');
    if (dot && ((slash && dot < slash) || (bslash && dot < bslash))) dot = NULL;
    return open_image(img, 1, dot, passcode, what, out, e);
}

int mp_source_open_passcode(const char *path, const char *passcode, mp_source **out, mp_err *e) {
    return open_path(path, passcode, MP_OPEN_GAME, out, e);
}

int mp_source_open_as(const char *path, int what, mp_source **out, mp_err *e) {
    what &= MP_OPEN_ANY;
    return open_path(path, g_passcode, what ? what : MP_OPEN_GAME, out, e);
}

int mp_source_is_dlc_package(const mp_source *s) { return s->dlc_package; }

mp_source_kind mp_source_kind_of(const mp_source *s) { return s->kind; }
size_t mp_source_count(const mp_source *s) { return s->n; }
const mp_entry *mp_source_entry(const mp_source *s, size_t i) { return i < s->n ? &s->entries[i] : NULL; }
size_t mp_source_dir_count(const mp_source *s) { return s->ndirs; }

static const char *kind_word(mp_source_kind k) {
    switch (k) {
    case MP_SRC_EXFAT: return "exfat";
    case MP_SRC_UFS2: return "ufs2";
    case MP_SRC_PFS: return "pfs";
    case MP_SRC_FPKG: return "fpkg";
    default: return "folder";
    }
}

const char *mp_source_kind_name(const mp_source *s) { return s->container ? "pfsc" : kind_word(s->kind); }

const char *mp_source_inner_name(const mp_source *s) { return s->container ? kind_word(s->inner) : ""; }

const char *mp_source_package_id(const mp_source *s) { return s->package_id ? s->package_id(s) : ""; }

const char *mp_source_kind_text(const mp_source *s, char *buf, size_t n) {
    if (s->kind == MP_SRC_FPKG) {
        snprintf(buf, n, "PS5 package (FPKG)%s", s->compressed ? ", compressed" : "");
        return buf;
    }
    static const char *words[] = {"folder", "exFAT image", "UFS2 image", "PFS image"};
    static const char *articles[] = {"a", "an", "a", "a"};
    mp_source_kind k = s->container ? s->inner : s->kind;
    size_t at = k >= MP_SRC_EXFAT && k <= MP_SRC_PFS ? (size_t)k - 1 : 0;
    const char *w = words[at];
    if (s->container)
        snprintf(buf, n, "PFS container with %s %s inside%s", articles[at], w, s->compressed ? ", compressed" : "");
    else if (s->compressed)
        snprintf(buf, n, "%s, compressed", w);
    else
        snprintf(buf, n, "%s", w);
    return buf;
}

int mp_source_compressed(const mp_source *s) { return s->compressed; }

// A package is decrypted (and, compressed, decoded) as it is read: like inflating, work for threads.
unsigned mp_source_readers(const mp_source *s) {
    if (s->readers) return s->readers;
    return s->compressed || s->kind == MP_SRC_FPKG ? 16 : 4;
}

long mp_source_find(const mp_source *s, const char *path) {
    size_t lo = 0, hi = s->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(s->entries[mid].path, path);
        if (c == 0) return (long)mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

int mp_source_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e) {
    if (i >= s->n) {
        mp_err_set(e, "file index out of range: %zu", i);
        return -1;
    }
    const mp_entry *en = &s->entries[i];
    if (off > en->size || len > en->size - off) {
        mp_err_set(e, "read outside %s (%" PRIu64 " + %zu > %" PRIu64 ")", en->path, off, len,
                   en->size);
        return -1;
    }
    if (len == 0) return 0;
    return s->read(s, i, off, buf, len, e);
}

void mp_source_close(mp_source *s) {
    if (!s) return;
    if (s->close) s->close(s);
    for (size_t i = 0; i < s->n; i++) free(s->entries[i].path);
    free(s->entries);
    free(s);
}

// ---- for the readers of UFS2 and PFS --------------------------------------------------------

// Length of the UTF-8 sequence at p (n bytes left), or 0 if it is not one (overlong forms,
// surrogates and code points past U+10FFFF are not).
static size_t utf8_len(const uint8_t *p, size_t n) {
    uint8_t c = p[0];
    if (c < 0x80) return 1;
    size_t k = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!k || k > n) return 0;
    for (size_t i = 1; i < k; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && p[1] < 0xA0) return 0;
    if (c == 0xED && p[1] > 0x9F) return 0;
    if (c == 0xF0 && p[1] < 0x90) return 0;
    if (c == 0xF4 && p[1] > 0x8F) return 0;
    return k;
}

char *mp_image_join(const char *prefix, const uint8_t *name, size_t len, mp_err *e) {
    const char *why = NULL;
    if (len == 0) why = "empty";
    else if (len > 255) why = "longer than 255 bytes";
    else if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) why = "\".\" or \"..\"";
    for (size_t i = 0; !why && i < len;) {
        if (name[i] == 0 || name[i] == '/' || name[i] == '\\') {
            why = "it has '/', '\\' or a NUL byte";
            break;
        }
        size_t k = utf8_len(name + i, len - i);
        if (!k) why = "it is not UTF-8";
        i += k ? k : 1;
    }
    if (why) {
        char shown[64];
        size_t m = 0;
        for (size_t i = 0; i < len && m + 4 < sizeof shown; i++)
            shown[m++] = name[i] >= 0x20 && name[i] < 0x7F && name[i] != '\\' ? (char)name[i] : '?';
        shown[m] = 0;
        mp_err_set(e, "invalid name in %s/: \"%s\" (%s)", prefix, shown, why);
        return NULL;
    }
    size_t a = strlen(prefix);
    char *p = malloc(a + len + 2);
    if (!p) {
        mp_err_set(e, "out of memory");
        return NULL;
    }
    memcpy(p, prefix, a);
    if (a) p[a++] = '/';
    memcpy(p + a, name, len);
    p[a + len] = 0;
    return p;
}

static int cmp_name(const void *a, const void *b) {
    const mp_image_name *x = a, *y = b;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->p, y->p, n);
    return c ? c : (x->len > y->len) - (x->len < y->len);
}

int mp_image_unique(mp_image_name *v, size_t n, const char *dir, mp_err *e) {
    if (n > 1) qsort(v, n, sizeof *v, cmp_name);
    for (size_t i = 1; i < n; i++)
        if (cmp_name(&v[i - 1], &v[i]) == 0) {
            char shown[64];
            size_t m = 0;
            for (size_t k = 0; k < v[i].len && m + 4 < sizeof shown; k++)
                shown[m++] = v[i].p[k] >= 0x20 && v[i].p[k] < 0x7F ? (char)v[i].p[k] : '?';
            shown[m] = 0;
            mp_err_set(e, "the name \"%s\" is twice in %s/", shown, dir);
            return -1;
        }
    return 0;
}

static int cmp_range(const void *a, const void *b) {
    const mp_image_range *x = a, *y = b;
    return (x->start > y->start) - (x->start < y->start);
}

int mp_image_disjoint(mp_image_range *v, size_t n, const char *img, mp_err *e) {
    if (n > 1) qsort(v, n, sizeof *v, cmp_range);
    uint64_t end = 0;
    int any = 0;
    for (size_t i = 0; i < n; i++) {
        if (!v[i].len) continue;
        if (any && v[i].start < end) {
            mp_err_set(e, "damaged %s image: two files or folders use the same blocks", img);
            return -1;
        }
        end = v[i].start + v[i].len;
        any = 1;
    }
    return 0;
}
