// SPDX-License-Identifier: GPL-3.0-or-later
// A whole request to the installer's API without going through the HTTP server: for whoever embeds
// the installer (the console's payload) and has its own server.
#ifndef MP_API_CALL_H
#define MP_API_CALL_H

#include <stddef.h>
#include <stdint.h>

#include "mp_api.h"

typedef struct {
    int status;
    char ctype[64];
    uint8_t *body;   // malloc; freed by mp_api_out_free
    size_t len;
} mp_api_out;

// method "GET"/"POST"; path with the installer's path (and its query, if any); host and ctype as
// they arrive (the same checks as on the server apply). loopback: 1 if it comes from the
// console itself. 0 = answered (out filled in); -1 = out of memory or invalid arguments.
int mp_api_call(mp_api *api, const char *method, const char *path, const char *host, const char *ctype,
                const uint8_t *body, size_t body_len, int loopback, mp_api_out *out);
void mp_api_out_free(mp_api_out *out);

#endif
