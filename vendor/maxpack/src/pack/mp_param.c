// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <string.h>

#include "mp_json.h"
#include "mp_param.h"

// Copies s into dst (of cap bytes) without splitting a multibyte UTF-8 character.
static void copy_str(char *dst, size_t cap, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) n--;   // s[n] is a continuation: step back
    }
    if (n) memcpy(dst, s, n);
    dst[n] = 0;
}

// The JSON of a param.json: a UTF-8 BOM at the start, or padding spaces and NULs at the end, do not
// change it. NULL if it is not valid JSON.
static mp_json *parse(const char *json, size_t n) {
    if (n >= 3 && !memcmp(json, "\xef\xbb\xbf", 3)) {
        json += 3;
        n -= 3;
    }
    while (n > 0 && (json[n - 1] == 0 || json[n - 1] == ' ' || json[n - 1] == '\t' || json[n - 1] == '\r' ||
                     json[n - 1] == '\n'))
        n--;
    return mp_json_parse(json, n, NULL);
}

int mp_param_read(const char *json, size_t n, char id[16], char ver[16], char name[128]) {
    id[0] = ver[0] = name[0] = 0;
    mp_json *j = parse(json, n);
    if (!j) return -1;
    copy_str(id, 16, mp_json_str(mp_json_get(j, "titleId")));
    copy_str(ver, 16, mp_json_str(mp_json_get(j, "contentVersion")));
    const mp_json *loc = mp_json_get(j, "localizedParameters");
    const char *def = mp_json_str(mp_json_get(loc, "defaultLanguage"));
    const char *t = def ? mp_json_str(mp_json_get(mp_json_get(loc, def), "titleName")) : NULL;
    if (!t) t = mp_json_str(mp_json_get(j, "titleName"));
    copy_str(name, 128, t);
    mp_json_free(j);
    return 0;
}

int mp_param_content_id(const char *json, size_t n, char cid[48]) {
    cid[0] = 0;
    mp_json *j = parse(json, n);
    if (!j) return -1;
    copy_str(cid, 48, mp_json_str(mp_json_get(j, "contentId")));
    mp_json_free(j);
    return 0;
}
