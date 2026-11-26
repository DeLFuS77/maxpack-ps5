# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(maxpack VERSION 1.0.6 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MAXPACK_PS5 "Build maxpack.elf for the PS5 (with the ps5-payload-dev SDK: prospero-cmake)" OFF)

include(cmake/ThirdParty.cmake)
find_package(Threads REQUIRED)

function(mp_warnings tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W4 /utf-8)
    target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra)
  endif()
endfunction()

file(GLOB MP_CORE_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.c)
add_library(maxpack_core STATIC ${MP_CORE_SRC})
target_include_directories(maxpack_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_link_libraries(maxpack_core PUBLIC mp_zstd mp_zlib Threads::Threads)
if(WIN32)
  target_link_libraries(maxpack_core PUBLIC shell32)
endif()
mp_warnings(maxpack_core)

file(GLOB MP_ANALYZE_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/analyze/*.c)
add_library(maxpack_analyze STATIC ${MP_ANALYZE_SRC})
target_include_directories(maxpack_analyze PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/analyze)
target_link_libraries(maxpack_analyze PUBLIC maxpack_core)
# libm separately only where it exists (on the PS5 the math functions are in libc).
find_library(MP_LIBM m)
if(MP_LIBM)
  target_link_libraries(maxpack_analyze PUBLIC ${MP_LIBM})
endif()
mp_warnings(maxpack_analyze)

file(GLOB MP_PACK_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/pack/*.c)
add_library(maxpack_pack STATIC ${MP_PACK_SRC})
target_include_directories(maxpack_pack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/pack)
target_link_libraries(maxpack_pack PUBLIC maxpack_analyze)
mp_warnings(maxpack_pack)

# Minimal networking (phase 2): HTTP server and client.
file(GLOB MP_NET_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.c)
add_library(maxpack_net STATIC ${MP_NET_SRC})
target_include_directories(maxpack_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/net)
target_link_libraries(maxpack_net PUBLIC maxpack_core)
if(WIN32)
  target_link_libraries(maxpack_net PUBLIC ws2_32)
endif()
mp_warnings(maxpack_net)

# Installer (phase 2): portable engine. Dependencies: maxpack_web -> maxpack_install -> maxpack_pack.
file(GLOB MP_INSTALL_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/install/*.c)
add_library(maxpack_install STATIC ${MP_INSTALL_SRC})
target_include_directories(maxpack_install PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/install)
target_link_libraries(maxpack_install PUBLIC maxpack_pack maxpack_net)
mp_warnings(maxpack_install)

# The installer's web (phase 2): the JSON API.
file(GLOB MP_WEB_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/web/mp_*.c)
add_library(maxpack_web STATIC ${MP_WEB_SRC})
target_include_directories(maxpack_web PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/web)
target_link_libraries(maxpack_web PUBLIC maxpack_install)
mp_warnings(maxpack_web)

# The .p5z icon reader of the file manager thumbnails: only zstd decompression and zlib's crc32 (the
# Windows thumbnail provider builds the same sources with its own copy of those, see below).
add_library(maxpack_thumb STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/thumb/mp_p5z_icon.c)
target_include_directories(maxpack_thumb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/thumb)
target_link_libraries(maxpack_thumb PUBLIC mp_zstd mp_zlib)
mp_warnings(maxpack_thumb)

# Windows: maxpack-thumbs.dll, the Explorer's thumbnail provider of .p5z files (src/thumb/win). It carries
# its C runtime, since the Explorer may load it where none is installed, so it builds the icon reader with
# its own copy of zstd's decompression and zlib's crc32, with that runtime; it exports only what COM needs.
if(WIN32)
  file(GLOB MP_THUMB_ZSTD_SRC ${MP_ZSTD_DIR}/common/*.c ${MP_ZSTD_DIR}/decompress/*.c)
  add_library(mp_thumb_deps OBJECT ${MP_THUMB_ZSTD_SRC} ${MP_ZLIB_DIR}/crc32.c)
  target_include_directories(mp_thumb_deps PRIVATE ${MP_ZSTD_DIR} ${MP_ZLIB_DIR})
  target_compile_definitions(mp_thumb_deps PRIVATE ZSTD_DISABLE_ASM XXH_NAMESPACE=ZSTD_)
  mp_quiet(mp_thumb_deps)
  add_library(maxpack-thumbs SHARED ${CMAKE_CURRENT_SOURCE_DIR}/src/thumb/win/maxpack_thumbs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thumb/win/maxpack_thumbs.def ${CMAKE_CURRENT_SOURCE_DIR}/src/thumb/mp_p5z_icon.c
    $<TARGET_OBJECTS:mp_thumb_deps>)
  target_include_directories(maxpack-thumbs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/thumb ${MP_ZSTD_DIR} ${MP_ZLIB_DIR})
  target_link_libraries(maxpack-thumbs PRIVATE ole32 windowscodecs)
  set_target_properties(mp_thumb_deps maxpack-thumbs PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_target_properties(maxpack-thumbs PROPERTIES PREFIX "")
  mp_warnings(maxpack-thumbs)
endif()

if(MAXPACK_PS5)
  # maxpack.elf: the phase 2 installer. It is no longer published nor built by this
  # repository's CI: the console payload (DeLFuS77/maxpack-ps5) replaces it and uses only the
  # libraries from here (maxpack_web, maxpack_install, ...). The target remains to test those libraries
  # with the SDK. With -DMAXPACK_RUNTIME=<libSceAmpr.sprx> it carries inside
  # that ampr-emu-ng (and -DMAXPACK_RUNTIME_VERSION=<text> to show it).
  set(MAXPACK_RUNTIME "" CACHE FILEPATH "ampr-emu-ng's libSceAmpr.sprx that the installer carries")
  set(MAXPACK_RUNTIME_VERSION "" CACHE STRING "Version of that runtime")
  set(MP_PS5_SRC src/ps5/main_ps5.c)
  if(MAXPACK_RUNTIME)
    set(MP_RT_C ${CMAKE_BINARY_DIR}/gen/mp_runtime.c)
    add_custom_command(OUTPUT ${MP_RT_C}
      COMMAND ${CMAKE_COMMAND} -DIN=${MAXPACK_RUNTIME} -DOUT=${MP_RT_C} -DNAME=mp_runtime_sprx
              -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
      DEPENDS ${MAXPACK_RUNTIME} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
      COMMENT "Embedding the runtime")
    list(APPEND MP_PS5_SRC ${MP_RT_C})
    file(TIMESTAMP ${MAXPACK_RUNTIME} MP_RT_MTIME "%s" UTC)
  endif()
  add_executable(maxpack_elf ${MP_PS5_SRC})
  set_target_properties(maxpack_elf PROPERTIES OUTPUT_NAME maxpack.elf)
  target_link_libraries(maxpack_elf PRIVATE maxpack_web SceNotification)
  if(MAXPACK_RUNTIME)
    target_compile_definitions(maxpack_elf PRIVATE MAXPACK_HAVE_RUNTIME MAXPACK_RUNTIME_MTIME=${MP_RT_MTIME}
                               MAXPACK_RUNTIME_VERSION="${MAXPACK_RUNTIME_VERSION}")
  endif()
  mp_warnings(maxpack_elf)
  set(MAXPACK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
else()
  # PC installer, only to test the engine and its JSON API without a console (the page is the console payload's).
  add_executable(maxpack-installer-pc src/web/main_pc.c)
  target_link_libraries(maxpack-installer-pc PRIVATE maxpack_web)
  mp_warnings(maxpack-installer-pc)
endif()

if(NOT MAXPACK_PS5)
  add_executable(maxpack src/cli/main.c)
  target_link_libraries(maxpack PRIVATE maxpack_pack)
  mp_warnings(maxpack)
endif()

option(MAXPACK_BUILD_TESTS "Build the tests" ON)
if(MAXPACK_BUILD_TESTS)
  enable_testing()
  find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)
  set(MP_FIXTURES_DIR ${CMAKE_BINARY_DIR}/fixtures)
  set(MP_TEST_TMP ${CMAKE_BINARY_DIR}/tmp)

  # mp_add_test(<name> [FIXTURES|IMAGES|FPKG]): builds tests/<name>.c and registers it with CTest
  # (IMAGES: it also needs the UFS2, PFS and PFSC fixtures; FPKG: the PS5 packages).
  function(mp_add_test name)
    cmake_parse_arguments(T "FIXTURES;IMAGES;FPKG" "" "" ${ARGN})
    add_executable(${name} tests/${name}.c)
    target_link_libraries(${name} PRIVATE maxpack_web)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(${name} PRIVATE
      MP_TEST_TMP="${MP_TEST_TMP}" MP_FIXTURES_DIR="${MP_FIXTURES_DIR}")
    mp_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 300)
    if(T_FPKG)
      set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "exfat;fpkg")
    elseif(T_IMAGES)
      set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "exfat;images")
    elseif(T_FIXTURES)
      set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED exfat)
    endif()
  endfunction()

  # Synthetic exFAT fixtures (Python) and comparison with the reference reader.
  add_test(NAME gen_fixtures COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/make_exfat.py --out ${MP_FIXTURES_DIR})
  set_tests_properties(gen_fixtures PROPERTIES FIXTURES_SETUP exfat)
  add_test(NAME check_ref COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/check_ref.py ${MP_FIXTURES_DIR})
  set_tests_properties(check_ref PROPERTIES FIXTURES_REQUIRED exfat)
  # UFS2 (.ffpkg), PFS (.ffpfs) and PFSC (.ffpfsc) fixtures, and the real tools' images of tests/data.
  add_test(NAME gen_images COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/make_images.py --out ${MP_FIXTURES_DIR}
    --data ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)
  set_tests_properties(gen_images PROPERTIES FIXTURES_SETUP images)
  # PS5 debug packages (.pkg) and Kraken blocks, encrypted with the engine's AES (mp_fpkgtool).
  add_test(NAME gen_fpkg COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/make_fpkg.py --out ${MP_FIXTURES_DIR} --xts $<TARGET_FILE:mp_fpkgtool>)
  set_tests_properties(gen_fpkg PROPERTIES FIXTURES_SETUP fpkg TIMEOUT 600)
  # Dumps with a list of the original package's files (sce_sys/playgo-hash-table.dat).
  add_test(NAME gen_playgo COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/make_playgo.py --out ${MP_FIXTURES_DIR})
  set_tests_properties(gen_playgo PROPERTIES FIXTURES_SETUP playgo)

  mp_add_test(test_smoke)
  mp_add_test(test_platform)
  mp_add_test(test_source_dir)
  mp_add_test(test_extent)
  mp_add_test(test_source_exfat FIXTURES)
  set_tests_properties(test_source_exfat PROPERTIES TIMEOUT 10)
  mp_add_test(test_source_images IMAGES)
  set_tests_properties(test_source_images PROPERTIES TIMEOUT 300)
  mp_add_test(test_source_fpkg FPKG)
  set_tests_properties(test_source_fpkg PROPERTIES TIMEOUT 600)
  mp_add_test(test_dlc FPKG)
  set_tests_properties(test_dlc PROPERTIES TIMEOUT 600 FIXTURES_REQUIRED "exfat;fpkg;playgo")
  mp_add_test(test_rules)
  mp_add_test(test_sample)
  mp_add_test(test_measure)
  mp_add_test(test_parallel)
  mp_add_test(test_readahead FIXTURES)
  mp_add_test(test_dedup FIXTURES)
  mp_add_test(test_v2exp FIXTURES)
  mp_add_test(test_analyze FIXTURES)
  mp_add_test(test_levels FIXTURES)
  mp_add_test(test_bench)
  mp_add_test(test_report)
  mp_add_test(test_hash)
  mp_add_test(test_crypto)
  mp_add_test(test_kraken FPKG)
  mp_add_test(test_wfile)
  mp_add_test(test_fsops)
  mp_add_test(test_json)
  mp_add_test(test_inst_scan FIXTURES)
  mp_add_test(test_inst_install FIXTURES)
  mp_add_test(test_inst_resume FIXTURES)
  mp_add_test(test_inst_replace FIXTURES)
  mp_add_test(test_inst_manage FIXTURES)
  mp_add_test(test_inst_copy FIXTURES)
  mp_add_test(test_http)
  mp_add_test(test_smp)
  mp_add_test(test_api FIXTURES)
  mp_add_test(test_api_call FIXTURES)
  mp_add_test(test_idx3 FIXTURES)
  set_tests_properties(test_idx3 PROPERTIES FIXTURES_SETUP idx3)
  add_test(NAME check_idx3 COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/check_idx3.py ${MP_TEST_TMP}/idx3)
  set_tests_properties(check_idx3 PROPERTIES FIXTURES_REQUIRED idx3)
  mp_add_test(test_zpk FIXTURES)
  set_tests_properties(test_zpk PROPERTIES FIXTURES_SETUP zpk)
  add_test(NAME check_zpk COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ampr_ref.py zpk-check ${MP_TEST_TMP}/zpk/roundtrip
    ${MP_FIXTURES_DIR}/basic.exfat)
  set_tests_properties(check_zpk PROPERTIES FIXTURES_REQUIRED "exfat;zpk")
  add_test(NAME check_zpk_multi COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ampr_ref.py zpk-check ${MP_TEST_TMP}/zpk/multi
    ${MP_FIXTURES_DIR}/basic.exfat)
  set_tests_properties(check_zpk_multi PROPERTIES FIXTURES_REQUIRED "exfat;zpk")
  mp_add_test(test_pack FIXTURES)
  set_tests_properties(test_pack PROPERTIES TIMEOUT 600)
  mp_add_test(test_p5z FIXTURES)
  mp_add_test(test_unpack FIXTURES)
  mp_add_test(test_describe FIXTURES)
  mp_add_test(test_p5z_icon)
  target_link_libraries(test_p5z_icon PRIVATE maxpack_thumb)
  # the DLL as the Explorer loads it: mp_thumbtest <maxpack-thumbs.dll>
  if(WIN32)
    add_executable(mp_thumbtest tests/win/thumbtest.c)
    target_link_libraries(mp_thumbtest PRIVATE maxpack_web ole32 shlwapi)
    target_include_directories(mp_thumbtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(mp_thumbtest PRIVATE MP_TEST_TMP="${MP_TEST_TMP}" MP_FIXTURES_DIR="${MP_FIXTURES_DIR}")
    mp_warnings(mp_thumbtest)
    add_dependencies(mp_thumbtest maxpack-thumbs)
    add_test(NAME mp_thumbtest COMMAND mp_thumbtest $<TARGET_FILE:maxpack-thumbs>)
    set_tests_properties(mp_thumbtest PROPERTIES TIMEOUT 300)
  endif()
  mp_add_test(test_play FIXTURES)
  mp_add_test(test_grow FPKG)
  target_link_libraries(test_grow PRIVATE maxpack_thumb)
  set_tests_properties(test_grow PROPERTIES TIMEOUT 600)
  mp_add_test(test_volumes FIXTURES)
  mp_add_test(test_image_pack IMAGES)
  set_tests_properties(test_image_pack PROPERTIES TIMEOUT 600 FIXTURES_REQUIRED "exfat;images;fpkg")
  mp_add_test(test_identity FIXTURES)
  set_tests_properties(test_identity PROPERTIES TIMEOUT 600)
  mp_add_test(test_cancel FIXTURES)
  mp_add_test(test_verify FIXTURES)
  mp_add_test(test_malformed FIXTURES)
  mp_add_test(test_complete FIXTURES)
  set_tests_properties(test_complete PROPERTIES FIXTURES_REQUIRED "exfat;playgo")
  add_test(NAME test_cli COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_cli.py $<TARGET_FILE:maxpack> ${MP_FIXTURES_DIR}
    ${MP_TEST_TMP}/cli)
  set_tests_properties(test_cli PROPERTIES FIXTURES_REQUIRED "exfat;images;fpkg;playgo" TIMEOUT 300)

  # A package's version comes from the tag (v1.2.3 -> 1.2.3); needs bash.
  find_program(MP_BASH bash)
  if(MP_BASH)
    add_test(NAME test_version COMMAND ${Python3_EXECUTABLE} -I
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_version.py ${MP_BASH}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/version.sh ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
  endif()

  # Test tool: dumps a range of a source's file to stdout.
  add_executable(mp_cat tests/tools/mp_cat.c)
  target_link_libraries(mp_cat PRIVATE maxpack_core)
  mp_warnings(mp_cat)

  # Test tool for make_fpkg.py: encrypts the packages it writes with the engine's AES.
  add_executable(mp_fpkgtool tests/tools/mp_fpkgtool.c)
  target_link_libraries(mp_fpkgtool PRIVATE maxpack_core)
  mp_warnings(mp_fpkgtool)

  # Benchmark tool: reads a source's packable files the way the packer does and prints how fast.
  add_executable(mp_readbench tests/tools/mp_readbench.c)
  target_link_libraries(mp_readbench PRIVATE maxpack_pack)
  mp_warnings(mp_readbench)
endif()
