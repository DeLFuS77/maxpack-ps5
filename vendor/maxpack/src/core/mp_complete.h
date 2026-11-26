// SPDX-License-Identifier: GPL-3.0-or-later
// Whether a dump is complete: its files against the list of the files of the original package's
// image (sce_sys/playgo-hash-table.dat, see mp_playgo.h). The list has hashes, not names: what is
// missing is a count. A file of the dump that the list does not have is extra; of those, the ones
// dump tools are known to leave behind are leftovers.
//
// The rule, for each file of the dump:
//   - listed: the table has its hash (case does not matter);
//   - else ignored: OS junk (Thumbs.db, .DS_Store, ._*, __MACOSX/...), anything in sce_sys/,
//     where a dump puts the package's own entries (param.json, the PlayGo tables...), which are not
//     files of the image, and what MaxPack adds for the DLC it packs with a game (dlc_emu.ini and the
//     addcont0/ to addcont99/ folders, mp_complete_is_dlc);
//   - else extra; a leftover when dump tools leave it: a decrypted/ folder at the root (the raw ELF
//     copies of the executables), *.auth_info (written next to the executables) and
//     entitlements.txt or entitlement_key.dat at the root.
// sce_sys/about/right.sprx is not counted, listed or not: package builders add their own.
// Without a usable table nothing is checked, and the leftovers are only the files that match those
// names: nothing proves they are not the game's.
//
// When the dump has no usable table, the list the dump tool wrote serves instead: ampr_emu.index at
// its root (AMPRIDX3: every file it wrote, with its size). It proves less: that the copy has every
// file the dump tool wrote, with the sizes it wrote them (it finds copies cut short or damaged), not
// that the dump tool got every file of the game; the source says which list it was. Against it:
//   - listed: each row of the list (case does not matter, as for the runtime), but what a fine dump
//     differs in, which is not counted: info and checksum files that are not the game's (*.nfo, *.sfv, *.diz), the emulators the user
//     changes (fakelib/, fakelib2/, dlc_emu.ini), what the game writes while it runs (anything under a
//     Saved/ folder, *.log), the runtime's own files (ampr_commands.bin, apr_emu.log) and the list
//     itself; present: there with its size; missing: not there; size_mismatch: there, of another size;
//   - extra: what the dump has that the list does not (OS junk and what MaxPack adds for the DLC aside);
//   - leftovers: by their names only, as without a table: this list does not prove they are not the
//     game's (the dump tool writes them too), so pack never leaves them out because of it.
// A list that cannot be trusted is not used ("not checked"): a damaged one ("dump_index_damaged"),
// one of which nothing is here or that most files disagree with in size (another game or version:
// "dump_index_mismatch"), or one whose files are in packs, not loose ("dump_index_packed").
// The dates of the list are never compared: copies and archives change them.
#ifndef MP_COMPLETE_H
#define MP_COMPLETE_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"
#include "mp_json.h"
#include "mp_source.h"

typedef struct {
    const char *path;   // relative, with '/'
    uint64_t size;
} mp_cfile;

// The class of each file (mp_complete_check's cls).
enum { MP_CF_IGNORED = 0, MP_CF_LISTED = 1, MP_CF_EXTRA = 2, MP_CF_LEFTOVER = 3 };

#define MP_COMPLETE_PATHS 20

typedef struct {
    int checkable;
    // "" when checkable; else "no_table", "unknown_format", "damaged" or "unreadable" (static), or
    // why the dump tool's list was not used: "dump_index_damaged", "dump_index_mismatch",
    // "dump_index_packed"
    const char *reason;
    // the list it was checked against: "playgo" (the original package's), "dump_index" (the dump
    // tool's), "" when not checkable (static)
    const char *source;
    uint64_t listed, present, missing;
    uint64_t size_mismatch;   // dump_index: there, of another size than the dump tool wrote
    uint64_t extra_count, extra_bytes;
    uint64_t leftover_count, leftover_bytes;
    // the first extra files, in the order given (a source's: by path), malloc
    size_t npaths;
    char *paths[MP_COMPLETE_PATHS];
} mp_complete;

// Checks files[0..n) against table (the bytes of playgo-hash-table.dat; NULL when the dump has none).
// cls, when not NULL, gets the class of each file. 0, or -1 when memory runs out (c says nothing).
int mp_complete_check(const mp_cfile *files, size_t n, const uint8_t *table, size_t table_len, mp_complete *c,
                      uint8_t *cls);
// The same with the dump tool's list too (the bytes of ampr_emu.index; NULL when there is none): the
// table wins when it can be used.
int mp_complete_check_lists(const mp_cfile *files, size_t n, const uint8_t *table, size_t table_len,
                            const uint8_t *dump_index, size_t dump_index_len, mp_complete *c, uint8_t *cls);
// 1 when the leftovers are proven not to be the game's: only the original package's table proves it.
int mp_complete_proves_leftovers(const mp_complete *c);
// The same over the files of a source, with the table it carries. A table that cannot be read
// leaves it unchecked ("unreadable"). cls: one per entry of the source. -1 only out of memory.
int mp_complete_source(mp_source *s, mp_complete *c, uint8_t *cls, mp_err *e);
// Frees the paths; c can be checked again.
void mp_complete_free(mp_complete *c);
// Keeps only the counts (frees the paths): for a result that is copied around.
void mp_complete_counts_only(mp_complete *c);

// 1 for what dump tools leave behind, by its path alone.
int mp_complete_is_leftover(const char *path);
// 1 for the files of the OS (Thumbs.db, .DS_Store, ._*) and what is in its folders.
int mp_complete_is_junk(const char *path);
// 1 for what MaxPack adds to a game it packs with its DLC: dlc_emu.ini at the root and what is in the
// addcont0/ to addcont99/ folders at the root (ASCII case aside). Neither list ever counts it as a file
// of more.
int mp_complete_is_dlc(const char *path);

// The object {"checkable":...,"reason":...,"listed":...,...}; with_paths adds "extra_paths".
void mp_complete_json(const mp_complete *c, int with_paths, mp_jbuf *b);
// One line: "complete: all N files of the original package", "incomplete: N of the M files of the
// original package are missing", for the dump tool's list "all N files the dump tool wrote are here
// (...)" or "damaged copy: ...", or "not checked: <why>".
void mp_complete_summary(const mp_complete *c, char *buf, size_t cap);
// What is not in the original package, and what of it dump tools left (empty when there is none).
void mp_complete_extra_line(const mp_complete *c, char *buf, size_t cap);

#endif
