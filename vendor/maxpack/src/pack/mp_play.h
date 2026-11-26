// SPDX-License-Identifier: GPL-3.0-or-later
// Playing a .p5z without installing it: the console extracts the game's loose files to internal storage and reads the
// packs' volumes in place, inside the .p5z. Format in docs/p5z-format.md.
#ifndef MP_PLAY_H
#define MP_PLAY_H

#include <stddef.h>

typedef struct mp_play mp_play;

// The most volumes maxpack.image takes (ampr-emu-ng's p5z mode, 256 entries).
#define MP_PLAY_MAX_VOLUMES 256u

// Opens a .p5z to play it without installing. Checks header, footer and index (mp_p5z_open), ampr_zstd.index
// and each volume's header (mp_p5z_zpk, which calls check_volume), that the .p5z header's archive_id is
// the index's, and that its volume records are the packs' volumes. It does not cap their number: the
// console checks that maxpack.image takes them (256) and says so. NULL with the reason in err
// (NUL-terminated, truncated to errlen).
mp_play *mp_play_open(const char *p5z, char *err, size_t errlen);
// The volumes mp_play_json lists.
size_t mp_play_volume_count(const mp_play *p);
// JSON with malloc (freed with mp_play_free_str):
// {"title_id","version","name","archive_id":"<32 lowercase hex>","size":<bytes of the .p5z>,
//  "loose_bytes":N,"loose":[{"path","size"}...],"volumes":[{"id","offset","length"}...]}
// loose: roles 0, 2 and 3, in the index's order; volumes: role 1, by id (the NNN of ampr_zstd-NNN.pak).
char *mp_play_json(mp_play *p);
// Writes the loose file rel (role 0, 2 or 3) to dest, full path: creates the folders, replaces whatever was there,
// sets its mtime and checks its SHA-256. 0 if all goes well; -1 with the reason in err (dest may be left half-written).
int mp_play_extract(mp_play *p, const char *rel, const char *dest, char *err, size_t errlen);
void mp_play_close(mp_play *p);
void mp_play_free_str(char *s);

#endif
