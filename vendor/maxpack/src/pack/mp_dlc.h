// SPDX-License-Identifier: GPL-3.0-or-later
// The DLC of a game, packed with it: each DLC with files of its own goes to addcont0/, addcont1/...
// inside the game, and MaxPack writes the dlc_emu.ini that tells dlc_emu (drakmor/dlc_emu, which
// MaxPack for the console adds) what the game owns: a [PSAC] section with the content ID and the
// mount point /app0/addcontN of each DLC with files, a [PSAL] one with download_status=NO_EXTRA_DATA
// for each license. The game then sees its DLC installed, from inside the .p5z, as a game in a folder
// does with a dlc_emu.ini written by hand.
//
// A DLC is a package of additional content (CNT content type 0x21), or a folder or image with
// sce_sys/param.json and no eboot.bin at its root. Its content ID is the package's, or its
// param.json's contentId: 36 characters (XXYYYY-TITLEID_00-LABEL, as dlc_emu wants it), of the game's
// title ID. One with no file outside sce_sys/ (OS junk aside) is a license only and adds no folder.
#ifndef MP_DLC_H
#define MP_DLC_H

#include <stddef.h>
#include <stdint.h>

#include "mp_complete.h"
#include "mp_err.h"
#include "mp_source.h"

#define MP_DLC_MAX_DATA 100          // addcont0 to addcont99: dlc_emu takes mount points of at most 15 characters
#define MP_DLC_INI "dlc_emu.ini"
#define MP_DLC_INI_MAX (32u << 10)   // the most of it dlc_emu reads
#define MP_DLC_MAX_ENTRIES 1024      // the most entries dlc_emu takes

typedef struct {
    char content_id[37];
    int data;            // it has files of its own (in folder); else a license only
    char folder[16];     // "addcont0" ("" for a license)
    uint64_t files, bytes;   // what is in folder
} mp_dlc;

typedef struct {
    mp_dlc *v;
    size_t n;
} mp_dlc_list;

void mp_dlc_list_free(mp_dlc_list *l);

// Opens the DLC at paths[0..n) (packages with mp_source_set_passcode's passcode), checks them against
// the game and gives the game with them (mp_source_merge): *game is taken; on success it is the game
// with its DLC, on failure it is closed and NULL, with the reason in e and *code "dlc_has_ini" (the
// game has its own dlc_emu.ini), "dlc_too_many" (more than MP_DLC_MAX_DATA with files, or an ini of
// more than MP_DLC_INI_MAX) or "dlc_refused" (any other). out, when not NULL, gets what each DLC is,
// in the order given.
int mp_dlc_attach(mp_source **game, const char *const *paths, size_t n, mp_dlc_list *out, const char **code,
                  mp_err *e);
// The same for a game that carries have[0..nhave) already (a .p5z made with them): its dlc_emu.ini must not
// be in game, since it is written again with those first and then the new ones; the new DLC with data go to
// the folders after the last of theirs, and one of them (or its label) again is refused. out gets the new ones.
int mp_dlc_attach_ex(mp_source **game, const mp_dlc *have, size_t nhave, const char *const *paths, size_t n,
                     mp_dlc_list *out, const char **code, mp_err *e);

// The dlc_emu.ini of v[0..n) (malloc, NUL-terminated; *len its bytes); NULL out of memory.
char *mp_dlc_ini(const mp_dlc *v, size_t n, size_t *len);

// 1 if cid is a content ID dlc_emu takes: 36 characters, '-' at 6 and 19, '_' at 16, the rest ASCII
// letters and digits.
int mp_dlc_content_id_ok(const char *cid);
// 1 if a DLC has files of its own: one outside sce_sys/ that is not the OS's (mp_complete_is_junk).
int mp_dlc_has_data(mp_source *s);

// The DLC a dlc_emu.ini lists (a game's, written by MaxPack or by hand), as dlc_emu would take them:
// each [PSAC] (data, its folder the mount point less "/app0/", "" when it is not one dlc_emu takes) and
// [PSAL] section with a content_id of 36 printable characters, in their order, at most
// MP_DLC_MAX_ENTRIES; other sections and keys are not read. More than MP_DLC_INI_MAX bytes lists none,
// as dlc_emu reads none. Files and bytes are 0 (see mp_dlc_sizes). 0, or -1 out of memory.
int mp_dlc_parse(const char *ini, size_t len, mp_dlc_list *out);
// The files and bytes of each DLC with a folder: those of files[0..n) in it (ASCII case aside).
void mp_dlc_sizes(mp_dlc_list *l, const mp_cfile *files, size_t n);

#endif
