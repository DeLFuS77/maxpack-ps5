// SPDX-License-Identifier: GPL-3.0-or-later
// Internals of the .p5z reader, shared by mp_p5z_read.c, mp_unpack.c and mp_verify.c.
#ifndef MP_P5Z_INTERNAL_H
#define MP_P5Z_INTERNAL_H

#include "mp_p5z.h"
#include "mp_platform.h"

typedef struct {
    char *path;
    int role, storage;
    uint64_t size, data_off, data_len;
    int64_t mtime;
    uint32_t first_frame, nframes;
    uint8_t sha[32];
} p5z_file;

typedef struct {
    uint64_t off;
    uint32_t csize, usize;
} p5z_frame;

// Index version 2 (a .p5z that grew): the 56 bytes of version 1, then u64 prev_size (the size of the
// .p5z before the append that wrote this index; its footer then is its last 32 bytes), u32 grown (how
// many appends) and u32 0.
#define P5Z_INDEX_HEADER_V2 72u
// The most volumes maxpack.image takes (ampr-emu-ng's p5z mode): a .p5z grows only up to that many.
#define P5Z_PLAY_MAX_VOLUMES 256u

struct mp_p5z {
    mp_file *f;
    uint64_t size, index_off, index_len;
    uint32_t version, grown;
    uint64_t prev_size;
    p5z_file *files;
    size_t nfiles;
    p5z_frame *frames;
    size_t nframes;
    mp_pack_manifest man;
    uint32_t *man_file;   // index in files[] of each manifest record
};

// Reads the contents of files[i] in chunks (decompressing if needed) and checks its SHA-256.
typedef int (*p5z_sink)(void *ctx, const void *p, size_t n, mp_err *e);
int p5z_read_file(mp_p5z *p, size_t i, p5z_sink sink, void *ctx, mp_err *e);

// ---- the writer's extras (mp_p5z_write.c) ---------------------------------------------------------

// A .p5z grown by appending to f (the caller's, open for writing as its only writer): the regions go at
// its end; finish writes index version 2 (prev_size the size f had, grown as given) and a new footer,
// with the records in a fixed order (loose files by path, volumes by id, ampr_emu.index, the runtime,
// the originals by path, ampr_zstd.index). It never writes the header, nor closes or removes f.
int p5z_out_append(mp_wfile *f, unsigned threads, uint64_t max_size, uint32_t grown, mp_pack_out *out, mp_err *e);
// Record i of from, as it is stored (its region and frames stay where they are), with that mtime.
int p5z_out_keep(mp_pack_out *out, const mp_p5z *from, size_t i, int64_t mtime, mp_err *e);
// Record i of from with its stored bytes copied at the end (frames as they are, nothing compressed again).
int p5z_out_copy(mp_pack_out *out, mp_p5z *from, size_t i, mp_err *e);

// ---- the journal of an append (mp_p5z_journal.c) ----------------------------------------------------

int p5z_journal_write(const char *p5z, uint64_t old_size, const uint8_t footer[32], mp_err *e);
// 0 with what it says, 1 if there is one that is damaged, 2 if there is one that could not be read (an
// I/O error: never to be taken for damaged), 3 if there is only a .tmp cut short (no append began), -1 if
// there is none. The journal is <name>.maxpack-append or, when that is not there whole, a whole
// <name>.maxpack-append.tmp (a rename that did not reach the disk).
int p5z_journal_read(const char *p5z, uint64_t *old_size, uint8_t footer[32]);
// 1 if the journal is whole and the .p5z, size bytes long, has its footer where it says.
int p5z_journal_matches(const char *p5z, uint64_t size, uint64_t *old_size);
// 1 if the append that began when the .p5z was old_size bytes long had ended: its last 32 bytes are a
// whole footer whose index, past old_size, is of version 2 with prev_size old_size; 0 if not, -1 if the
// .p5z could not be read to tell.
int p5z_append_ended(const char *p5z, uint64_t size, uint64_t old_size);
// Deletes the journal and its .tmp: 0 when neither is there afterwards.
int p5z_journal_remove(const char *p5z);
// mp_p5z_recover with the .p5z already open as its only writer (f).
int p5z_restore_locked(mp_wfile *f, const char *p5z, int *what, mp_err *e);

#endif
