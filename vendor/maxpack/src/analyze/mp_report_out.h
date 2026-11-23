// SPDX-License-Identifier: GPL-3.0-or-later
// Salida del análisis: texto para la terminal y JSON para guardar.
#ifndef MP_REPORT_OUT_H
#define MP_REPORT_OUT_H

#include <stdio.h>

#include "mp_analyze.h"

int mp_report_write_text(const mp_report *r, FILE *out);
// JSON en memoria (malloc; NULL si no hay memoria).
char *mp_report_json(const mp_report *r);
int mp_report_write_json(const mp_report *r, const char *utf8_path, mp_err *e);

#endif
