// SPDX-License-Identifier: GPL-3.0-or-later
// JSON mínimo para el instalador: archivos de estado y registro, API web y ShadowMountPlus.
#ifndef MP_JSON_H
#define MP_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

typedef struct mp_json mp_json;

// Analiza s[0..n) entero (profundidad máxima 32). NULL si no es JSON válido.
mp_json *mp_json_parse(const char *s, size_t n, mp_err *e);
void mp_json_free(mp_json *j);
const mp_json *mp_json_get(const mp_json *obj, const char *key);   // NULL si falta o no es objeto
const char *mp_json_str(const mp_json *j);                         // NULL si no es cadena (UTF-8)
int mp_json_u64(const mp_json *j, uint64_t *out);                  // 0 si es entero >= 0 que cabe
int mp_json_i64(const mp_json *j, int64_t *out);                   // 0 si es entero que cabe
int mp_json_bool(const mp_json *j, int *out);                      // 0 si es true/false
size_t mp_json_len(const mp_json *arr);                            // 0 si no es array
const mp_json *mp_json_at(const mp_json *arr, size_t i);           // NULL fuera de rango

// Salida que crece. p siempre acaba en NUL; n no lo cuenta. oom != 0 si faltó memoria.
typedef struct {
    char *p;
    size_t n, cap;
    int oom;
} mp_jbuf;
void mp_jbuf_raw(mp_jbuf *b, const char *s);   // tal cual: { } [ ] , :
void mp_jbuf_str(mp_jbuf *b, const char *s);   // entre comillas; < > & y controles escapados,
                                               // UTF-8 no válido como �
void mp_jbuf_u64(mp_jbuf *b, uint64_t v);
void mp_jbuf_i64(mp_jbuf *b, int64_t v);
void mp_jbuf_free(mp_jbuf *b);

#endif
