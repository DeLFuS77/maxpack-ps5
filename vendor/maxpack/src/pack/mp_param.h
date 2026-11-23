// SPDX-License-Identifier: GPL-3.0-or-later
// Lectura de sce_sys/param.json, compartida por pack, describe y el instalador.
#ifndef MP_PARAM_H
#define MP_PARAM_H

#include <stddef.h>

// Lee titleId, contentVersion y el nombre: el titleName de localizedParameters[defaultLanguage] y,
// si no está, el de primer nivel. Los textos se cortan en un límite de carácter UTF-8. Un BOM de
// UTF-8 al principio y los espacios y NUL del final no cuentan. Devuelve 0, o -1 si no es JSON
// válido (con los tres vacíos).
int mp_param_read(const char *json, size_t n, char id[16], char ver[16], char name[128]);

#endif
