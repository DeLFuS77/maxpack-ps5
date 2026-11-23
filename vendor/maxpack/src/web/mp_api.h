// SPDX-License-Identifier: GPL-3.0-or-later
// API JSON del instalador y su cola de trabajos (una instalación a la vez).
//   GET  /                 la página
//   GET  /api/state        unidades, encontrados, instalados, trabajo en marcha, último resultado
//   POST /api/scan {}      vuelve a buscar
//   POST /api/probe        {"path"}
//   POST /api/install      {"p5z","dest","replace":[...],"delete_p5z"}
//   POST /api/pause {}
//   POST /api/verify       {"title_id"}
//   POST /api/uninstall    {"title_id","confirm":true}
//   GET  /api/icon?p5z=    sce_sys/icon0.png del .p5z
//   POST /api/quit {}      pide salir a quien la alberga (solo desde loopback; 403 si no)
// Un error de la aplicación es 200 {"ok":false,"code","detail"}; un JSON mal formado, 400; una ruta
// desconocida, 404.
#ifndef MP_API_H
#define MP_API_H

#include <stddef.h>

#include "mp_http.h"
#include "mp_inst.h"
#include "mp_smp.h"

typedef struct mp_api mp_api;

// env y smp se copian. smp_retry_ms: cada cuánto se reintenta lo pendiente en ShadowMountPlus.
mp_api *mp_api_new(const mp_inst_env *env, const mp_smp_cfg *smp, unsigned smp_retry_ms, mp_err *e);
void mp_api_handle(void *api, const mp_http_req *rq, mp_http_resp *rs);   // para mp_http_start
int mp_api_wait_idle(mp_api *a, unsigned timeout_ms);                    // 0 si la cola se vació
void mp_api_set_page(mp_api *a, const unsigned char *html, size_t len);
// Con mp_api_set_quit, POST /api/quit (solo loopback) llama a cb(ctx) una vez y responde 200.
// cb no debe parar el servidor ni liberar la API: solo avisa (se llama dentro del manejador, antes de
// enviar la respuesta, y pararlo desde ahí se bloquearía o usaría memoria liberada). Hay que llamar
// a mp_api_set_quit antes de que la API empiece a atender peticiones: cb y ctx no llevan cerrojo.
typedef void (*mp_api_quit_cb)(void *ctx);
void mp_api_set_quit(mp_api *a, mp_api_quit_cb cb, void *ctx);
void mp_api_free(mp_api *a);

#endif
