# SPDX-License-Identifier: GPL-3.0-or-later
# cmake -DIN=<archivo> -DOUT=<salida.c> -DNAME=<símbolo> -P embed.cmake
# Escribe un .c con el contenido de IN como array de bytes (acabado en NUL) y su longitud.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR len "${hexlen} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}"
  "// Generado por cmake/embed.cmake a partir de ${IN}. No editar.\n"
  "#include <stddef.h>\n"
  "const unsigned char ${NAME}[] = {${bytes}0x00};\n"
  "const size_t ${NAME}_len = ${len};\n")
