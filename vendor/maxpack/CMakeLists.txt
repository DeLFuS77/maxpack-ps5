# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(maxpack VERSION 0.0.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MAXPACK_PS5 "Compilar maxpack.elf para la PS5 (con el SDK ps5-payload-dev: prospero-cmake)" OFF)

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
# libm aparte solo donde existe (en la PS5 las funciones matemáticas van en la libc).
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

# Red mínima (fase 2): servidor y cliente HTTP.
file(GLOB MP_NET_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.c)
add_library(maxpack_net STATIC ${MP_NET_SRC})
target_include_directories(maxpack_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/net)
target_link_libraries(maxpack_net PUBLIC maxpack_core)
if(WIN32)
  target_link_libraries(maxpack_net PUBLIC ws2_32)
endif()
mp_warnings(maxpack_net)

# Instalador (fase 2): motor portable. Dependencias: maxpack_web -> maxpack_install -> maxpack_pack.
file(GLOB MP_INSTALL_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/install/*.c)
add_library(maxpack_install STATIC ${MP_INSTALL_SRC})
target_include_directories(maxpack_install PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/install)
target_link_libraries(maxpack_install PUBLIC maxpack_pack maxpack_net)
mp_warnings(maxpack_install)

# Web del instalador (fase 2): API y página.
file(GLOB MP_WEB_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/web/mp_*.c)
add_library(maxpack_web STATIC ${MP_WEB_SRC})
target_include_directories(maxpack_web PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/web)
target_link_libraries(maxpack_web PUBLIC maxpack_install)
mp_warnings(maxpack_web)

# La página del instalador, incrustada en el binario.
set(MP_PAGE_C ${CMAKE_BINARY_DIR}/gen/mp_page.c)
add_custom_command(OUTPUT ${MP_PAGE_C}
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/web/index.html -DOUT=${MP_PAGE_C} -DNAME=mp_page_html
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  COMMENT "Incrustando web/index.html")
add_library(maxpack_page STATIC ${MP_PAGE_C})
target_include_directories(maxpack_page PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/web)
target_link_libraries(maxpack_web PUBLIC maxpack_page)

if(MAXPACK_PS5)
  # maxpack.elf: el instalador en la consola. Con -DMAXPACK_RUNTIME=<libSceAmpr.sprx> lleva dentro
  # ese ampr-emu-ng (y -DMAXPACK_RUNTIME_VERSION=<texto> para mostrarlo).
  set(MAXPACK_RUNTIME "" CACHE FILEPATH "libSceAmpr.sprx de ampr-emu-ng que lleva el instalador")
  set(MAXPACK_RUNTIME_VERSION "" CACHE STRING "Versión de ese runtime")
  set(MP_PS5_SRC src/ps5/main_ps5.c)
  if(MAXPACK_RUNTIME)
    set(MP_RT_C ${CMAKE_BINARY_DIR}/gen/mp_runtime.c)
    add_custom_command(OUTPUT ${MP_RT_C}
      COMMAND ${CMAKE_COMMAND} -DIN=${MAXPACK_RUNTIME} -DOUT=${MP_RT_C} -DNAME=mp_runtime_sprx
              -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
      DEPENDS ${MAXPACK_RUNTIME} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
      COMMENT "Incrustando el runtime")
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
  # Instalador de PC, solo para probar el motor y la página sin consola.
  add_executable(maxpack-installer-pc src/web/main_pc.c)
  target_link_libraries(maxpack-installer-pc PRIVATE maxpack_web)
  mp_warnings(maxpack-installer-pc)
endif()

if(NOT MAXPACK_PS5)
  add_executable(maxpack src/cli/main.c)
  target_link_libraries(maxpack PRIVATE maxpack_pack)
  mp_warnings(maxpack)
endif()

option(MAXPACK_BUILD_TESTS "Compilar las pruebas" ON)
if(MAXPACK_BUILD_TESTS)
  enable_testing()
  find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)
  set(MP_FIXTURES_DIR ${CMAKE_BINARY_DIR}/fixtures)
  set(MP_TEST_TMP ${CMAKE_BINARY_DIR}/tmp)

  # mp_add_test(<nombre> [FIXTURES]): compila tests/<nombre>.c y lo registra en CTest.
  function(mp_add_test name)
    cmake_parse_arguments(T "FIXTURES" "" "" ${ARGN})
    add_executable(${name} tests/${name}.c)
    target_link_libraries(${name} PRIVATE maxpack_web)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(${name} PRIVATE
      MP_TEST_TMP="${MP_TEST_TMP}" MP_FIXTURES_DIR="${MP_FIXTURES_DIR}")
    mp_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 300)
    if(T_FIXTURES)
      set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED exfat)
    endif()
  endfunction()

  # Fixtures exFAT sintéticas (Python) y contraste del lector de referencia.
  add_test(NAME gen_fixtures COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/make_exfat.py --out ${MP_FIXTURES_DIR})
  set_tests_properties(gen_fixtures PROPERTIES FIXTURES_SETUP exfat)
  add_test(NAME check_ref COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/check_ref.py ${MP_FIXTURES_DIR})
  set_tests_properties(check_ref PROPERTIES FIXTURES_REQUIRED exfat)

  mp_add_test(test_smoke)
  mp_add_test(test_platform)
  mp_add_test(test_source_dir)
  mp_add_test(test_extent)
  mp_add_test(test_source_exfat FIXTURES)
  set_tests_properties(test_source_exfat PROPERTIES TIMEOUT 10)
  mp_add_test(test_rules)
  mp_add_test(test_sample)
  mp_add_test(test_measure)
  mp_add_test(test_parallel)
  mp_add_test(test_dedup FIXTURES)
  mp_add_test(test_v2exp FIXTURES)
  mp_add_test(test_analyze FIXTURES)
  mp_add_test(test_report)
  mp_add_test(test_hash)
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
  mp_add_test(test_page)
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
  mp_add_test(test_cancel FIXTURES)
  mp_add_test(test_verify FIXTURES)
  mp_add_test(test_malformed FIXTURES)
  add_test(NAME test_cli COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_cli.py $<TARGET_FILE:maxpack> ${MP_FIXTURES_DIR}
    ${MP_TEST_TMP}/cli)
  set_tests_properties(test_cli PROPERTIES FIXTURES_REQUIRED exfat TIMEOUT 300)

  # Herramienta de prueba: vuelca un rango de un archivo de un origen a stdout.
  add_executable(mp_cat tests/tools/mp_cat.c)
  target_link_libraries(mp_cat PRIVATE maxpack_core)
  mp_warnings(mp_cat)
endif()
