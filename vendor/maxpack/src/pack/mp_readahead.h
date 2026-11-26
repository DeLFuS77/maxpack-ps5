// SPDX-License-Identifier: GPL-3.0-or-later
// Read-ahead of a source: reads a list of its files (or of ranges of them), each from start to end
// and in that order, ahead of the thread that consumes them. Several threads read pieces of a few
// MiB at once (whole runs of an exFAT image's clusters, with several requests in flight), and the
// SHA-256 of each file is computed on the way by whichever of them holds that file's next piece, so
// different files hash at the same time. The consumer gets the pieces in order, exactly the bytes of
// reading the files one after another; memory is bounded by a fixed budget.
#ifndef MP_READAHEAD_H
#define MP_READAHEAD_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"
#include "mp_source.h"

#define MP_READAHEAD_MAX_THREADS 16u

typedef struct {
    unsigned threads;   // reading threads, 1..MP_READAHEAD_MAX_THREADS
    size_t piece;       // bytes per piece: within a range they start at multiples of it, the last is shorter
    size_t budget;      // bytes read and not yet given back, at most (at least one piece)
} mp_readahead_opts;

// 4 threads, 4 MiB pieces, 256 MiB (measured on an NVMe, through the system's cache: 4 threads with
// 4 MiB reads gave the most, 8 gave less).
void mp_readahead_opts_default(mp_readahead_opts *o);

typedef struct mp_readahead mp_readahead;

typedef struct {
    uint32_t file;       // source index
    uint64_t off, len;   // the bytes [off, off + len) of that file
} mp_readahead_range;

typedef struct {
    const uint8_t *data;   // valid until the next mp_readahead_next or mp_readahead_close
    size_t len;            // > 0
    size_t item;           // position in the list
    uint64_t off;          // offset in its file
    int last;              // the item's last piece: its sha is complete
} mp_readahead_piece;

// files[k] = source index of the k-th file to read whole (the array is copied). If sha is not NULL,
// sha[k] gets the SHA-256 of files[k]: of the empty ones at once, of the others when their last piece
// is handed out. Starts the threads; -1 with e set if not one can be started.
int mp_readahead_open(mp_source *src, const uint32_t *files, size_t n, uint8_t (*sha)[32],
                      const mp_readahead_opts *o, mp_readahead **out, mp_err *e);
// The same with ranges of files (copied too); sha[k] is then that of range k.
int mp_readahead_open_ranges(mp_source *src, const mp_readahead_range *ranges, size_t n, uint8_t (*sha)[32],
                             const mp_readahead_opts *o, mp_readahead **out, mp_err *e);
// The next piece in order (and gives back the previous one): 1, or 0 when every item has been
// handed out. -1 on the first piece, in order, that could not be read (e = the source's message), or
// once mp_cancelled() stops the reading (e = "cancelled"); after -1 it keeps returning -1.
int mp_readahead_next(mp_readahead *r, mp_readahead_piece *p, mp_err *e);
// Stops the threads (each finishes the read it is doing), joins them and frees everything (r may be NULL).
void mp_readahead_close(mp_readahead *r);

#endif
