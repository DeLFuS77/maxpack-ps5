// SPDX-License-Identifier: GPL-3.0-or-later
// Utilidades internas del motor del instalador.
#ifndef MP_INST_INTERNAL_H
#define MP_INST_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "mp_inst.h"

char *mpi_join(const char *a, const char *b);   // "a/b" (malloc)
// Lee un archivo entero de como mucho max bytes (malloc, acabado en NUL).
int mpi_read_small(const char *path, size_t max, char **out, size_t *len, mp_err *e);
// Escribe path.tmp y lo pone en lugar de path.
int mpi_write_replace(const char *path, const void *p, size_t n, mp_err *e);
void mpi_hex(const uint8_t *b, size_t n, char *out);       // out: 2n+1
int mpi_unhex(const char *s, uint8_t *b, size_t n);        // 0 si s son exactamente 2n hex
// titleId, contentVersion y titleName (el del idioma por defecto si lo hay) de un param.json.
int mpi_param_parse(const char *json, size_t n, char id[16], char ver[16], char name[128]);
// titleId de <dir>/sce_sys/param.json, o "" si no lo hay.
void mpi_dir_title(const char *dir, char id[16]);
uint64_t mpi_tree_size(const char *path);                 // archivo o carpeta entera
int mpi_has_ext(const char *name, const char *ext);       // ext con punto, sin distinguir mayúsculas
int mpi_record_path(const mp_inst_env *env, const char *title_id, char *out, size_t cap);
// Temporal de title_id en dest_root: <internal>/maxpack/tmp/<ID> o <unidad>/maxpack-tmp/<ID>.
void mpi_tmp_path(const mp_inst_env *env, const char *dest_root, const char *title_id, char *out, size_t cap);
// 1 si path es una raíz que nunca es "una copia de un juego": las unidades, sus homebrew/,
// etaHEN/, etaHEN/games/ y las carpetas de MaxPack.
int mpi_reserved_path(const mp_inst_env *env, const char *path);
// SHA-256 de un archivo desde el byte skip (64 en un volumen); size = tamaño total.
int mpi_file_sha(const char *path, uint64_t skip, uint8_t sha[32], uint64_t *size, mp_err *e);

// Estado de una instalación a medias (mp_inst_state.c).
typedef struct mpi_state mpi_state;
// Si hay un estado del mismo archive_id y destino (y la temporal existe), *resumed = 1 y done
// recibe las rutas apuntadas; si no, se borran el estado y la temporal y se empieza de cero.
int mpi_state_begin(const mp_inst_env *env, const char *title_id, const uint8_t aid[16], const char *p5z,
                    const char *dest_root, const char *tmp, int *resumed, char ***done, size_t *ndone,
                    mpi_state **st, mp_err *e);
int mpi_state_add(mpi_state *st, const char *rel, mp_err *e);
void mpi_state_close(mpi_state *st);
void mpi_state_remove(const mp_inst_env *env, const char *title_id);
void mpi_done_free(char **v, size_t n);

// Pone item (carpeta o archivo de la temporal) en final_path: si final_path está ocupado (y
// confirmado en ex) lo aparta y lo borra después; luego borra las demás copias de ex.
int mpi_place(const mp_inst_env *env, const char *item, const char *final_path, const mp_inst_existing *ex,
              size_t nex, mp_err *e);
// Instalar tal cual una carpeta o una imagen (mp_inst_copy.c). Rellena r.
int mpi_copy_install(const mp_inst_env *env, const mp_inst_req *req, const mp_inst_found *f, const char *tmp_dir,
                     const char *final_path, const mp_inst_existing *ex, size_t nex, mp_inst_result *r, mp_err *e);

#endif
