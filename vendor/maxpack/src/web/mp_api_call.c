// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_api_call.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mp_api_call(mp_api *api, const char *method, const char *path, const char *host, const char *ctype,
                const uint8_t *body, size_t body_len, int loopback, mp_api_out *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!api || !method || !path) return -1;
    // The query goes apart, as the server delivers it.
    char *p = malloc(strlen(path) + 1);
    // The body ends in NUL, as on the server.
    char *b = malloc(body_len + 1);
    if (!p || !b) {
        free(p);
        free(b);
        return -1;
    }
    strcpy(p, path);
    if (body_len) memcpy(b, body, body_len);
    b[body_len] = 0;
    const char *query = "";
    char *q = strchr(p, '?');
    if (q) {
        *q = 0;
        query = q + 1;
    }
    mp_http_req rq = {method, p, query, b, body_len, ctype ? ctype : "", host ? host : "", loopback ? 1 : 0};
    mp_http_resp rs = {200, NULL, NULL, 0};
    mp_api_handle(api, &rq, &rs);
    out->status = rs.status;
    snprintf(out->ctype, sizeof out->ctype, "%s", rs.content_type ? rs.content_type : "application/json");
    out->body = (uint8_t *)rs.body;   // takes over the response's malloc
    out->len = rs.body ? rs.body_len : 0;
    free(p);
    free(b);
    return 0;
}

void mp_api_out_free(mp_api_out *out) {
    if (!out) return;
    free(out->body);
    memset(out, 0, sizeof *out);
}
