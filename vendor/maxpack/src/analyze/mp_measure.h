// SPDX-License-Identifier: GPL-3.0-or-later
// Compresses a window of data with each of the analyzer's configurations and adds up the sizes.
#ifndef MP_MEASURE_H
#define MP_MEASURE_H

#include <stddef.h>
#include <stdint.h>
#include <zstd.h>

#define MP_BLOCK_V1 262144u   // AMPRZPK1 block (2^18)
#define MP_BLOCK_ZLIB 65536u  // .ffpfsc block
#define MP_WINDOW_MAX (1u << 20)   // maximum window measured at once
#define MP_LONG_FRAME (32u << 20)  // .p5z frames (design 6.4: up to 32 MiB)
#define MP_LONG_MAX_THREADS 8u     // each long-window context takes ~120 MB

typedef enum {
    MP_CFG_V1_L5,     // 256 KiB, windowLog 18 (what Dump Forge uses by default)
    MP_CFG_V1_L9,
    MP_CFG_V1_L19,
    MP_CFG_V1_L22,
    MP_CFG_1M_L19,    // block = 1 MiB window, windowLog 20
    MP_CFG_DICT_L19,  // 256 KiB, windowLog 18, with dictionary
    MP_CFG_ZLIB64K,   // zlib level 9 in 64 KiB blocks (≈ .ffpfsc)
    MP_CFG_COUNT
} mp_cfg;

// "v1-l5" "v1-l9" "v1-l19" "v1-l22" "1m-l19" "dict-l19" "zlib-64k"
const char *mp_cfg_name(mp_cfg c);
// 1 if the configuration produces v1 packs compatible with ampr-emu-ng.
int mp_cfg_is_v1(mp_cfg c);

typedef struct {
    uint64_t raw, stored, ns;
} mp_cfg_stat;

// dict[p] was trained with units of parity p and is used to evaluate those of parity 1 - p.
typedef struct {
    void *dict[2];
    size_t size[2];
} mp_dict_pair;

typedef struct mp_measurer mp_measurer;   // one per thread

mp_measurer *mp_measurer_new(const mp_dict_pair *dicts);   // dicts may be NULL
void mp_measurer_free(mp_measurer *m);
// len <= 1 MiB. Adds raw/stored/ns of each configuration into st[]. In each block the frame
// is kept only if it is strictly smaller than the raw block.
void mp_measure_window(mp_measurer *m, const uint8_t *data, size_t len, unsigned parity,
                       mp_cfg_stat st[MP_CFG_COUNT]);

// zstd 19 with a long window (2^25) and LDM, like the .p5z (design 6.4).
void mp_zstd_set_long(ZSTD_CCtx *c);

// A zstd frame with checksum and content size. Resets session and parameters.
// Returns the frame size or 0 on failure.
size_t mp_zstd_block(ZSTD_CCtx *c, int level, int window_log, const void *src, size_t n, void *dst,
                     size_t cap);

#endif
