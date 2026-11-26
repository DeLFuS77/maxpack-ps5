// SPDX-License-Identifier: GPL-3.0-or-later
// Packing: from the original dump to an output (a folder for the console or a .p5z for the PC).
#ifndef MP_PACK_H
#define MP_PACK_H

#include <stddef.h>
#include <stdint.h>

#include "mp_complete.h"
#include "mp_zpk.h"

typedef struct {
    int level;                       // 1..22 (19)
    unsigned threads;
    int progress;
    const char *runtime;             // ampr-emu-ng's libSceAmpr.sprx, or NULL
    const char *const *keep_loose;
    size_t nkeep;
    uint64_t volume_limit;           // MP_ZPK_VOLUME_LIMIT
    // leave out what dump tools leave behind (mp_complete.h), only when the game's own list of the
    // original package's files proves it is not the game's (the dump tool's list does not): without
    // one, everything stays
    int leave_out_leftovers;
    // --dlc: the game's DLC (packages, folders or images) to pack with it, in this order (mp_dlc.h)
    const char *const *dlc;
    size_t ndlc;
} mp_pack_opts;

void mp_pack_opts_default(mp_pack_opts *o);

enum {
    MP_ROLE_LOOSE = 0,   // loose file of the game
    MP_ROLE_VOLUME,      // ampr_zstd-NNN.pak (handled by the zpk sink)
    MP_ROLE_ZPK_INDEX,   // ampr_zstd.index
    MP_ROLE_IDX3,        // new ampr_emu.index
    MP_ROLE_RUNTIME,     // fakelib/libSceAmpr.sprx from --runtime
    MP_ROLE_ORIGINAL     // original file that the folder replaces (only for --original)
};

#define MP_MANIFEST_PACKED 1u     // its contents are in the packs
#define MP_MANIFEST_REPLACED 2u   // the folder carries another one; the original is kept apart

typedef struct {
    char *path;
    uint64_t size;
    int64_t mtime;
    uint8_t sha[32];
    uint32_t flags;
} mp_manifest_entry;

typedef struct mp_pack_manifest {
    mp_manifest_entry *v;   // one record per file of the original dump, in path order
    size_t n;
    char title_id[16], content_version[16], title_name[128];
    int level;
    uint8_t archive_id[16];
} mp_pack_manifest;

void mp_pack_manifest_free(mp_pack_manifest *m);

// Packing output. Files arrive as a stream: begin, write..., end.
typedef struct mp_pack_out {
    mp_zpk_sink zpk;
    int (*file_begin)(void *ctx, const char *rel, int role, uint64_t size, int64_t mtime, mp_err *e);
    int (*file_write)(void *ctx, const void *p, size_t n, mp_err *e);
    int (*file_end)(void *ctx, const uint8_t sha[32], mp_err *e);
    // Optional (NULL = nothing to do): called once after the loose files and before the packs, so
    // the output finishes what it still holds of the files before them and frees that memory first.
    int (*packs_begin)(void *ctx, mp_err *e);
    int (*finish)(void *ctx, const mp_pack_manifest *m, mp_err *e);
    void (*destroy)(void *ctx);
    void *ctx;
} mp_pack_out;

typedef struct {
    uint64_t loose_files, loose_bytes, packed_files, packed_bytes, stored_bytes, volumes;
    int runtime_without_zstd;   // the final libSceAmpr.sprx does not seem to read zstd packs
    char title_id[16];          // the TITLE_ID from sce_sys/param.json ("" if it gives none)
    mp_complete complete;       // the source against its list of the original package (counts only)
    uint64_t left_out_files, left_out_bytes;   // the leftovers left out (leave_out_leftovers)
    // the DLC packed with it: with files of their own (in addcont0/...), licenses only, and what the
    // files of the first take; when they stopped it, why, as a code (mp_dlc_attach), else NULL
    unsigned dlc_data, dlc_license;
    uint64_t dlc_bytes;
    const char *dlc_error;
} mp_pack_result;

int mp_pack_run(const char *src_path, mp_pack_out *out, const mp_pack_opts *o, mp_pack_result *r,
                mp_err *e);

// ---- shared with growing a .p5z (mp_grow.c) -----------------------------------------------------

#define MP_RUNTIME_PATH "fakelib/libSceAmpr.sprx"
// 1 for the files of the dump that ampr_emu.index does not list (ampr_commands.bin, apr_emu.log); the
// dump's own ampr_emu.index is not listed either.
int mp_pack_idx3_skip(const char *path);
// 1 for a file that says the dump is packed already (ampr_zstd.index, ampr_zstd-*.pak).
int mp_pack_already_packed(const char *path);
// Copies file i of src to out as rel, with that role, in pieces through buf (4 MiB), and gives its SHA-256.
int mp_pack_copy_file(mp_source *src, size_t i, const char *rel, int role, mp_pack_out *out, uint8_t *buf,
                      uint8_t sha[32], mp_progress *prog, mp_err *e);
// Writes the n bytes of p to out as rel.
int mp_pack_put_buffer(mp_pack_out *out, const char *rel, int role, const uint8_t *p, size_t n, int64_t mtime,
                       mp_err *e);

// Folder ready for the console. The folder must not exist or must be empty.
int mp_pack_out_dir(const char *out_dir, mp_pack_out *out, mp_err *e);

// ---- verify -------------------------------------------------------------------------------

typedef struct {
    uint64_t files_ok, files_bad, bytes_checked;
} mp_verify_result;

// Checks the whole .p5z: each stored file and each packed file against the SHA-256 in the
// manifest. Returns 0 if it could check (failures go in files_bad and are listed on stderr).
int mp_verify_p5z(const char *path, unsigned threads, int progress, mp_verify_result *r, mp_err *e);
// Checks a packed folder (packs, blocks and ampr_emu.index). With against (the source),
// it also compares each file of the original game by SHA-256.
int mp_verify_folder(const char *dir, const char *against, unsigned threads, int progress,
                     mp_verify_result *r, mp_err *e);

#endif
