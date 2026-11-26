// SPDX-License-Identifier: GPL-3.0-or-later
// The installer's JSON API and its job queue (one install at a time).
//   GET  /api/state        drives, found, installed, job in progress, last result
//   POST /api/scan {}      searches again
//   POST /api/probe        {"path"}
//   POST /api/install      {"p5z","dest","replace":[...],"delete_p5z"}
//   POST /api/pause {}
//   POST /api/verify       {"title_id"}
//   POST /api/uninstall    {"title_id","confirm":true}
//   GET  /api/icon?p5z=    the .p5z's sce_sys/icon0.png
//   POST /api/quit {}      asks whoever hosts it to exit (loopback only; 403 otherwise)
// An application error is 200 {"ok":false,"code","detail"}; a malformed JSON, 400; an unknown
// path, 404.
#ifndef MP_API_H
#define MP_API_H

#include <stddef.h>

#include "mp_http.h"
#include "mp_inst.h"
#include "mp_smp.h"

typedef struct mp_api mp_api;

// env and smp are copied. smp_retry_ms: how often what is pending in ShadowMountPlus is retried.
mp_api *mp_api_new(const mp_inst_env *env, const mp_smp_cfg *smp, unsigned smp_retry_ms, mp_err *e);
void mp_api_handle(void *api, const mp_http_req *rq, mp_http_resp *rs);   // for mp_http_start
int mp_api_wait_idle(mp_api *a, unsigned timeout_ms);                    // 0 if the queue emptied
// With mp_api_set_quit, POST /api/quit (loopback only) calls cb(ctx) once and answers 200.
// cb must not stop the server or free the API: it only notifies (it is called inside the handler, before
// sending the response, and stopping it from there would block or use freed memory). mp_api_set_quit
// must be called before the API starts serving requests: cb and ctx have no lock.
typedef void (*mp_api_quit_cb)(void *ctx);
void mp_api_set_quit(mp_api *a, mp_api_quit_cb cb, void *ctx);
void mp_api_free(mp_api *a);

#endif
