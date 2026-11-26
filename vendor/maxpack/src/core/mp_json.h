// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal JSON for the installer: state and registry files, web API and ShadowMountPlus.
#ifndef MP_JSON_H
#define MP_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

typedef struct mp_json mp_json;

// Parses all of s[0..n) (maximum depth 32). NULL if it is not valid JSON.
mp_json *mp_json_parse(const char *s, size_t n, mp_err *e);
void mp_json_free(mp_json *j);
const mp_json *mp_json_get(const mp_json *obj, const char *key);   // NULL if missing or not an object
const char *mp_json_str(const mp_json *j);                         // NULL if not a string (UTF-8)
int mp_json_u64(const mp_json *j, uint64_t *out);                  // 0 if it is an integer >= 0 that fits
int mp_json_i64(const mp_json *j, int64_t *out);                   // 0 if it is an integer that fits
int mp_json_bool(const mp_json *j, int *out);                      // 0 if it is true/false
size_t mp_json_len(const mp_json *arr);                            // 0 if not an array
const mp_json *mp_json_at(const mp_json *arr, size_t i);           // NULL if out of range

// Growing output. p always ends in NUL; n does not count it. oom != 0 if memory ran out.
typedef struct {
    char *p;
    size_t n, cap;
    int oom;
} mp_jbuf;
void mp_jbuf_raw(mp_jbuf *b, const char *s);   // as is: { } [ ] , :
void mp_jbuf_str(mp_jbuf *b, const char *s);   // quoted; < > & and control characters escaped,
                                               // invalid UTF-8 as �
void mp_jbuf_u64(mp_jbuf *b, uint64_t v);
void mp_jbuf_i64(mp_jbuf *b, int64_t v);
void mp_jbuf_free(mp_jbuf *b);

#endif
