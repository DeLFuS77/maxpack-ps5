// SPDX-License-Identifier: GPL-3.0-or-later
// Description of a game (folder, image or .p5z) for a front end: ID, content ID, version,
// name, sizes, its packs, whether it carries a runtime that reads zstd, whether it is a DLC and the
// DLC it carries. It is what `maxpack info --json` shows.
#ifndef MP_DESCRIBE_H
#define MP_DESCRIBE_H

#include <stddef.h>
#include <stdint.h>

#include "mp_complete.h"
#include "mp_dlc.h"
#include "mp_err.h"
#include "mp_p5z.h"

typedef struct {
    char kind[8];   // "p5z", "folder", "exfat", "ufs2" (.ffpkg), "pfs" (.ffpfs), "pfsc" (.ffpfsc)
    char inner[8];  // for "pfsc", the image inside that holds the game ("exfat", "ufs2", "pfs"), else ""
    int compressed; // an image whose reading decompresses PFSC blocks
    char title_id[16], version[16], name[128];
    uint64_t original_size;   // sum of the original dump's files
    uint64_t install_size;    // what it takes up when installed (.p5z: what unpack --folder leaves)
    uint64_t p5z_size;        // size of the .p5z (0 if it is not one)
    uint64_t files;           // files of the original dump
    uint64_t packed_files;    // of those, the ones in the zstd packs (0 if it is not packed)
    uint64_t loose_bytes;     // .p5z: what the console extracts when playing it (roles 0, 2 and 3); the rest, 0
    uint64_t volumes;         // volumes of the packs, ampr_zstd-NNN.pak (0 if it is not packed)
    int has_runtime, has_zstd_runtime, packed;   // packed: already carries ampr_zstd.index
    char content_id[48];      // contentId of param.json ("EP0001-PPSA10001_00-..."), "" if it gives none
    // additional content, not a game: a package of it, or a folder or image with sce_sys/param.json and
    // no eboot.bin (its title_id is then its content ID's, the one pack --dlc checks); dlc_data, one
    // with files of its own outside sce_sys/ (else a license only)
    int dlc, dlc_data;
    // .p5z: how many times it grew (maxpack add or update; 0 for one as packed) and the bytes it does not use
    // (mp_p5z_unused)
    uint32_t grown;
    uint64_t unused_bytes;
} mp_describe;

// 0 if it could describe it; -1 with the reason in e.
int mp_describe_path(const char *path, mp_describe *d, mp_err *e);
// The same and, when c is not NULL, whether the game is complete against the list of the original
// package's files it carries (mp_complete.h); the caller frees c with mp_complete_free. A .p5z checks
// its manifest (the files of the dump it was made of), a compressed folder its loose files and the
// ones in its packs.
int mp_describe_path_ex(const char *path, mp_describe *d, mp_complete *c, mp_err *e);
// The same and, when dlc is not NULL, the DLC the game carries: the ones its own dlc_emu.ini lists
// (mp_dlc_parse; none when it has none or it cannot be read), each with what its folder holds (a
// compressed folder: its loose files and the ones in its packs). The caller frees it with
// mp_dlc_list_free, also when this fails.
int mp_describe_full(const char *path, mp_describe *d, mp_complete *c, mp_dlc_list *dlc, mp_err *e);
// Only whether it is complete (what verify says after it checks).
int mp_describe_complete(const char *path, mp_complete *c, mp_err *e);
// Whether the dump a .p5z was made of is complete, from the table the .p5z carries.
int mp_complete_p5z(mp_p5z *p, mp_complete *c, mp_err *e);
// sce_sys/icon0.png: 0 with the bytes in *png (allocated with malloc; the caller frees them);
// -1 if it has no icon or it could not be read (e says why).
int mp_describe_icon(const char *path, uint8_t **png, size_t *len, mp_err *e);

#endif
