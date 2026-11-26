// SPDX-License-Identifier: GPL-3.0-or-later
// ampr-emu-ng zstd packs: ampr_zstd.index (AMPRZPK1) + ampr_zstd-NNN.pak (AMPRZDT1).
// Format: ampr-emu-ng's docs/zstd-format.md and ps5-dump-forge's docs/zstd.md.
#ifndef MP_ZPK_H
#define MP_ZPK_H

#include <stddef.h>
#include <stdint.h>

#include "mp_measure.h"
#include "mp_parallel.h"
#include "mp_source.h"

#define MP_ZPK_VOLUME_LIMIT 4294901760ull   // 4 GiB - 64 KiB, header included
#define MP_ZPK_MAX_VOLUMES 4096u
#define MP_ZPK_HEADER 128u
#define MP_ZPK_VOL_HEADER 64u

// Where the volumes go: a folder or regions of a .p5z.
typedef struct {
    int (*vol_begin)(void *ctx, uint32_t vol, mp_err *e);   // starts the volume: 64 B of zeros
    int (*vol_write)(void *ctx, const void *p, size_t n, mp_err *e);
    int (*vol_end)(void *ctx, uint32_t vol, mp_err *e);
    int (*vol_header)(void *ctx, uint32_t vol, const uint8_t hdr[64], mp_err *e);   // at the end
    void *ctx;
} mp_zpk_sink;

typedef struct {
    int level;              // 1..22
    unsigned threads;
    uint64_t volume_limit;  // MP_ZPK_VOLUME_LIMIT (smaller in tests)
    mp_progress *prog;
    uint32_t first_volume;  // the id of the first volume written (0; more when a .p5z grows)
} mp_zpk_opts;

typedef struct {
    uint64_t files, chunks, volumes, logical, stored;
    uint8_t archive_id[16];
} mp_zpk_stats;

// files: source indices of the packable files. Returns the ampr_zstd.index (malloc) and in sha[i]
// the SHA-256 of files[i]'s contents. The output does not depend on o->threads.
int mp_zpk_write(mp_source *src, const uint32_t *files, size_t n, const mp_zpk_opts *o,
                 const mp_zpk_sink *sink, uint8_t **index, size_t *index_len, uint8_t (*sha)[32],
                 mp_zpk_stats *st, mp_err *e);

// The chunks of files[0..n) only, as mp_zpk_write writes them, into volumes numbered from
// o->first_volume: no volume headers and no index (mp_zpk_build_index and mp_zpk_volume_header make
// them). chunks holds the files' chunk records in the index's order (path_hash, path); first[k], where
// those of files[k] begin; vol_size[k], the size of volume first_volume + k, of the nvol written.
typedef struct {
    uint64_t *chunks, nchunks;
    uint64_t *first;
    uint64_t *vol_size;
    uint32_t nvol;
    uint64_t logical, stored;
} mp_zpk_chunks;
int mp_zpk_write_chunks(mp_source *src, const uint32_t *files, size_t n, const mp_zpk_opts *o,
                        const mp_zpk_sink *sink, mp_zpk_chunks *out, uint8_t (*sha)[32], mp_err *e);
void mp_zpk_chunks_free(mp_zpk_chunks *c);

// A file of an index to build: its path, size and chunk records (ceil(size / 256 KiB) of them).
typedef struct {
    const char *path;
    uint64_t size;
    const uint64_t *chunks;
} mp_zpk_entry;
// The ampr_zstd.index of v[0..n) (sorted here by path_hash and path), with nvol volumes of the sizes
// given, at that zstd level. archive_id NULL: the one a fresh pack has, the SHA-256 of "MAXPACK1" and
// the tables; else that one. aid gets the one it carries.
int mp_zpk_build_index(mp_zpk_entry *v, size_t n, const uint64_t *vol_size, uint32_t nvol, int level,
                       const uint8_t *archive_id, uint8_t **index, size_t *len, uint8_t aid[16], mp_err *e);
// The 64-byte header of volume vol of nvol, of size bytes with its header.
void mp_zpk_volume_header(uint32_t vol, uint32_t nvol, const uint8_t aid[16], uint64_t size, uint8_t out[64]);

// Volumes as ampr_zstd-NNN.pak inside dir (which must already exist).
int mp_zpk_sink_dir(const char *dir, mp_zpk_sink *out, mp_err *e);
void mp_zpk_sink_free(mp_zpk_sink *s);

// ---- Reading -----------------------------------------------------------------------------

typedef struct {
    int (*read)(void *ctx, uint32_t vol, uint64_t off, void *buf, size_t n, mp_err *e);
    uint64_t (*size)(void *ctx, uint32_t vol);   // 0 if it does not exist
    void *ctx;
} mp_zpk_volsrc;

typedef struct mp_zpk mp_zpk;
// Checks everything the documents require (CRCs, tables, order, consecutive blocks, volume
// headers). Does not decode the blocks: mp_zpk_read_file does that.
int mp_zpk_open(const uint8_t *index, size_t len, const mp_zpk_volsrc *vs, mp_zpk **out, mp_err *e);
// The rules an index is checked by. PACK: every zstd pack's (mp_zpk_open). P5Z: those of ampr-emu-ng's
// p5z mode, for the packs of a .p5z that grew by appending (index version 2): the chunks of each volume
// may sit in any order, with room between them and volumes with none, as long as no two overlap and each
// one lies in [64, size) of its volume; a volume header may count fewer volumes than the index, never more.
enum { MP_ZPK_RULES_PACK = 0, MP_ZPK_RULES_P5Z = 1 };
int mp_zpk_open_ex(const uint8_t *index, size_t len, const mp_zpk_volsrc *vs, int rules, mp_zpk **out, mp_err *e);
int mp_zpk_level(const mp_zpk *z);
const uint8_t *mp_zpk_archive_id(const mp_zpk *z);
uint64_t mp_zpk_volume_size(const mp_zpk *z, uint32_t vol);   // 0 if there is no such volume
// The chunk records of file i (*count of them; NULL with 0 for an empty file).
const uint64_t *mp_zpk_file_chunks(const mp_zpk *z, size_t i, uint64_t *count);
uint64_t mp_zpk_stored_bytes(const mp_zpk *z);   // what every chunk takes in the volumes
// The chunks of z's files written again into sink, as is (nothing compressed again), in the index's
// order and back to back from byte 64 of volume 0, a new volume where the next chunk would pass
// volume_limit: what a fresh pack of those files writes. Gives the new index (its archive_id that of a
// fresh pack) and writes the volume headers.
int mp_zpk_relayout(mp_zpk *z, uint64_t volume_limit, const mp_zpk_sink *sink, mp_progress *prog, uint8_t **index,
                    size_t *len, uint8_t aid[16], mp_err *e);
size_t mp_zpk_count(const mp_zpk *z);
const char *mp_zpk_path(const mp_zpk *z, size_t i);
uint64_t mp_zpk_size(const mp_zpk *z, size_t i);
uint32_t mp_zpk_volume_count(const mp_zpk *z);
// Index of the file with that path (records are sorted by hash and path), or -1.
long mp_zpk_find(const mp_zpk *z, const char *path);
// Data of block k of file i (any pointer may be NULL). 0 if it exists.
int mp_zpk_chunk(const mp_zpk *z, size_t i, uint32_t k, uint32_t *vol, uint64_t *off, uint32_t *stored,
                 int *codec);
// Reads [off, off+n) of file i decoding its blocks; checks sizes and checksums.
int mp_zpk_read_file(mp_zpk *z, size_t i, uint64_t off, void *buf, size_t n, mp_err *e);
void mp_zpk_close(mp_zpk *z);

int mp_zpk_volsrc_dir(const char *dir, mp_zpk_volsrc *out, mp_err *e);   // opens on demand
void mp_zpk_volsrc_free(mp_zpk_volsrc *vs);

#endif
