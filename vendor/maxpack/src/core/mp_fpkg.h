// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 debug packages (FPKG, ".pkg"), read only: the internals of the FPKG source. See
// docs/source-images.md. Ported from pearlxcore/ProsperoPkgTool (MIT; Containers/
// ProsperoPackageReader.cs, OuterPfsReader.cs, ProsperoPackageContent.cs, ProsperoNapsLayout.cs,
// ProsperoPs5InnerImageReader.cs, NapsDialectRegistry.cs, NapsIndexedLogicalMap.cs,
// NapsSparseFidxDialect.cs, ProsperoInnerMountStream.cs, ProsperoInnerPfsReader.cs, Crypto/
// ProsperoKeys.cs, Crypto/ProsperoEntryCipher.cs), with fthdgn-gh/Alembic's port of the same
// (GPL-3.0-or-later; src/core/fpkg.cpp, fpkg_inner.cpp) as the second reading.
//
//   FIH header (64 KiB) | outer PFS | CNT | SI
//
// The outer PFS is encrypted with AES-128-XTS (one unit per 64 KiB block) under keys that come only
// from the package's content ID and its 32-character passcode (fake packages use 32 '0'): no RSA or
// other key is needed. It holds pfs_image.dat, a stream of stored or Kraken-compressed blocks, and
// naps_pkg_layout.dat, the map of those blocks onto the "mount": an uncompressed PFS of 64 KiB blocks
// whose uroot folder is the game. The CNT keeps the sce_sys files a dump has (param.json, the
// images, ...), some AES-128-CBC encrypted.
#ifndef MP_FPKG_H
#define MP_FPKG_H

#include <stddef.h>
#include <stdint.h>

#include "mp_crypto.h"
#include "mp_image.h"

#define MP_FPKG_BLOCK 0x10000u
#define MP_FPKG_PASSCODE_LEN 32
// What mp_fpkg_open accepts (or-ed): a game's package (CNT content type 0x20), additional content (0x21).
#define MP_FPKG_GAME 1
#define MP_FPKG_DLC 2

// ---- the package and its outer PFS (mp_fpkg.c) ---------------------------------------------

typedef struct {
    uint32_t id, name_off, flags1, flags2, data_off, data_size;
    uint8_t row[32];   // the table row as stored (an encrypted entry's key comes from it)
    char *name;        // from the names table (NULL if it has none)
} mp_cnt_entry;

typedef struct mp_fpkg {
    mp_image *file;          // the package (owned)
    uint64_t pfs_off, pfs_size, cnt_off;
    uint64_t inner_meta_base;   // FIH 0x50: where the inner PFS's superblock is in the mount, in 64 KiB blocks
    char content_id[37];
    uint32_t content_type;
    int accept;              // what mp_fpkg_open was told to take: MP_FPKG_GAME, MP_FPKG_DLC or both
    char passcode[MP_FPKG_PASSCODE_LEN + 1];
    int default_passcode;    // the passcode is the default (32 '0')
    mp_cnt_entry *entries;
    size_t nentries;
    // the outer PFS
    uint32_t nblocks, sb;    // 64 KiB blocks, and the superblock's
    int plain;               // stored without encryption (the "PPRPLAIN-NOAUTH!" seed)
    int icv_ok;              // the superblock's SHA3-256 checksum matches
    mp_aes128 data_key, tweak_key;
    uint8_t *naps;           // naps_pkg_layout.dat
    size_t naps_len;
    int64_t mtime;           // of uroot in the outer PFS (the time it was built)
} mp_fpkg;

// Reads the header, the CNT and the outer PFS of the package in img (takes img; closed with the
// package, or on failure), decrypting with passcode (NULL or "" = the default). Refuses retail
// packages, patches, PS4 packages, a content type accept does not take (MP_FPKG_GAME, MP_FPKG_DLC)
// and a passcode that does not open it, each with its message.
int mp_fpkg_open(mp_image *img, const char *passcode, int accept, mp_fpkg **out, mp_err *e);
void mp_fpkg_close(mp_fpkg *p);   // p may be NULL
// 1 if passcode is one a package can have: 32 printable ASCII characters, no spaces.
int mp_fpkg_passcode_ok(const char *passcode);
// pfs_image.dat decrypted, as an image of its own (it borrows p: close it first).
int mp_fpkg_inner_image(mp_fpkg *p, mp_image **out, mp_err *e);
// The plaintext of entry i, read whole (decrypted if it is protected); *len = its data size.
int mp_fpkg_entry(mp_fpkg *p, size_t i, uint8_t **out, size_t *len, mp_err *e);
// The sce_sys path of entry i ("param.json", "trophy2/trophy00.ucp", ...), or NULL if it is not a
// file of the game (the digests, keys, tables...).
const char *mp_fpkg_entry_path(const mp_fpkg *p, size_t i);

// ---- the block map and the mount (mp_naps.c) -----------------------------------------------

typedef struct {
    uint64_t coff;        // where its bytes start in pfs_image.dat
    uint64_t off;         // where it is in the mount
    uint32_t clen;        // its bytes in pfs_image.dat (0 for an alias)
    uint32_t even_len;    // the first sub-chunk's compressed size
    uint32_t len;         // decoded (at most 256 KiB)
    uint8_t kraken, zero, alias;
    int kflags;           // Kraken flags (mp_kraken.h)
    int64_t source;       // an alias: the block it repeats (-1: zeros)
} mp_naps_block;

typedef struct {
    mp_naps_block *b;
    size_t n;
    uint64_t mount_size;   // 64 KiB-aligned
    const char *dialect;
    int kraken;            // some block is Kraken
} mp_naps_plan;

// Called with a candidate plan when the readings of the map disagree: 0 if it gives a readable inner
// PFS.
typedef int (*mp_naps_probe)(void *ctx, const mp_naps_plan *plan, mp_err *e);

// The plan of naps_pkg_layout.dat over an image of image_len bytes.
int mp_naps_plan_of(const uint8_t *naps, size_t len, uint64_t image_len, mp_naps_probe probe, void *ctx,
                    mp_naps_plan *out, mp_err *e);
void mp_naps_plan_free(mp_naps_plan *p);
// The mount: the plan over inner (an image of pfs_image.dat) as a random-access image, decoding blocks
// as they are read (safe across threads). own: it takes inner and the plan's blocks (also on
// failure); else it borrows them (they must outlive it).
int mp_naps_mount(mp_image *inner, mp_naps_plan *plan, int own, const char *name, mp_image **out, mp_err *e);

#endif
