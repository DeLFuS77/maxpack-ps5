// SPDX-License-Identifier: GPL-3.0-or-later
// maxpack analyze --levels: compares zstd levels on a game. A sample of its packable data (the
// analyzer's) is packed at each level with the pack's own pipeline (mp_zpk_write) and the threads
// given, and the loose files are compressed as a .p5z compresses them, to tell what each level makes
// of the whole game and how long packing it takes on this PC. Nothing is written.
#ifndef MP_LEVELS_H
#define MP_LEVELS_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"
#include "mp_source.h"

#define MP_LEVELS_MAX 22

// A level compared. The sizes are estimates for the whole game; rate is how fast the pack went
// through the sample, and the times are for packing the whole game: the loose files, then the packs
// at the speed of the sample or of reading it, whichever is slower.
typedef struct mp_level_row {
    int level;
    uint64_t packed;        // what the packable files take in the packs
    uint64_t p5z_size;      // the .p5z: packs, indices and the loose files compressed
    uint64_t folder_size;   // the folder for the console: packs, indices and the loose files as they are
    uint64_t rate;          // bytes of the game a second
    uint64_t p5z_seconds, folder_seconds;
} mp_level_row;

typedef struct {
    int levels[MP_LEVELS_MAX];   // in increasing order, each once
    size_t nlevels;
    double sample_fraction;      // 0.02, as analyze
    uint64_t sample_min;         // 1 GiB, as analyze
    uint64_t loose_min;          // 256 MiB of the loose files' 32 MiB frames at least (all, if fewer)
    int full;                    // the whole game, no sample
    unsigned threads;
    int progress;                // progress on stderr
    const char *const *keep_loose;
    size_t nkeep;
    // told of each level once it is measured, in order (may be NULL)
    void (*on_level)(void *ctx, const mp_level_row *row);
    void *ctx;
} mp_levels_opts;

// The levels 3, 9, 15 and 19, the analyzer's sample, all the CPU's threads.
void mp_levels_opts_default(mp_levels_opts *o);
// A list such as "1,6,12,19" (levels 1 to 22, any order, repeated ones once). -1 leaves o as it was.
int mp_levels_parse(const char *list, mp_levels_opts *o, mp_err *e);

typedef struct {
    mp_source_kind kind;
    uint64_t image_size;         // the file of a compressed image (PFSC) or of a package, 0 for anything else
    uint64_t files, original_size;
    uint64_t loose_files, loose_bytes, packable_files, packable_bytes;
    uint64_t sample_bytes;       // of the packable data, in 1 MiB windows
    double sample_rate;
    uint64_t loose_sample_bytes; // of the loose files, in 32 MiB frames
    double loose_rate;
    uint64_t loose_compressed;   // the loose files in a .p5z (zstd 19, long window, 32 MiB frames)
    uint64_t loose_ns;           // compressing them on this PC
    uint64_t read_rate;          // bytes a second the sample was read at (0 if nothing was read)
    unsigned threads;
    size_t nlevels;
    mp_level_row rows[MP_LEVELS_MAX];
    uint64_t elapsed_ns;
} mp_levels_report;

int mp_levels_run(const char *src_path, const mp_levels_opts *o, mp_levels_report *r, mp_err *e);

#endif
