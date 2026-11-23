// SPDX-License-Identifier: GPL-3.0-or-later
// Una petición entera a la API del instalador sin pasar por el servidor HTTP: para quien incrusta
// el instalador (el payload de la consola) y tiene su propio servidor.
#ifndef MP_API_CALL_H
#define MP_API_CALL_H

#include <stddef.h>
#include <stdint.h>

#include "mp_api.h"

typedef struct {
    int status;
    char ctype[64];
    uint8_t *body;   // malloc; lo libera mp_api_out_free
    size_t len;
} mp_api_out;

// method "GET"/"POST"; path con la ruta del instalador (y su query, si la hay); host y ctype como
// llegan (se aplican las mismas comprobaciones que en el servidor). loopback: 1 si viene de la
// propia consola. 0 = respondida (out rellena); -1 = sin memoria o argumentos inválidos.
int mp_api_call(mp_api *api, const char *method, const char *path, const char *host, const char *ctype,
                const uint8_t *body, size_t body_len, int loopback, mp_api_out *out);
void mp_api_out_free(mp_api_out *out);

#endif
