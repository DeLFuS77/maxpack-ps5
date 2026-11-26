// SPDX-License-Identifier: GPL-3.0-or-later
// Growing a .p5z without packing it again: a DLC that comes out later (add), the files a new version
// of the game changes (update), and a copy without the room those leave (compact). A .p5z grows only by
// appending: new regions, then a new index (version 2) and a new footer; nothing before its end is
// written again, and the journal of mp_p5z.h undoes an append that stops. Format: docs/p5z-format.md.
#ifndef MP_GROW_H
#define MP_GROW_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"
#include "mp_source.h"

typedef struct {
    unsigned threads;
    int progress;
    uint64_t volume_limit;          // of the new volumes (MP_ZPK_VOLUME_LIMIT; smaller in tests)
    uint64_t max_size;              // the .p5z may not go over it (0: no cap), as pack --max-file-size
    const char *const *keep_loose;  // also leave these loose (new or changed files only)
    size_t nkeep;
    int leave_out_leftovers;        // as pack --leave-out-leftovers, for the new dump
    int force;                      // update: even when more than half of the game changed
    uint32_t max_volumes;           // the most volumes the .p5z may end with (0: 256, what maxpack.image takes)
} mp_grow_opts;

void mp_grow_opts_default(mp_grow_opts *o);

typedef struct {
    // the new dump against the .p5z's manifest (by path, size and SHA-256)
    uint64_t same_files, new_files, changed_files, removed_files;
    uint64_t stored_files, stored_bytes;   // the new and changed ones: what went into the .p5z
    uint64_t total_bytes;                  // of the new dump
    uint64_t added_bytes;                  // what the .p5z grew by
    uint64_t p5z_size, volumes, volumes_added, unused_bytes;   // after
    uint32_t grown;                        // how many times it has grown, this one included
    int nothing;                           // nothing changed: nothing was written
    int par2;                              // it has repair data next to it (<name>.p5z.par2): no longer of it
    unsigned dlc_data, dlc_license;        // add: the DLC added
    uint64_t dlc_bytes;
    // on failure, why as a code: "busy" (another MaxPack is changing it), "no_space", "too_big"
    // (max_size), "too_many_volumes", "mostly_changed", "already_packed" (the dump has packs), "no_packs_left"
    // (the new dump leaves the packs empty), "other_game", "same_version", "no_title_id", "dlc_has_ini",
    // "dlc_too_many", "dlc_refused", "changed_meanwhile"; NULL for any other (an I/O error)
    const char *code;
    char title_id[16], old_version[16], new_version[16];
} mp_grow_result;

// For a file of the new dump with the path and size of one of the manifest: 1 with its SHA-256 in sha
// when the caller knows it without reading it (a file taken from the .p5z itself), else 0.
typedef int (*mp_grow_known)(void *ctx, const char *path, uint64_t size, uint8_t sha[32]);

// The core of add and update: the .p5z at p5z becomes dump. A file of dump with the path, size and
// SHA-256 of one of the manifest stays where it is (only its date changes, if it did); the rest is
// stored as pack would store it, at the end, with new volumes after the old ones; what dump does not
// have leaves the indexes. Takes no ownership of dump.
int mp_grow_run(const char *p5z, mp_source *dump, mp_grow_known known, void *kctx, const mp_grow_opts *o,
                mp_grow_result *r, mp_err *e);

// add: the DLC at dlc[0..n) (packages with mp_source_set_passcode's passcode, folders or images) go into the
// .p5z as pack --dlc would have put them: each one with files of its own in the addcontN/ after the last
// the game has, and dlc_emu.ini written again with the DLC it had and the new ones. A dlc_emu.ini that
// MaxPack did not write stops it ("dlc_has_ini"), as pack --dlc stops; so does a DLC the game has already.
int mp_grow_add(const char *p5z, const char *const *dlc, size_t n, const mp_grow_opts *o, mp_grow_result *r,
                mp_err *e);

// update: the .p5z becomes the new dump at dump (a folder, an image or a package), which must be of the same
// game (the title ID of its param.json, "other_game"; none, "no_title_id") in another version
// ("same_version"). Only its new and changed files are stored; what it no longer has leaves the .p5z's
// indexes. The DLC the .p5z carries (its dlc_emu.ini written by MaxPack and the addcontN/ it lists) stay,
// unless the new dump has a dlc_emu.ini of its own; files of the dump where they are stop it ("dlc_refused").
// More than half of the game's bytes to store stops it ("mostly_changed") unless o->force.
int mp_grow_update(const char *p5z, const char *dump, const mp_grow_opts *o, mp_grow_result *r, mp_err *e);

// compact: a new .p5z at out (which must not exist) with what in uses and nothing else: the room an update
// left goes. Nothing is compressed again: the stored records are copied as they are and the chunks of the
// packs go to new volumes of at most volume_limit bytes, back to back. It is always of version 1, the .p5z a
// fresh pack of the same dump writes (with that volume limit). *out_size gets its size.
int mp_compact(const char *in, const char *out, unsigned threads, uint64_t volume_limit, int progress, uint64_t *out_size,
               mp_err *e);

// Tests only: called at every step of an append; 0 goes on, 1 fails there as an error would (the .p5z is
// taken back), 2 stops there as a power cut would (nothing is undone, the journal stays). Nothing but the
// tests sets it.
typedef int (*mp_grow_fault_fn)(const char *step);
void mp_grow_set_fault(mp_grow_fault_fn fn);

#endif
