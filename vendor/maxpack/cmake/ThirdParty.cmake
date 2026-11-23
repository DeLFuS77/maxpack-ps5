# SPDX-License-Identifier: GPL-3.0-or-later
# zstd y zlib se compilan con este CMake, no con los suyos: el de zlib renombra zconf.h
# dentro del submódulo y el de zstd arrastra opciones que aquí no hacen falta.

set(MP_ZSTD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib)
set(MP_ZLIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib)

if(NOT EXISTS ${MP_ZSTD_DIR}/zstd.h OR NOT EXISTS ${MP_ZLIB_DIR}/zlib.h)
  message(FATAL_ERROR "Faltan los submódulos: ejecuta 'git submodule update --init'")
endif()

function(mp_quiet tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W0)
  else()
    target_compile_options(${tgt} PRIVATE -w)
  endif()
endfunction()

file(GLOB MP_ZSTD_SRC
  ${MP_ZSTD_DIR}/common/*.c
  ${MP_ZSTD_DIR}/compress/*.c
  ${MP_ZSTD_DIR}/decompress/*.c
  ${MP_ZSTD_DIR}/dictBuilder/*.c)
add_library(mp_zstd STATIC ${MP_ZSTD_SRC})
target_include_directories(mp_zstd PUBLIC ${MP_ZSTD_DIR})
# Sin ensamblador (el .S no compila con MSVC) y sin hilos internos (repartimos nosotros).
target_compile_definitions(mp_zstd PRIVATE ZSTD_DISABLE_ASM XXH_NAMESPACE=ZSTD_)
mp_quiet(mp_zstd)

set(MP_ZLIB_SRC adler32.c compress.c crc32.c deflate.c inflate.c inftrees.c inffast.c
                trees.c uncompr.c zutil.c)
list(TRANSFORM MP_ZLIB_SRC PREPEND ${MP_ZLIB_DIR}/)
add_library(mp_zlib STATIC ${MP_ZLIB_SRC})
target_include_directories(mp_zlib PUBLIC ${MP_ZLIB_DIR})
mp_quiet(mp_zlib)
