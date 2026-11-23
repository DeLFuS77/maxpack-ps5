// SPDX-License-Identifier: GPL-3.0-or-later
// Cliente de la API local de ShadowMountPlus 1.7 (HTTP/JSON en 127.0.0.1:10101, todo POST).
// Solo se usan version, games, games/fakelib (del juego instalado), manual/add, manual/remove y
// scan. Nunca settings/update: los ajustes globales son del usuario.
#ifndef MP_SMP_H
#define MP_SMP_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"
#include "mp_inst.h"

// Provisional: lo decide la prueba P1 en consola. Se cambia en <internal>/maxpack/config.json
// con "smp_fakelib_mode".
#define MP_SMP_FAKELIB_DEFAULT "full"

typedef struct {
    const char *host;
    uint16_t port;
    unsigned timeout_ms;
    const char *fakelib_mode;
    char mode_buf[16];
} mp_smp_cfg;

void mp_smp_cfg_default(mp_smp_cfg *c);   // 127.0.0.1, 10101, 3000, MP_SMP_FAKELIB_DEFAULT
// Lee "smp_fakelib_mode" de <internal>/maxpack/config.json si está y es válido.
void mp_smp_cfg_load(mp_smp_cfg *c, const mp_inst_env *env);

int mp_smp_alive(const mp_smp_cfg *c, char *version, size_t vlen);   // 1 si responde

typedef struct {
    char *title_id, *path, *source_type;
    int running;   // -1: ShadowMountPlus 1.7 no lo dice
} mp_smp_game;

int mp_smp_games(const mp_smp_cfg *c, mp_smp_game **out, size_t *n, mp_err *e);
void mp_smp_games_free(mp_smp_game *v, size_t n);

// 0 hecho; 1 pendiente (why: motivo, p. ej. "escaneo aplazado: game_active"); -1 sin ShadowMountPlus.
// set_fakelib: 1 en juegos con packs zstd (games/fakelib primero); 0 en el resto (solo manual/add y scan).
int mp_smp_register(const mp_smp_cfg *c, const char *title_id, const char *path, int set_fakelib, char *why,
                    size_t wl);
// manual/remove de path y scan (al desinstalar). Mismos códigos que mp_smp_register.
int mp_smp_forget(const mp_smp_cfg *c, const char *path, char *why, size_t wl);
int mp_smp_rescan(const mp_smp_cfg *c, char *why, size_t wl);

// Reintenta los registros con smp_pending y guarda el resultado en cada uno.
int mp_inst_smp_sync(const mp_inst_env *env, const mp_smp_cfg *c, mp_err *e);

#endif
