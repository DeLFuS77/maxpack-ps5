// SPDX-License-Identifier: GPL-3.0-or-later
// Analysis output: text for the terminal and JSON to save.
#ifndef MP_REPORT_OUT_H
#define MP_REPORT_OUT_H

#include <stdio.h>

#include "mp_analyze.h"

int mp_report_write_text(const mp_report *r, FILE *out);
// JSON in memory (malloc; NULL if out of memory).
char *mp_report_json(const mp_report *r);
int mp_report_write_json(const mp_report *r, const char *utf8_path, mp_err *e);

#endif
