// SPDX-License-Identifier: GPL-3.0-or-later
// A .p5z's sce_sys/icon0.png for the file manager thumbnails. Self-contained (zstd decompression and
// zlib's crc32 only), so the Windows thumbnail provider can carry it: it reads the header, the footer,
// the index and the icon's region, never the rest of the game, and refuses anything over its caps
// before allocating it.
#ifndef MP_P5Z_ICON_H
#define MP_P5Z_ICON_H

#include <stddef.h>
#include <stdint.h>

// Reads n bytes at off into buf: 0 when all of them were read, -1 if fewer or on error.
typedef int (*mp_icon_read_at)(void *ctx, void *buf, size_t n, uint64_t off);
typedef struct {
    mp_icon_read_at read_at;
    uint64_t size;   // size of the .p5z
    void *ctx;
} mp_icon_src;

// Caps: a .p5z over any of them gives no icon.
#define MP_ICON_INDEX_MAX (64u << 20)     // compressed index
#define MP_ICON_UINDEX_MAX (128u << 20)   // uncompressed index
#define MP_ICON_MAX (8u << 20)            // the icon (in at most 64 frames, each no larger than zstd makes it)

// 0: *png (malloc'd; the caller frees it) and *len set; 1: the .p5z has no icon; -1: not a readable
// .p5z, or over a cap (err says why). err may be NULL.
int mp_p5z_icon_read(const mp_icon_src *src, uint8_t **png, size_t *len, char *err, size_t errlen);

#endif
