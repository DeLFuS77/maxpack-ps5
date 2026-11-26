// SPDX-License-Identifier: GPL-3.0-or-later
// Which files must stay loose on the console and which can be packed (design 6.3).
#ifndef MP_RULES_H
#define MP_RULES_H

#include <stdint.h>

#include "mp_err.h"

typedef enum { MP_PACKABLE = 0, MP_LOOSE = 1 } mp_class;

typedef struct mp_rules mp_rules;

mp_rules *mp_rules_new(void);
// Adds a --keep-loose pattern. Error if it is empty or contains '\'.
int mp_rules_add_keep_loose(mp_rules *r, const char *glob, mp_err *e);
mp_class mp_rules_classify(const mp_rules *r, const char *rel_path, uint64_t size);
void mp_rules_free(mp_rules *r);

// '*' and '?' within a segment, '**' = any number of segments (also zero).
// ASCII case-insensitive. Returns 1 if it matches.
int mp_glob_match(const char *glob, const char *path);

#endif
