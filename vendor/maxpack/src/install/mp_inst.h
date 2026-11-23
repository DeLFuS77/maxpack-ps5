// SPDX-License-Identifier: GPL-3.0-or-later
// Motor del instalador (fase 2): buscar lo instalable, instalar, reanudar, reemplazar, verificar y
// desinstalar. Portable: el mismo código en el PC (pruebas) y en la PS5.
// Diseño: docs/diseno/fase2-instalador.md
#ifndef MP_INST_H
#define MP_INST_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

typedef struct {
    const uint8_t *data;   // NULL: el instalador no lleva runtime
    size_t len;
    int64_t mtime;
    const char *version;
} mp_inst_runtime;

// Se llama tras cada escritura con los bytes escritos en esta llamada a mp_inst_install.
// Devuelve 0 para seguir, 1 para parar (pausa) o -1 para fallar (con e).
typedef int (*mp_inst_io_check)(void *ctx, uint64_t bytes_written, mp_err *e);

// progress recibe una de estas etapas: "copiando", "comprobando", "terminando".
typedef struct {
    const char *internal;                    // "/data"
    const char *const *drive_candidates;     // "/mnt/usb0".."/mnt/ext1"; cuentan las que existen
    size_t ndrive_candidates;
    mp_inst_runtime runtime;
    unsigned threads;                        // 4 por defecto
    uint64_t space_margin;                   // 256 MiB por defecto
    int (*free_space)(void *ctx, const char *path, uint64_t *out);   // NULL: mp_disk_free
    mp_inst_io_check io_check;
    void (*notify)(void *ctx, const char *msg);
    void (*progress)(void *ctx, const char *stage, uint64_t done, uint64_t total);
    void *ctx;
} mp_inst_env;

// Valores de la PS5: /data y /mnt/usb0..7, /mnt/ext0..1.
void mp_inst_env_default(mp_inst_env *env);
int mp_inst_title_ok(const char *title_id);   // 1 si son 4 letras A-Z y 5 dígitos
size_t mp_inst_drives(const mp_inst_env *env, const char **out, size_t max);   // las que existen

enum { MP_SRC_P5Z = 0, MP_SRC_FOLDER, MP_SRC_IMAGE };

typedef struct {
    char *path;
    int kind;     // MP_SRC_*
    int packed;   // carpeta (o imagen exFAT) con packs zstd: ampr_zstd.index en la raíz
    char title_id[16], version[16], name[128];
    uint64_t p5z_size, install_size;   // install_size = lo que ocupará la carpeta instalada
    uint64_t original_size;            // lo que ocupaba el dump original
    uint8_t archive_id[16];
} mp_inst_found;

int mp_inst_probe(const char *p5z_path, mp_inst_found *out, mp_err *e);   // abre y valida
// .p5z, carpeta de juego (con sce_sys/param.json) o imagen .exfat/.ffpkg/.ffpfs/.ffpfsc. En
// .ffpkg/.ffpfs/.ffpfsc el TITLE_ID sale del nombre del archivo. Fuera de un .p5z, archive_id queda
// a cero y p5z_size = install_size = original_size = lo que se copia.
int mp_inst_probe_any(const char *path, mp_inst_found *out, mp_err *e);
// .p5z, carpetas de juego e imágenes en la raíz y un nivel por debajo de cada unidad, y .p5z en
// <internal>/maxpack/. Los que no se pueden abrir se saltan (con notify).
int mp_inst_scan(const mp_inst_env *env, mp_inst_found **out, size_t *n, mp_err *e);
void mp_inst_found_free(mp_inst_found *v, size_t n);

typedef struct {
    char *path;
    uint64_t size;
    int is_file;
    int from_maxpack;   // el registro de MaxPack apunta a esta ruta
} mp_inst_existing;

// Copias de title_id en las rutas que exploran los cargadores (por su sce_sys/param.json), más
// <raíz>/homebrew/<title_id> si existe, más 'known' (rutas que da ShadowMountPlus).
int mp_inst_find_existing(const mp_inst_env *env, const char *title_id, const char *const *known,
                          size_t nknown, mp_inst_existing **out, size_t *n, mp_err *e);
void mp_inst_existing_free(mp_inst_existing *v, size_t n);

// Registro de lo instalado: <internal>/maxpack/installed/<TITLE_ID>.json.
typedef struct {
    char title_id[16], version[16], name[128];
    char dir[1024];   // carpeta del juego, o el archivo de la imagen
    char p5z[1024];   // de dónde se instaló (.p5z, carpeta o imagen)
    int kind;         // MP_SRC_*
    int packed;       // lleva packs zstd: ShadowMountPlus tiene que dejarle su fakelib
    uint8_t archive_id[16], runtime_sha[32];
    int runtime_replaced, smp_pending;
    char smp_why[256];
    int64_t installed_at;
} mp_inst_record;

int mp_inst_record_load(const mp_inst_env *env, const char *title_id, mp_inst_record *r, mp_err *e);
int mp_inst_record_save(const mp_inst_env *env, const mp_inst_record *r, mp_err *e);

// ---- instalar ------------------------------------------------------------------------------

typedef struct {
    const char *p5z_path;                           // .p5z, carpeta de juego o imagen
    const char *dest_root;                          // env->internal o una unidad que existe
    const char *const *replace;                     // rutas confirmadas para borrar
    size_t nreplace;
    const char *const *known;                       // copias que conoce ShadowMountPlus
    size_t nknown;
    int delete_p5z;
} mp_inst_req;

enum { MP_INST_DONE = 0, MP_INST_STOPPED, MP_INST_NEEDS_CONFIRM, MP_INST_NO_SPACE, MP_INST_FAILED };

typedef struct {
    int status;
    const char *code;   // "done" "stopped" "needs_confirm" "no_space" "bad_p5z" "bad_dest"
                        // "no_runtime" "io_error" "bad_source"
    char title_id[16];
    char final_dir[1024];
    uint64_t need, avail;
    int runtime_replaced;
    mp_inst_existing *existing;   // con needs_confirm
    size_t nexisting;
} mp_inst_result;

// 0 si terminó con una decisión (done, stopped, needs_confirm, no_space); -1 si falló (e).
// Lo instalado queda con smp_pending = 1 en el registro: el registro en ShadowMountPlus es aparte.
int mp_inst_install(const mp_inst_env *env, const mp_inst_req *req, mp_inst_result *r, mp_err *e);
void mp_inst_result_free(mp_inst_result *r);

// Lo que haría mp_inst_install antes de escribir nada: origen, destino, copias que hay que confirmar
// y espacio. La API lo usa para contestar enseguida y el motor, para decidir; así nunca discrepan.
typedef struct {
    mp_inst_found f;
    char final_path[1024];   // carpeta del juego, o archivo de la imagen
    char tmp_root[1024], tmp[1100];
    mp_inst_existing *ex;    // copias que se borrarán (ya sin el origen, las raíces ni las que lo contienen)
    size_t nex;
    int status;              // MP_INST_DONE (se puede seguir), MP_INST_NEEDS_CONFIRM o MP_INST_NO_SPACE
    uint64_t need, avail;    // avail cuenta también lo que ya ocupa la temporal de este juego
} mp_inst_plan;

// 0 con status; -1 si no se puede instalar (code: "bad_p5z", "bad_source", "bad_dest", "io_error").
int mp_inst_plan_make(const mp_inst_env *env, const mp_inst_req *req, mp_inst_plan *pl, const char **code,
                      mp_err *e);
void mp_inst_plan_free(mp_inst_plan *pl);

// Instalaciones a medias (estado guardado) y descartarlas.
typedef struct {
    char title_id[16];
    char source[1024], dest[1024];
    uint64_t bytes;   // lo que ya hay escrito en su temporal
} mp_inst_pending_item;
int mp_inst_pending(const mp_inst_env *env, mp_inst_pending_item **out, size_t *n, mp_err *e);
int mp_inst_discard(const mp_inst_env *env, const char *title_id, mp_err *e);   // borra estado y temporal
// Tras un corte entre apartar el juego viejo y poner el nuevo, vuelve a dejar el viejo en su sitio.
void mp_inst_recover(const mp_inst_env *env);

// Relee la carpeta y compara cada archivo que viene del .p5z con su SHA-256 (skip_runtime: no
// compara ampr_emu.index ni el runtime, que cambió el instalador). Fallos, o -1 si no pudo.
typedef struct mp_p5z mp_p5z;
long mp_inst_check_dir(mp_p5z *p, const char *dir, int skip_runtime, unsigned threads, mp_err *e);

// ---- gestionar lo instalado ------------------------------------------------------------------

// Registros de lo instalado, por TITLE_ID (malloc). Los dañados se saltan con notify.
int mp_inst_list(const mp_inst_env *env, mp_inst_record **out, size_t *n, mp_err *e);

typedef struct {
    uint64_t files_ok, files_bad;
    int p5z_checked;   // se comparó con el .p5z (sigue en su sitio y es el mismo)
} mp_inst_verify_result;

int mp_inst_verify(const mp_inst_env *env, const char *title_id, mp_inst_verify_result *r, mp_err *e);
// Solo títulos del registro de MaxPack: borra la carpeta y el registro.
int mp_inst_uninstall(const mp_inst_env *env, const char *title_id, mp_err *e);

#endif
