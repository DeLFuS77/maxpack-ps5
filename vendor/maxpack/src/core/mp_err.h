// SPDX-License-Identifier: GPL-3.0-or-later
// Mensajes de error: cada función que puede fallar recibe un mp_err* (puede ser NULL).
#ifndef MP_ERR_H
#define MP_ERR_H

typedef struct {
    char msg[512];
} mp_err;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void mp_err_set(mp_err *e, const char *fmt, ...);

#endif
