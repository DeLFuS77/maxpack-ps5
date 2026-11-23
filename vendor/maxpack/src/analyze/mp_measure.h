// SPDX-License-Identifier: GPL-3.0-or-later
// Comprime una ventana de datos con cada configuración del analizador y suma los tamaños.
#ifndef MP_MEASURE_H
#define MP_MEASURE_H

#include <stddef.h>
#include <stdint.h>
#include <zstd.h>

#define MP_BLOCK_V1 262144u   // bloque de AMPRZPK1 (2^18)
#define MP_BLOCK_ZLIB 65536u  // bloque de .ffpfsc
#define MP_WINDOW_MAX (1u << 20)   // ventana máxima que se mide de una vez
#define MP_LONG_FRAME (32u << 20)  // frames del .p5z (diseño 6.4: hasta 32 MiB)
#define MP_LONG_MAX_THREADS 8u     // cada contexto de ventana larga ocupa ~120 MB

typedef enum {
    MP_CFG_V1_L5,     // 256 KiB, windowLog 18 (lo que usa Dump Forge por defecto)
    MP_CFG_V1_L9,
    MP_CFG_V1_L19,
    MP_CFG_V1_L22,
    MP_CFG_1M_L19,    // bloque = ventana de 1 MiB, windowLog 20
    MP_CFG_DICT_L19,  // 256 KiB, windowLog 18, con diccionario
    MP_CFG_ZLIB64K,   // zlib nivel 9 en bloques de 64 KiB (≈ .ffpfsc)
    MP_CFG_COUNT
} mp_cfg;

// "v1-l5" "v1-l9" "v1-l19" "v1-l22" "1m-l19" "dict-l19" "zlib-64k"
const char *mp_cfg_name(mp_cfg c);
// 1 si la configuración produce packs v1 compatibles con ampr-emu-ng.
int mp_cfg_is_v1(mp_cfg c);

typedef struct {
    uint64_t raw, stored, ns;
} mp_cfg_stat;

// dict[p] se entrenó con unidades de paridad p y se usa para evaluar las de paridad 1 - p.
typedef struct {
    void *dict[2];
    size_t size[2];
} mp_dict_pair;

typedef struct mp_measurer mp_measurer;   // uno por hilo

mp_measurer *mp_measurer_new(const mp_dict_pair *dicts);   // dicts puede ser NULL
void mp_measurer_free(mp_measurer *m);
// len <= 1 MiB. Suma raw/stored/ns de cada configuración en st[]. En cada bloque se guarda
// el frame solo si es estrictamente menor que el bloque en bruto.
void mp_measure_window(mp_measurer *m, const uint8_t *data, size_t len, unsigned parity,
                       mp_cfg_stat st[MP_CFG_COUNT]);

// zstd 19 con ventana larga (2^25) y LDM, como el .p5z (diseño 6.4).
void mp_zstd_set_long(ZSTD_CCtx *c);

// Un frame zstd con checksum y tamaño de contenido. Reinicia sesión y parámetros.
// Devuelve el tamaño del frame o 0 si falla.
size_t mp_zstd_block(ZSTD_CCtx *c, int level, int window_log, const void *src, size_t n, void *dst,
                     size_t cap);

#endif
