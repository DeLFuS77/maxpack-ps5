// SPDX-License-Identifier: GPL-3.0-or-later
// Deduplicación exacta (mejora M4): lee todo lo empaquetable y cuenta bloques y archivos repetidos.
#ifndef MP_DEDUP_H
#define MP_DEDUP_H

#include <stddef.h>
#include <stdint.h>

#include "mp_parallel.h"
#include "mp_source.h"

typedef struct {
    uint64_t blocks, dup_blocks, dup_block_bytes;   // bloques de 256 KiB alineados al inicio del archivo
    uint64_t dup_files, dup_file_bytes;             // archivos idénticos a otro anterior en orden de ruta
    uint64_t bytes_read, ns;
} mp_dedup_result;

// files: índices de origen de los archivos empaquetables, en orden de ruta.
int mp_dedup_run(mp_source *src, const uint32_t *files, size_t nfiles, unsigned threads,
                 mp_progress *prog, mp_dedup_result *out, mp_err *e);

#endif
