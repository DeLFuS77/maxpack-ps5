// SPDX-License-Identifier: GPL-3.0-or-later
// La página del instalador (web/index.html), incrustada al compilar (cmake/embed.cmake).
#ifndef MP_PAGE_H
#define MP_PAGE_H

#include <stddef.h>

extern const unsigned char mp_page_html[];   // acaba en NUL (no contado en la longitud)
extern const size_t mp_page_html_len;

#endif
