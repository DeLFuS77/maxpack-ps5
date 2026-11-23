// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 32

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct mp_json {
    int type;
    int b;
    char *key;   // en un miembro de objeto
    char *s;     // cadena o texto del número
    mp_json **kids;
    size_t nkids;
};

typedef struct {
    const char *p, *end;
    mp_err *e;
    int failed;
} parser;

static void fail(parser *ps, const char *why) {
    if (!ps->failed) mp_err_set(ps->e, "JSON no válido: %s", why);
    ps->failed = 1;
}

static void skip_ws(parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

void mp_json_free(mp_json *j) {
    if (!j) return;
    for (size_t i = 0; i < j->nkids; i++) mp_json_free(j->kids[i]);
    free(j->kids);
    free(j->key);
    free(j->s);
    free(j);
}

static int add_kid(parser *ps, mp_json *parent, mp_json *kid) {
    mp_json **k = realloc(parent->kids, (parent->nkids + 1) * sizeof *k);
    if (!k) {
        fail(ps, "sin memoria");
        return -1;
    }
    parent->kids = k;
    k[parent->nkids++] = kid;
    return 0;
}

static int hex4(parser *ps, unsigned *out) {
    if (ps->end - ps->p < 4) {
        fail(ps, "\\u incompleto");
        return -1;
    }
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = *ps->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else {
            fail(ps, "\\u con un carácter que no es hexadecimal");
            return -1;
        }
    }
    *out = v;
    return 0;
}

static size_t put_utf8(char *o, unsigned cp) {
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Lee una cadena (ps->p en la comilla de apertura). Devuelve malloc o NULL.
static char *parse_string(parser *ps) {
    ps->p++;
    // Cada escape ocupa al menos tantos bytes como su UTF-8: el tamaño de la entrada basta.
    const char *q = ps->p;
    while (q < ps->end && *q != '"') q += (*q == '\\' && q + 1 < ps->end) ? 2 : 1;
    if (q >= ps->end) {
        fail(ps, "cadena sin cerrar");
        return NULL;
    }
    char *out = malloc((size_t)(q - ps->p) + 1), *o = out;
    if (!out) {
        fail(ps, "sin memoria");
        return NULL;
    }
    while (ps->p < ps->end && *ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p;
        if (c < 0x20) {
            fail(ps, "carácter de control sin escapar en una cadena");
            break;
        }
        if (c != '\\') {
            *o++ = (char)c;
            ps->p++;
            continue;
        }
        ps->p++;
        if (ps->p >= ps->end) break;
        char x = *ps->p++;
        switch (x) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '/': *o++ = '/'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            unsigned cp = 0, lo = 0;
            if (hex4(ps, &cp) != 0) break;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u') {
                    fail(ps, "sustituto UTF-16 sin pareja");
                    break;
                }
                ps->p += 2;
                if (hex4(ps, &lo) != 0) break;
                if (lo < 0xDC00 || lo > 0xDFFF) {
                    fail(ps, "sustituto UTF-16 sin pareja");
                    break;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                fail(ps, "sustituto UTF-16 sin pareja");
                break;
            } else if (cp == 0) {
                fail(ps, "carácter NUL en una cadena");
                break;
            }
            o += put_utf8(o, cp);
            break;
        }
        default: fail(ps, "escape desconocido");
        }
        if (ps->failed) break;
    }
    if (ps->failed) {
        free(out);
        return NULL;
    }
    ps->p++;   // comilla de cierre
    *o = 0;
    return out;
}

static mp_json *parse_value(parser *ps, int depth);

static mp_json *new_node(parser *ps, int type) {
    mp_json *j = calloc(1, sizeof *j);
    if (!j) fail(ps, "sin memoria");
    else j->type = type;
    return j;
}

static mp_json *parse_number(parser *ps) {
    const char *s = ps->p;
    if (ps->p < ps->end && *ps->p == '-') ps->p++;
    if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') {
        fail(ps, "número mal formado");
        return NULL;
    }
    if (*ps->p == '0') ps->p++;
    else
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') {
            fail(ps, "número mal formado");
            return NULL;
        }
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') {
            fail(ps, "número mal formado");
            return NULL;
        }
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    }
    mp_json *j = new_node(ps, J_NUM);
    if (!j) return NULL;
    size_t len = (size_t)(ps->p - s);
    j->s = malloc(len + 1);
    if (!j->s) {
        free(j);
        fail(ps, "sin memoria");
        return NULL;
    }
    memcpy(j->s, s, len);
    j->s[len] = 0;
    return j;
}

static int word(parser *ps, const char *w) {
    size_t n = strlen(w);
    if ((size_t)(ps->end - ps->p) >= n && !memcmp(ps->p, w, n)) {
        ps->p += n;
        return 1;
    }
    return 0;
}

static mp_json *parse_container(parser *ps, int depth, int obj) {
    if (depth >= MAX_DEPTH) {
        fail(ps, "demasiados niveles");
        return NULL;
    }
    ps->p++;
    mp_json *j = new_node(ps, obj ? J_OBJ : J_ARR);
    if (!j) return NULL;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
        ps->p++;
        return j;
    }
    for (;;) {
        char *key = NULL;
        if (obj) {
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != '"') {
                fail(ps, "se esperaba el nombre de un miembro");
                break;
            }
            key = parse_string(ps);
            if (!key) break;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                fail(ps, "falta ':'");
                break;
            }
            ps->p++;
        }
        mp_json *v = parse_value(ps, depth + 1);
        if (!v) {
            free(key);
            break;
        }
        v->key = key;
        if (add_kid(ps, j, v) != 0) {
            mp_json_free(v);
            break;
        }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
            ps->p++;
            return j;
        }
        fail(ps, obj ? "falta ',' o '}'" : "falta ',' o ']'");
        break;
    }
    mp_json_free(j);
    return NULL;
}

static mp_json *parse_value(parser *ps, int depth) {
    skip_ws(ps);
    if (ps->p >= ps->end) {
        fail(ps, "falta un valor");
        return NULL;
    }
    char c = *ps->p;
    if (c == '{') return parse_container(ps, depth, 1);
    if (c == '[') return parse_container(ps, depth, 0);
    if (c == '"') {
        char *s = parse_string(ps);
        if (!s) return NULL;
        mp_json *j = new_node(ps, J_STR);
        if (!j) {
            free(s);
            return NULL;
        }
        j->s = s;
        return j;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(ps);
    mp_json *j = NULL;
    if (word(ps, "true")) {
        if ((j = new_node(ps, J_BOOL)) != NULL) j->b = 1;
    } else if (word(ps, "false")) {
        j = new_node(ps, J_BOOL);
    } else if (word(ps, "null")) {
        j = new_node(ps, J_NULL);
    } else {
        fail(ps, "valor desconocido");
    }
    return j;
}

mp_json *mp_json_parse(const char *s, size_t n, mp_err *e) {
    parser ps = {s, s + n, e, 0};
    mp_json *j = parse_value(&ps, 0);
    if (j) {
        skip_ws(&ps);
        if (ps.p != ps.end) {
            fail(&ps, "hay texto después del valor");
            mp_json_free(j);
            j = NULL;
        }
    }
    return j;
}

const mp_json *mp_json_get(const mp_json *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (size_t i = 0; i < obj->nkids; i++)
        if (!strcmp(obj->kids[i]->key, key)) return obj->kids[i];
    return NULL;
}

const char *mp_json_str(const mp_json *j) { return j && j->type == J_STR ? j->s : NULL; }

static int is_integer_text(const char *s) { return !strpbrk(s, ".eE"); }

int mp_json_u64(const mp_json *j, uint64_t *out) {
    if (!j || j->type != J_NUM || !is_integer_text(j->s) || j->s[0] == '-') return -1;
    uint64_t v = 0;
    for (const char *p = j->s; *p; p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) return -1;
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

int mp_json_i64(const mp_json *j, int64_t *out) {
    if (!j || j->type != J_NUM || !is_integer_text(j->s)) return -1;
    int neg = j->s[0] == '-';
    uint64_t v = 0;
    for (const char *p = j->s + neg; *p; p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) return -1;
        v = v * 10 + d;
    }
    if (neg ? v > (uint64_t)INT64_MAX + 1 : v > (uint64_t)INT64_MAX) return -1;
    *out = neg ? (v == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)v) : (int64_t)v;
    return 0;
}

int mp_json_bool(const mp_json *j, int *out) {
    if (!j || j->type != J_BOOL) return -1;
    *out = j->b;
    return 0;
}

size_t mp_json_len(const mp_json *arr) { return arr && arr->type == J_ARR ? arr->nkids : 0; }

const mp_json *mp_json_at(const mp_json *arr, size_t i) {
    return arr && arr->type == J_ARR && i < arr->nkids ? arr->kids[i] : NULL;
}

// ---- salida -------------------------------------------------------------------------------

static void put(mp_jbuf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) {
            b->oom = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void mp_jbuf_raw(mp_jbuf *b, const char *s) { put(b, s, strlen(s)); }

// Longitud de una secuencia UTF-8 válida que empieza en s, o 0.
static size_t utf8_len(const unsigned char *s) {
    unsigned c = s[0];
    if (c < 0x80) return 1;
    size_t n;
    unsigned cp;
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if ((n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
        return 0;
    return n;
}

void mp_jbuf_str(mp_jbuf *b, const char *s) {
    put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned char c = *p;
        char esc[8];
        const char *rep = NULL;
        switch (c) {
        case '"': rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n"; break;
        case '\r': rep = "\\r"; break;
        case '\t': rep = "\\t"; break;
        case '\b': rep = "\\b"; break;
        case '\f': rep = "\\f"; break;
        case '<': rep = "\\u003c"; break;
        case '>': rep = "\\u003e"; break;
        case '&': rep = "\\u0026"; break;
        default:
            if (c < 0x20) {
                snprintf(esc, sizeof esc, "\\u%04x", c);
                rep = esc;
            }
        }
        if (rep) {
            put(b, rep, strlen(rep));
            p++;
            continue;
        }
        size_t n = utf8_len(p);
        if (n == 0) {
            put(b, "\\ufffd", 6);
            p++;
        } else {
            put(b, (const char *)p, n);
            p += n;
        }
    }
    put(b, "\"", 1);
}

void mp_jbuf_u64(mp_jbuf *b, uint64_t v) {
    char t[24];
    snprintf(t, sizeof t, "%llu", (unsigned long long)v);
    put(b, t, strlen(t));
}

void mp_jbuf_i64(mp_jbuf *b, int64_t v) {
    char t[24];
    snprintf(t, sizeof t, "%lld", (long long)v);
    put(b, t, strlen(t));
}

void mp_jbuf_free(mp_jbuf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
    b->oom = 0;
}
