// SPDX-License-Identifier: GPL-3.0-or-later
// Internal utilities of the installer engine.
#ifndef MP_INST_INTERNAL_H
#define MP_INST_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "mp_inst.h"

char *mpi_join(const char *a, const char *b);   // "a/b" (malloc)
// Reads a whole file of at most max bytes (malloc, NUL-terminated).
int mpi_read_small(const char *path, size_t max, char **out, size_t *len, mp_err *e);
// Writes path.tmp and puts it in place of path.
int mpi_write_replace(const char *path, const void *p, size_t n, mp_err *e);
void mpi_hex(const uint8_t *b, size_t n, char *out);       // out: 2n+1
int mpi_unhex(const char *s, uint8_t *b, size_t n);        // 0 if s is exactly 2n hex digits
// titleId, contentVersion and titleName (the default language's if there is one) of a param.json.
int mpi_param_parse(const char *json, size_t n, char id[16], char ver[16], char name[128]);
// titleId of <dir>/sce_sys/param.json, or "" if there is none.
void mpi_dir_title(const char *dir, char id[16]);
uint64_t mpi_tree_size(const char *path);                 // a file or a whole folder
int mpi_has_ext(const char *name, const char *ext);       // ext with the dot, case-insensitive
int mpi_record_path(const mp_inst_env *env, const char *title_id, char *out, size_t cap);
// Temporary folder of title_id in dest_root: <internal>/maxpack/tmp/<ID> or <drive>/maxpack-tmp/<ID>.
void mpi_tmp_path(const mp_inst_env *env, const char *dest_root, const char *title_id, char *out, size_t cap);
// 1 if path is a root that is never "a copy of a game": the drives, their homebrew/,
// etaHEN/, etaHEN/games/ and MaxPack's folders.
int mpi_reserved_path(const mp_inst_env *env, const char *path);
// SHA-256 of a file from byte skip (64 in a volume); size = total size.
int mpi_file_sha(const char *path, uint64_t skip, uint8_t sha[32], uint64_t *size, mp_err *e);

// State of a half-done install (mp_inst_state.c).
typedef struct mpi_state mpi_state;
// If there is a state with the same archive_id and destination (and the temporary folder exists), *resumed = 1 and done
// receives the marked paths; otherwise the state and the temporary folder are deleted and it starts from scratch.
int mpi_state_begin(const mp_inst_env *env, const char *title_id, const uint8_t aid[16], const char *p5z,
                    const char *dest_root, const char *tmp, int *resumed, char ***done, size_t *ndone,
                    mpi_state **st, mp_err *e);
int mpi_state_add(mpi_state *st, const char *rel, mp_err *e);
void mpi_state_close(mpi_state *st);
void mpi_state_remove(const mp_inst_env *env, const char *title_id);
void mpi_done_free(char **v, size_t n);

// Puts item (a folder or file from the temporary folder) at final_path: if final_path is taken (and
// confirmed in ex) it sets it aside and deletes it afterwards; then deletes the other copies in ex.
int mpi_place(const mp_inst_env *env, const char *item, const char *final_path, const mp_inst_existing *ex,
              size_t nex, mp_err *e);
// Install a folder or an image as is (mp_inst_copy.c). Fills r.
int mpi_copy_install(const mp_inst_env *env, const mp_inst_req *req, const mp_inst_found *f, const char *tmp_dir,
                     const char *final_path, const mp_inst_existing *ex, size_t nex, mp_inst_result *r, mp_err *e);

#endif
