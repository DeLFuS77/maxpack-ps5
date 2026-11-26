// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal HTTP/1.1 for the installer: a server that handles connections one at a time
// (Connection: close) and a client to talk to ShadowMountPlus. Numeric IPv4 only.
#ifndef MP_HTTP_H
#define MP_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

#define MP_HTTP_MAX_BODY (1u << 20)

typedef struct {
    const char *method, *path, *query;   // query without '?', "" if there is none
    const char *body;
    size_t body_len;
    const char *content_type, *host;   // headers ("" if absent; NULL outside the server)
    int loopback;                      // 1 if the request comes from 127.0.0.0/8 (0 if unknown)
} mp_http_req;

typedef struct {
    int status;                 // 200 if the handler does not change it
    const char *content_type;   // "application/json" if not set
    char *body;                 // malloc; freed by the server
    size_t body_len;
} mp_http_resp;

typedef void (*mp_http_handler)(void *ctx, const mp_http_req *rq, mp_http_resp *rs);

typedef struct mp_http_server mp_http_server;
// port 0: the system picks a free one (mp_http_port says which).
mp_http_server *mp_http_start(const char *bind_addr, uint16_t port, mp_http_handler h, void *ctx, mp_err *e);
uint16_t mp_http_port(const mp_http_server *s);
void mp_http_stop(mp_http_server *s);

// Request with a maximum wait of timeout_ms to connect and for each read. resp: malloc, NUL-
// terminated. -1 if it could not connect or there was no complete response.
int mp_http_request(const char *host, uint16_t port, const char *method, const char *path, const char *body,
                    size_t body_len, unsigned timeout_ms, int *status, char **resp, size_t *resp_len, mp_err *e);

int mp_url_decode(const char *in, char *out, size_t cap);   // %XX and '+'; -1 if it does not fit or is malformed
// (Decoded) value of key in a query "a=1&b=2". -1 if it is absent.
int mp_http_query_get(const char *query, const char *key, char *out, size_t cap);

#endif
