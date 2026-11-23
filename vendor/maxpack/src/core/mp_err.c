// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_err.h"

#include <stdarg.h>
#include <stdio.h>

void mp_err_set(mp_err *e, const char *fmt, ...) {
    if (!e) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
}
