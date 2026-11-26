// SPDX-License-Identifier: GPL-3.0-or-later
// Internals of mp_source: shared by the different kinds of source.
#ifndef MP_SOURCE_IMPL_H
#define MP_SOURCE_IMPL_H

#include "mp_image.h"
#include "mp_source.h"

// Images inside one another, the outer one included (a PFS image holding a PFS image holding an
// exFAT image is 3).
#define MP_IMAGE_MAX_DEPTH 3
// Folders inside one another, and entries (files and folders) of a whole image.
#define MP_IMAGE_MAX_LEVELS 64
#define MP_IMAGE_MAX_ENTRIES (1u << 22)

struct mp_source {
    mp_source_kind kind;    // of the file opened
    mp_source_kind inner;   // of the image whose files these are (0 = kind)
    int container;          // kind is a PFS image that holds the image `inner`
    int compressed;         // reads decompress PFSC blocks
    int dlc_package;        // a package of additional content (CNT content type 0x21)
    unsigned readers;       // reading threads it can keep busy (0: by its kind, see mp_source_readers)
    const char *(*package_id)(const mp_source *s);   // its package's content ID (NULL: it is no package)
    mp_entry *entries;
    size_t n, ndirs;
    int (*read)(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e);
    void (*close)(mp_source *s);   // frees impl
    void *impl;
};

int mp_dir_open(const char *path, mp_source **out, mp_err *e);
int mp_exfat_open(const char *path, mp_source **out, mp_err *e);
// The readers of images take img (it is closed with the source, or on failure).
int mp_exfat_open_image(mp_image *img, mp_source **out, mp_err *e);
int mp_ufs2_open_image(mp_image *img, mp_source **out, mp_err *e);
// depth: images around it, itself included (1 for a file on disk).
int mp_pfs_open_image(mp_image *img, int depth, mp_source **out, mp_err *e);
// A PS5 debug package (mp_source_fpkg.c); passcode NULL or "" = the default; accept: MP_FPKG_GAME,
// MP_FPKG_DLC or both (mp_fpkg.h).
int mp_fpkg_open_source(mp_image *img, const char *passcode, int accept, mp_source **out, mp_err *e);

// What an image is, by its signature.
typedef enum {
    MP_IMG_UNKNOWN = 0, MP_IMG_EXFAT, MP_IMG_UFS2, MP_IMG_PFS, MP_IMG_UFS1, MP_IMG_UFS2_BE, MP_IMG_PKG
} mp_image_type;
int mp_image_probe(mp_image *img, mp_image_type *type, mp_err *e);
// Opens the image by its signature (takes img). depth: as for mp_pfs_open_image. ext: the file's
// extension or NULL, only to word the message when the signature is not one MaxPack reads. A package
// is only read as a file on disk (depth 1), with mp_source_set_passcode's passcode, as a game's.
int mp_source_open_image(mp_image *img, int depth, const char *ext, mp_source **out, mp_err *e);

// Sorts the entries by strcmp of the path.
void mp_source_sort(mp_source *s);

// ---- for the readers of UFS2 and PFS --------------------------------------------------------

// Checks a name read from an image (len bytes, not NUL-terminated) and joins it to prefix: NULL
// with e set if it is empty, ".", "..", has '/', '\' or NUL, is not UTF-8, or is longer than 255.
char *mp_image_join(const char *prefix, const uint8_t *name, size_t len, mp_err *e);

// The names of one folder, to find two that are the same.
typedef struct {
    const uint8_t *p;
    size_t len;
} mp_image_name;
// 0, or -1 with e set if two are equal (sorts v).
int mp_image_unique(mp_image_name *v, size_t n, const char *dir, mp_err *e);

// Ranges [start, start + len) of an image used by its files and metadata: two that overlap mean a
// damaged or hostile image (cross-linked blocks).
typedef struct {
    uint64_t start, len;
} mp_image_range;
// 0, or -1 with e set (what: "UFS2", "PFS") if two overlap (sorts v; ranges of length 0 are ignored).
int mp_image_disjoint(mp_image_range *v, size_t n, const char *img, mp_err *e);

#endif
