# SPDX-License-Identifier: GPL-3.0-or-later
# zstd and zlib are built with this CMake, not their own: zlib's renames zconf.h
# inside the submodule and zstd's drags in options that are not needed here.

set(MP_ZSTD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib)
set(MP_ZLIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib)

if(NOT EXISTS ${MP_ZSTD_DIR}/zstd.h OR NOT EXISTS ${MP_ZLIB_DIR}/zlib.h)
  message(FATAL_ERROR "The submodules are missing: run 'git submodule update --init'")
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
# Without assembly (the .S does not compile with MSVC) and without internal threads (we distribute the work ourselves).
target_compile_definitions(mp_zstd PRIVATE ZSTD_DISABLE_ASM XXH_NAMESPACE=ZSTD_)
mp_quiet(mp_zstd)

set(MP_ZLIB_SRC adler32.c compress.c crc32.c deflate.c inflate.c inftrees.c inffast.c
                trees.c uncompr.c zutil.c)
list(TRANSFORM MP_ZLIB_SRC PREPEND ${MP_ZLIB_DIR}/)
add_library(mp_zlib STATIC ${MP_ZLIB_SRC})
target_include_directories(mp_zlib PUBLIC ${MP_ZLIB_DIR})
mp_quiet(mp_zlib)
