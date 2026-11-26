// SPDX-License-Identifier: GPL-3.0-or-later
// The bytes an image reader parses, as a random-access read-only run: a file on disk, a range of
// another image (a file stored whole inside a PFS image) or a PFSC-compressed file inside one, seen
// decompressed. The exFAT, UFS2 and PFS readers take one, so an image can be read from inside
// another (an .ffpfsc is a PFS image holding an exFAT, UFS2 or PFS image). Reads are safe across
// threads.
#ifndef MP_IMAGE_H
#define MP_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

typedef struct mp_image mp_image;
struct mp_image {
    uint64_t size;
    char *name;       // for messages: the path, or "<path> (the file inside)"
    int compressed;   // reading it decompresses PFSC blocks (here or in an image around it)
    // Exactly len bytes at off; the caller keeps off + len <= size.
    int (*read)(mp_image *m, void *buf, size_t len, uint64_t off, mp_err *e);
    void (*close)(mp_image *m);   // frees impl (and the images it was taken from)
    void *impl;
};

// The file at path.
int mp_image_open_file(const char *path, mp_image **out, mp_err *e);
// Like mp_file_pread: reads [off, off + len) cut at the end of the image (*got < len only there).
int mp_image_pread(mp_image *m, void *buf, size_t len, uint64_t off, size_t *got, mp_err *e);
// Bytes [off, off + len) of parent as an image of their own. Takes parent (also on failure).
int mp_image_slice(mp_image *parent, uint64_t off, uint64_t len, const char *name, mp_image **out,
                   mp_err *e);
// The PFSC stream stored at [off, off + stored) of parent, decompressed: an image of `size` bytes.
// Takes parent (also on failure).
int mp_image_pfsc(mp_image *parent, uint64_t off, uint64_t stored, uint64_t size, const char *name,
                  mp_image **out, mp_err *e);
void mp_image_close(mp_image *m);   // m may be NULL

// ---- PFSC -----------------------------------------------------------------------------------
// The compression of PFS files (PS4/PS5 and MkPFS): a header, a table of where each block of
// (usually) 64 KiB starts, and the blocks, each stored as is (exactly a block long) or deflated
// with zlib (shorter). See docs/source-images.md.

#define MP_PFSC_HEADER 0x30
#define MP_PFSC_MAX_LOGICAL (2ull << 40)   // 2 TiB: a larger file is refused as absurd

typedef struct mp_pfsc mp_pfsc;
// Reads and checks the header and the whole block table of the stream at [off, off + stored) of
// img, for a file of `size` bytes; `what` names the file in messages. img stays the caller's and
// must outlive the decoder. cache_blocks: decompressed blocks kept for reads that take part of a
// block (0 = none).
int mp_pfsc_open(mp_image *img, uint64_t off, uint64_t stored, uint64_t size, const char *what,
                 unsigned cache_blocks, mp_pfsc **out, mp_err *e);
// Exactly len bytes at off (off + len <= size), decompressing the blocks they fall in.
int mp_pfsc_read(mp_pfsc *p, void *buf, size_t len, uint64_t off, mp_err *e);
void mp_pfsc_free(mp_pfsc *p);   // p may be NULL
// The stream of a decoder already open on parent as an image; takes both (also on failure).
int mp_image_pfsc_from(mp_image *parent, mp_pfsc *dec, const char *name, mp_image **out, mp_err *e);

#endif
