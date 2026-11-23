// SPDX-License-Identifier: GPL-3.0-or-later
// Packs zstd de ampr-emu-ng: ampr_zstd.index (AMPRZPK1) + ampr_zstd-NNN.pak (AMPRZDT1).
// Formato: docs/zstd-format.md de ampr-emu-ng y docs/zstd.md de ps5-dump-forge.
#ifndef MP_ZPK_H
#define MP_ZPK_H

#include <stddef.h>
#include <stdint.h>

#include "mp_measure.h"
#include "mp_parallel.h"
#include "mp_source.h"

#define MP_ZPK_VOLUME_LIMIT 4294901760ull   // 4 GiB - 64 KiB, cabecera incluida
#define MP_ZPK_MAX_VOLUMES 4096u
#define MP_ZPK_HEADER 128u
#define MP_ZPK_VOL_HEADER 64u

// Dónde van los volúmenes: una carpeta o regiones de un .p5z.
typedef struct {
    int (*vol_begin)(void *ctx, uint32_t vol, mp_err *e);   // empieza el volumen: 64 B de ceros
    int (*vol_write)(void *ctx, const void *p, size_t n, mp_err *e);
    int (*vol_end)(void *ctx, uint32_t vol, mp_err *e);
    int (*vol_header)(void *ctx, uint32_t vol, const uint8_t hdr[64], mp_err *e);   // al final
    void *ctx;
} mp_zpk_sink;

typedef struct {
    int level;              // 1..22
    unsigned threads;
    uint64_t volume_limit;  // MP_ZPK_VOLUME_LIMIT (más pequeño en pruebas)
    mp_progress *prog;
} mp_zpk_opts;

typedef struct {
    uint64_t files, chunks, volumes, logical, stored;
    uint8_t archive_id[16];
} mp_zpk_stats;

// files: índices de origen de los empaquetables. Devuelve el ampr_zstd.index (malloc) y en sha[i]
// el SHA-256 del contenido de files[i]. La salida no depende de o->threads.
int mp_zpk_write(mp_source *src, const uint32_t *files, size_t n, const mp_zpk_opts *o,
                 const mp_zpk_sink *sink, uint8_t **index, size_t *index_len, uint8_t (*sha)[32],
                 mp_zpk_stats *st, mp_err *e);

// Volúmenes como ampr_zstd-NNN.pak dentro de dir (que ya debe existir).
int mp_zpk_sink_dir(const char *dir, mp_zpk_sink *out, mp_err *e);
void mp_zpk_sink_free(mp_zpk_sink *s);

// ---- Lectura -----------------------------------------------------------------------------

typedef struct {
    int (*read)(void *ctx, uint32_t vol, uint64_t off, void *buf, size_t n, mp_err *e);
    uint64_t (*size)(void *ctx, uint32_t vol);   // 0 si no existe
    void *ctx;
} mp_zpk_volsrc;

typedef struct mp_zpk mp_zpk;
// Comprueba todo lo que exigen los documentos (CRC, tablas, orden, bloques seguidos, cabeceras
// de volumen). No decodifica los bloques: eso lo hace mp_zpk_read_file.
int mp_zpk_open(const uint8_t *index, size_t len, const mp_zpk_volsrc *vs, mp_zpk **out, mp_err *e);
size_t mp_zpk_count(const mp_zpk *z);
const char *mp_zpk_path(const mp_zpk *z, size_t i);
uint64_t mp_zpk_size(const mp_zpk *z, size_t i);
uint32_t mp_zpk_volume_count(const mp_zpk *z);
// Índice del archivo con esa ruta (los registros van ordenados por hash y ruta), o -1.
long mp_zpk_find(const mp_zpk *z, const char *path);
// Datos del bloque k del archivo i (cualquier puntero puede ser NULL). 0 si existe.
int mp_zpk_chunk(const mp_zpk *z, size_t i, uint32_t k, uint32_t *vol, uint64_t *off, uint32_t *stored,
                 int *codec);
// Lee [off, off+n) del archivo i decodificando sus bloques; comprueba tamaños y checksums.
int mp_zpk_read_file(mp_zpk *z, size_t i, uint64_t off, void *buf, size_t n, mp_err *e);
void mp_zpk_close(mp_zpk *z);

int mp_zpk_volsrc_dir(const char *dir, mp_zpk_volsrc *out, mp_err *e);   // abre bajo demanda
void mp_zpk_volsrc_free(mp_zpk_volsrc *vs);

#endif
