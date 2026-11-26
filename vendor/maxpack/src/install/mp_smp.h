// SPDX-License-Identifier: GPL-3.0-or-later
// Client of ShadowMountPlus 1.7's local API (HTTP/JSON at 127.0.0.1:10101, everything POST).
// Only version, games, games/fakelib (of the installed game), manual/add, manual/remove and
// scan are used. Never settings/update: global settings belong to the user.
#ifndef MP_SMP_H
#define MP_SMP_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"
#include "mp_inst.h"

// Provisional: decided by test P1 on the console. Changed in <internal>/maxpack/config.json
// with "smp_fakelib_mode".
#define MP_SMP_FAKELIB_DEFAULT "full"

typedef struct {
    const char *host;
    uint16_t port;
    unsigned timeout_ms;
    const char *fakelib_mode;
    char mode_buf[16];
} mp_smp_cfg;

void mp_smp_cfg_default(mp_smp_cfg *c);   // 127.0.0.1, 10101, 3000, MP_SMP_FAKELIB_DEFAULT
// Reads "smp_fakelib_mode" from <internal>/maxpack/config.json if present and valid.
void mp_smp_cfg_load(mp_smp_cfg *c, const mp_inst_env *env);

int mp_smp_alive(const mp_smp_cfg *c, char *version, size_t vlen);   // 1 if it responds

typedef struct {
    char *title_id, *path, *source_type;
    int running;   // -1: ShadowMountPlus 1.7 does not say
} mp_smp_game;

int mp_smp_games(const mp_smp_cfg *c, mp_smp_game **out, size_t *n, mp_err *e);
void mp_smp_games_free(mp_smp_game *v, size_t n);

// 0 done; 1 pending (why: reason, e.g. "scan postponed: game_active"); -1 without ShadowMountPlus.
// set_fakelib: 1 for games with zstd packs (games/fakelib first); 0 for the rest (only manual/add and scan).
int mp_smp_register(const mp_smp_cfg *c, const char *title_id, const char *path, int set_fakelib, char *why,
                    size_t wl);
// manual/remove of path and scan (on uninstall). Same codes as mp_smp_register.
int mp_smp_forget(const mp_smp_cfg *c, const char *path, char *why, size_t wl);
int mp_smp_rescan(const mp_smp_cfg *c, char *why, size_t wl);

// Retries the records with smp_pending and saves the result in each one.
int mp_inst_smp_sync(const mp_inst_env *env, const mp_smp_cfg *c, mp_err *e);

#endif
