// SPDX-License-Identifier: GPL-3.0-or-later
// HTTP/1.1 mínimo para el instalador: un servidor que atiende las conexiones una a una
// (Connection: close) y un cliente para hablar con ShadowMountPlus. Solo IPv4 numérico.
#ifndef MP_HTTP_H
#define MP_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

#define MP_HTTP_MAX_BODY (1u << 20)

typedef struct {
    const char *method, *path, *query;   // query sin '?', "" si no hay
    const char *body;
    size_t body_len;
    const char *content_type, *host;   // cabeceras ("" si no vienen; NULL fuera del servidor)
    int loopback;                      // 1 si la petición viene de 127.0.0.0/8 (0 si no se sabe)
} mp_http_req;

typedef struct {
    int status;                 // 200 si el manejador no lo cambia
    const char *content_type;   // "application/json" si no se pone
    char *body;                 // malloc; lo libera el servidor
    size_t body_len;
} mp_http_resp;

typedef void (*mp_http_handler)(void *ctx, const mp_http_req *rq, mp_http_resp *rs);

typedef struct mp_http_server mp_http_server;
// port 0: el sistema elige uno libre (mp_http_port lo dice).
mp_http_server *mp_http_start(const char *bind_addr, uint16_t port, mp_http_handler h, void *ctx, mp_err *e);
uint16_t mp_http_port(const mp_http_server *s);
void mp_http_stop(mp_http_server *s);

// Petición con espera máxima timeout_ms para conectar y para cada lectura. resp: malloc, acabado
// en NUL. -1 si no se pudo conectar o no hubo respuesta completa.
int mp_http_request(const char *host, uint16_t port, const char *method, const char *path, const char *body,
                    size_t body_len, unsigned timeout_ms, int *status, char **resp, size_t *resp_len, mp_err *e);

int mp_url_decode(const char *in, char *out, size_t cap);   // %XX y '+'; -1 si no cabe o está mal
// Valor (descodificado) de key en una query "a=1&b=2". -1 si no está.
int mp_http_query_get(const char *query, const char *key, char *out, size_t cap);

#endif
