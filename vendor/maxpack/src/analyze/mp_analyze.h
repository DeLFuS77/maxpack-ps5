// SPDX-License-Identifier: GPL-3.0-or-later
// maxpack analyze: mide cuánto gana cada técnica sobre un juego, sin escribir nada en él.
#ifndef MP_ANALYZE_H
#define MP_ANALYZE_H

#include <stddef.h>
#include <stdint.h>

#include "mp_dedup.h"
#include "mp_measure.h"
#include "mp_source.h"
#include "mp_v2exp.h"

typedef struct {
    double sample_fraction;   // 0.02
    uint64_t sample_min;      // 1 GiB
    int full;                 // medir todo, sin muestra
    unsigned threads;
    int dedup;                // pase completo de deduplicación
    int transport;            // medir el .p5z de transporte (zstd 19, ventana larga)
    int progress;             // progreso en stderr
    const char *const *keep_loose;
    size_t nkeep;
} mp_analyze_opts;

void mp_analyze_opts_default(mp_analyze_opts *o);

typedef struct {
    uint64_t lo, hi, files, bytes;
} mp_size_bin;

// Transporte: lo empaquetable comprimido como en un .p5z solo para el PC (zstd 19, ventana larga,
// frames de 32 MiB). La PS5 tendría que recomprimirlo a v1 al instalar.
typedef struct {
    uint64_t frames, sample_bytes, sample_raw, sample_stored, est_packed, ns, est_seconds;
    double rate;
    unsigned threads;
} mp_xport_stat;

#define MP_SIZE_BINS 8   // [0] [1,4K] [4K+1,64K] [64K+1,256K] [256K+1,1M] [1M+1,64M] [64M+1,4G] [4G+1,∞)

typedef struct {
    char *source_name;   // solo el nombre, no la ruta
    mp_source_kind kind;
    uint64_t files, dirs, total_bytes;
    uint64_t loose_files, loose_bytes, packable_files, packable_bytes;
    mp_size_bin bins[MP_SIZE_BINS];
    uint64_t sample_bytes, sample_windows;
    double sample_rate;
    unsigned threads;
    int full;
    mp_cfg_stat sampled[MP_CFG_COUNT];
    uint64_t est_packed[MP_CFG_COUNT];    // extrapolado a todo lo empaquetable
    uint64_t est_seconds[MP_CFG_COUNT];   // tiempo estimado de empaquetar con `threads` hilos
    uint64_t v1_index_bytes;              // AMPRZPK1 + AMPRIDX3 + cabeceras de volumen
    uint64_t loose_compressed;            // sueltos con zstd 19, ventana larga, frames de 32 MiB
    uint64_t p5z_estimate;
    int block_dicts;
    mp_small_result small;
    int dedup_done;
    mp_dedup_result dedup;
    int xport_done;
    mp_xport_stat xport;
    uint64_t p5z_transport;               // transporte + sueltos comprimidos
    uint64_t elapsed_ns;
} mp_report;

int mp_analyze(const char *src_path, const mp_analyze_opts *o, mp_report *r, mp_err *e);
void mp_report_free(mp_report *r);

#endif
