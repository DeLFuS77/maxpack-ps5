// SPDX-License-Identifier: GPL-3.0-or-later
// maxpack analyze: measures how much each technique gains on a game, without writing anything to it.
#ifndef MP_ANALYZE_H
#define MP_ANALYZE_H

#include <stddef.h>
#include <stdint.h>

#include "mp_complete.h"
#include "mp_dedup.h"
#include "mp_measure.h"
#include "mp_source.h"
#include "mp_v2exp.h"

typedef struct {
    double sample_fraction;   // 0.02
    uint64_t sample_min;      // 1 GiB
    int full;                 // measure everything, no sample
    unsigned threads;
    int dedup;                // full deduplication pass
    int transport;            // measure the transport .p5z (zstd 19, long window)
    int progress;             // progress on stderr
    const char *const *keep_loose;
    size_t nkeep;
} mp_analyze_opts;

void mp_analyze_opts_default(mp_analyze_opts *o);

typedef struct {
    uint64_t lo, hi, files, bytes;
} mp_size_bin;

// Transport: the packable files compressed as in a .p5z only for the PC (zstd 19, long window,
// 32 MiB frames). The PS5 would have to recompress it to v1 on install.
typedef struct {
    uint64_t frames, sample_bytes, sample_raw, sample_stored, est_packed, ns, est_seconds;
    double rate;
    unsigned threads;
} mp_xport_stat;

#define MP_SIZE_BINS 8   // [0] [1,4K] [4K+1,64K] [64K+1,256K] [256K+1,1M] [1M+1,64M] [64M+1,4G] [4G+1,∞)

typedef struct {
    char *source_name;   // only the name, not the path
    mp_source_kind kind;
    char kind_name[8], inner_name[8];   // mp_source_kind_name and mp_source_inner_name
    char kind_text[64];                 // mp_source_kind_text ("" = from kind)
    uint64_t image_size;                // the file of a compressed image (PFSC) or of a package, 0 for
                                        // anything else: it is not the game's size, so the text
                                        // compares with it too
    uint64_t files, dirs, total_bytes;
    uint64_t loose_files, loose_bytes, packable_files, packable_bytes;
    mp_size_bin bins[MP_SIZE_BINS];
    uint64_t sample_bytes, sample_windows;
    double sample_rate;
    unsigned threads;
    int full;
    mp_cfg_stat sampled[MP_CFG_COUNT];
    uint64_t est_packed[MP_CFG_COUNT];    // extrapolated to everything packable
    uint64_t est_seconds[MP_CFG_COUNT];   // estimated time to pack with `threads` threads
    uint64_t v1_index_bytes;              // AMPRZPK1 + AMPRIDX3 + volume headers
    uint64_t loose_compressed;            // loose files with zstd 19, long window, 32 MiB frames
    uint64_t p5z_estimate;
    int block_dicts;
    mp_small_result small;
    int dedup_done;
    mp_dedup_result dedup;
    int xport_done;
    mp_xport_stat xport;
    uint64_t p5z_transport;               // transport + compressed loose files
    mp_complete complete;                 // against the list of the original package's files
    uint64_t elapsed_ns;
} mp_report;

int mp_analyze(const char *src_path, const mp_analyze_opts *o, mp_report *r, mp_err *e);
void mp_report_free(mp_report *r);

// What the packable files take, from what was measured of each: raw[f] bytes of packable[f] took
// stored[f]. Extrapolated by strata (the files' extension, in lower case); a stratum with nothing
// measured takes the ratio of all. Exact when everything was measured.
int mp_strata_estimate(mp_source *src, const uint32_t *packable, size_t npack, const uint64_t *raw,
                       const uint64_t *stored, uint64_t *out, mp_err *e);
// The indices and headers a pack of the packable files adds (AMPRZPK1, AMPRIDX3 and the volume
// headers), with packed bytes in its volumes.
uint64_t mp_v1_index_bytes(mp_source *src, const uint32_t *packable, size_t npack, uint64_t packed);

#endif
