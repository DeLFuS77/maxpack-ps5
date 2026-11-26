// SPDX-License-Identifier: GPL-3.0-or-later
// Installer engine (phase 2): find what can be installed, install, resume, replace, verify and
// uninstall. Portable: the same code on the PC (tests) and on the PS5.
// Design: docs/design/phase2-installer.md
#ifndef MP_INST_H
#define MP_INST_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

typedef struct {
    const uint8_t *data;   // NULL: the installer carries no runtime
    size_t len;
    int64_t mtime;
    const char *version;
} mp_inst_runtime;

// Called after each write with the bytes written in this call to mp_inst_install.
// Returns 0 to continue, 1 to stop (pause) or -1 to fail (with e).
typedef int (*mp_inst_io_check)(void *ctx, uint64_t bytes_written, mp_err *e);

// progress receives one of these stages: "copiando", "comprobando", "terminando" (the API adds "registrando"). They are
// identifiers, not text for people: the console page maps them to its translated labels, so they keep these names.
typedef struct {
    const char *internal;                    // "/data"
    const char *const *drive_candidates;     // "/mnt/usb0".."/mnt/ext1"; those that exist count
    size_t ndrive_candidates;
    mp_inst_runtime runtime;
    unsigned threads;                        // 4 by default
    uint64_t space_margin;                   // 256 MiB by default
    int (*free_space)(void *ctx, const char *path, uint64_t *out);   // NULL: mp_disk_free
    mp_inst_io_check io_check;
    void (*notify)(void *ctx, const char *msg);
    void (*progress)(void *ctx, const char *stage, uint64_t done, uint64_t total);
    void *ctx;
} mp_inst_env;

// PS5 values: /data and /mnt/usb0..7, /mnt/ext0..1.
void mp_inst_env_default(mp_inst_env *env);
int mp_inst_title_ok(const char *title_id);   // 1 if it is 4 letters A-Z and 5 digits
size_t mp_inst_drives(const mp_inst_env *env, const char **out, size_t max);   // those that exist

enum { MP_SRC_P5Z = 0, MP_SRC_FOLDER, MP_SRC_IMAGE };

typedef struct {
    char *path;
    int kind;     // MP_SRC_*
    int packed;   // folder (or exFAT image) with zstd packs: ampr_zstd.index in the root
    char title_id[16], version[16], name[128];
    uint64_t p5z_size, install_size;   // install_size = what the installed folder will take up
    uint64_t original_size;            // what the original dump took up
    uint8_t archive_id[16];
} mp_inst_found;

int mp_inst_probe(const char *p5z_path, mp_inst_found *out, mp_err *e);   // opens and validates
// .p5z, game folder (with sce_sys/param.json) or .exfat/.ffpkg/.ffpfs/.ffpfsc image. In
// .ffpkg/.ffpfs/.ffpfsc the TITLE_ID comes from the file name. Outside a .p5z, archive_id is left
// at zero and p5z_size = install_size = original_size = what is copied.
int mp_inst_probe_any(const char *path, mp_inst_found *out, mp_err *e);
// .p5z files, game folders and images in the root and one level below each drive, and .p5z in
// <internal>/maxpack/. Those that cannot be opened are skipped (with notify).
int mp_inst_scan(const mp_inst_env *env, mp_inst_found **out, size_t *n, mp_err *e);
void mp_inst_found_free(mp_inst_found *v, size_t n);

typedef struct {
    char *path;
    uint64_t size;
    int is_file;
    int from_maxpack;   // MaxPack's record points to this path
} mp_inst_existing;

// Copies of title_id in the paths the loaders scan (by their sce_sys/param.json), plus
// <root>/homebrew/<title_id> if it exists, plus 'known' (paths ShadowMountPlus gives).
int mp_inst_find_existing(const mp_inst_env *env, const char *title_id, const char *const *known,
                          size_t nknown, mp_inst_existing **out, size_t *n, mp_err *e);
void mp_inst_existing_free(mp_inst_existing *v, size_t n);

// Registry of what was installed: <internal>/maxpack/installed/<TITLE_ID>.json.
typedef struct {
    char title_id[16], version[16], name[128];
    char dir[1024];   // the game's folder, or the image's file
    char p5z[1024];   // where it was installed from (.p5z, folder or image)
    int kind;         // MP_SRC_*
    int packed;       // carries zstd packs: ShadowMountPlus must leave its fakelib in place
    uint8_t archive_id[16], runtime_sha[32];
    int runtime_replaced, smp_pending;
    char smp_why[256];
    int64_t installed_at;
} mp_inst_record;

int mp_inst_record_load(const mp_inst_env *env, const char *title_id, mp_inst_record *r, mp_err *e);
int mp_inst_record_save(const mp_inst_env *env, const mp_inst_record *r, mp_err *e);

// ---- install -------------------------------------------------------------------------------

typedef struct {
    const char *p5z_path;                           // .p5z, game folder or image
    const char *dest_root;                          // env->internal or a drive that exists
    const char *const *replace;                     // paths confirmed for deletion
    size_t nreplace;
    const char *const *known;                       // copies ShadowMountPlus knows about
    size_t nknown;
    int delete_p5z;
} mp_inst_req;

enum { MP_INST_DONE = 0, MP_INST_STOPPED, MP_INST_NEEDS_CONFIRM, MP_INST_NO_SPACE, MP_INST_FAILED };

typedef struct {
    int status;
    const char *code;   // "done" "stopped" "needs_confirm" "no_space" "bad_p5z" "bad_dest"
                        // "no_runtime" "io_error" "bad_source" "grown_p5z"
    char title_id[16];
    char final_dir[1024];
    uint64_t need, avail;
    int runtime_replaced;
    mp_inst_existing *existing;   // with needs_confirm
    size_t nexisting;
} mp_inst_result;

// 0 if it ended with a decision (done, stopped, needs_confirm, no_space); -1 if it failed (e).
// What was installed is left with smp_pending = 1 in the record: registering with ShadowMountPlus is separate.
int mp_inst_install(const mp_inst_env *env, const mp_inst_req *req, mp_inst_result *r, mp_err *e);
void mp_inst_result_free(mp_inst_result *r);

// What mp_inst_install would do before writing anything: source, destination, copies that need confirming
// and space. The API uses it to answer right away and the engine, to decide; so they never disagree.
typedef struct {
    mp_inst_found f;
    char final_path[1024];   // the game's folder, or the image's file
    char tmp_root[1024], tmp[1100];
    mp_inst_existing *ex;    // copies that will be deleted (already without the source, the roots or those that contain it)
    size_t nex;
    int status;              // MP_INST_DONE (can continue), MP_INST_NEEDS_CONFIRM or MP_INST_NO_SPACE
    uint64_t need, avail;    // avail also counts what this game's temporary folder already takes up
} mp_inst_plan;

// 0 with status; -1 if it cannot be installed (code: "bad_p5z", "bad_source", "bad_dest", "io_error").
int mp_inst_plan_make(const mp_inst_env *env, const mp_inst_req *req, mp_inst_plan *pl, const char **code,
                      mp_err *e);
void mp_inst_plan_free(mp_inst_plan *pl);

// Half-done installs (saved state) and discarding them.
typedef struct {
    char title_id[16];
    char source[1024], dest[1024];
    uint64_t bytes;   // what is already written in its temporary folder
} mp_inst_pending_item;
int mp_inst_pending(const mp_inst_env *env, mp_inst_pending_item **out, size_t *n, mp_err *e);
int mp_inst_discard(const mp_inst_env *env, const char *title_id, mp_err *e);   // deletes the state and the temporary folder
// After a cut-off between setting the old game aside and putting the new one in, puts the old one back in place.
void mp_inst_recover(const mp_inst_env *env);

// Re-reads the folder and compares each file that comes from the .p5z with its SHA-256 (skip_runtime: does not
// compare ampr_emu.index or the runtime, which the installer changed). Failures, or -1 if it could not.
typedef struct mp_p5z mp_p5z;
long mp_inst_check_dir(mp_p5z *p, const char *dir, int skip_runtime, unsigned threads, mp_err *e);

// ---- manage what was installed ---------------------------------------------------------------

// Records of what was installed, by TITLE_ID (malloc). Damaged ones are skipped with notify.
int mp_inst_list(const mp_inst_env *env, mp_inst_record **out, size_t *n, mp_err *e);

typedef struct {
    uint64_t files_ok, files_bad;
    int p5z_checked;   // it was compared with the .p5z (still in place and the same one)
} mp_inst_verify_result;

int mp_inst_verify(const mp_inst_env *env, const char *title_id, mp_inst_verify_result *r, mp_err *e);
// Only titles in MaxPack's registry: deletes the folder and the record.
int mp_inst_uninstall(const mp_inst_env *env, const char *title_id, mp_err *e);

#endif
