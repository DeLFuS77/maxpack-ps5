// SPDX-License-Identifier: GPL-3.0-or-later
// .p5z: the game packed into a single file to keep on the PC (design 6.4).
// Full format in docs/p5z-format.md.
#ifndef MP_P5Z_H
#define MP_P5Z_H

#include <stdint.h>
#include <stdio.h>

#include "mp_pack.h"

#define P5Z_HEADER 64u
#define P5Z_FOOTER 32u
#define P5Z_INDEX_HEADER 56u
#define P5Z_FILE_REC 88u
#define P5Z_FRAME_REC 16u
#define P5Z_MANIFEST_REC 64u
#define P5Z_STORE_RAW 0
#define P5Z_STORE_FRAMES 1
#define P5Z_NO_FILE 0xFFFFFFFFu

// .p5z output (the file must not exist). threads: threads to compress the loose files.
int mp_pack_out_p5z(const char *path, unsigned threads, mp_pack_out *out, mp_err *e);
// Same, with a cap: if the .p5z is going to go over max_size bytes, the write fails with the message
// "the .p5z would go over N bytes" and what was written is deleted (0 = no cap).
int mp_pack_out_p5z_ex(const char *path, unsigned threads, uint64_t max_size, mp_pack_out *out, mp_err *e);
// 1 if the .p5z output failed because of the cap (and not something else), 0 if not; ask before destroy.
int mp_pack_out_p5z_too_big(const mp_pack_out *out);

// ---- Reading (Task 7) -------------------------------------------------------------------

typedef struct mp_p5z mp_p5z;
int mp_p5z_open(const char *path, mp_p5z **out, mp_err *e);
// mp_p5z_open with flags: MP_P5Z_OPEN_V1 refuses a .p5z that grew (index version 2), for a reader that
// takes only what a fresh pack writes (the installer, which leaves the packs as a folder).
#define MP_P5Z_OPEN_V1 1
int mp_p5z_open_ex(const char *path, int flags, mp_p5z **out, mp_err *e);
// 1 for a .p5z as packed; 2 for one that grew by appending (maxpack add or update), whose packs follow
// ampr-emu-ng's p5z rules (mp_zpk_open_ex).
uint32_t mp_p5z_version(const mp_p5z *p);
uint32_t mp_p5z_grown(const mp_p5z *p);   // how many times it grew (0 for version 1)

// An append to a .p5z writes first, next to it, <name>.p5z.maxpack-append (the journal): the size the
// .p5z had and its footer then. mp_p5z_recover, which every command that opens a .p5z runs first, takes
// a .p5z with a journal back to that size, exactly as it was, and deletes the journal: NONE (no journal),
// RESTORED, BUSY (another MaxPack is appending to it now: nothing is touched) or STALE (a journal that is
// damaged or not of this file as it is: it goes, the .p5z is not touched). -1 only when the .p5z could not
// be cut back (the journal stays for the next time). Meanwhile mp_p5z_open reads a .p5z with a journal
// that matches it as it was before the append.
#define MP_P5Z_JOURNAL ".maxpack-append"
enum { MP_RECOVER_NONE = 0, MP_RECOVER_RESTORED, MP_RECOVER_BUSY, MP_RECOVER_STALE };
int mp_p5z_recover(const char *p5z, int *what, mp_err *e);
const mp_pack_manifest *mp_p5z_manifest(const mp_p5z *p);
int mp_p5z_info(const mp_p5z *p, FILE *out);
int mp_p5z_zpk(mp_p5z *p, mp_zpk **out, mp_err *e);
uint64_t mp_p5z_volume_offset(const mp_p5z *p, uint32_t vol);   // where the volume starts in the .p5z
// The bytes of the .p5z that nothing uses any more: the indexes and footers an append replaced, the
// regions of the files an update changed or removed, the room in the volumes no chunk uses. 0 for a .p5z
// that never grew; maxpack compact makes a copy without them.
int mp_p5z_unused(mp_p5z *p, uint64_t *unused, mp_err *e);
// The files of the dump the .p5z was made of (its manifest) that take(ctx, path) takes (all with NULL), as a
// source: each one read from the .p5z. p must stay open while the source is.
int mp_p5z_source(mp_p5z *p, int (*take)(void *ctx, const char *path), void *ctx, mp_source **out, mp_err *e);
int mp_unpack_folder(mp_p5z *p, const char *out_dir, unsigned threads, int progress, mp_err *e);
// The same with volumes of at most volume_limit bytes (MP_ZPK_VOLUME_LIMIT, smaller in tests) for a .p5z that
// grew: its volumes are laid out again as pack --folder lays them out; one that did not is copied as it is.
int mp_unpack_folder_ex(mp_p5z *p, const char *out_dir, uint64_t volume_limit, unsigned threads, int progress,
                        mp_err *e);
int mp_unpack_original(mp_p5z *p, const char *out_dir, unsigned threads, int progress, mp_err *e);
void mp_p5z_close(mp_p5z *p);

#endif
