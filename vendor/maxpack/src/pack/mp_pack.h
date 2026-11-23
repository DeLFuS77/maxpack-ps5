// SPDX-License-Identifier: GPL-3.0-or-later
// Empaquetado: del dump original a una salida (carpeta para la consola o .p5z para el PC).
#ifndef MP_PACK_H
#define MP_PACK_H

#include <stddef.h>
#include <stdint.h>

#include "mp_zpk.h"

typedef struct {
    int level;                       // 1..22 (19)
    unsigned threads;
    int progress;
    const char *runtime;             // libSceAmpr.sprx de ampr-emu-ng, o NULL
    const char *const *keep_loose;
    size_t nkeep;
    uint64_t volume_limit;           // MP_ZPK_VOLUME_LIMIT
} mp_pack_opts;

void mp_pack_opts_default(mp_pack_opts *o);

enum {
    MP_ROLE_LOOSE = 0,   // archivo suelto del juego
    MP_ROLE_VOLUME,      // ampr_zstd-NNN.pak (lo gestiona el sink de zpk)
    MP_ROLE_ZPK_INDEX,   // ampr_zstd.index
    MP_ROLE_IDX3,        // ampr_emu.index nuevo
    MP_ROLE_RUNTIME,     // fakelib/libSceAmpr.sprx de --runtime
    MP_ROLE_ORIGINAL     // archivo original que la carpeta reemplaza (solo para --original)
};

#define MP_MANIFEST_PACKED 1u     // su contenido está en los packs
#define MP_MANIFEST_REPLACED 2u   // la carpeta lleva otro; el original se guarda aparte

typedef struct {
    char *path;
    uint64_t size;
    int64_t mtime;
    uint8_t sha[32];
    uint32_t flags;
} mp_manifest_entry;

typedef struct mp_pack_manifest {
    mp_manifest_entry *v;   // un registro por archivo del dump original, en orden de ruta
    size_t n;
    char title_id[16], content_version[16], title_name[128];
    int level;
    uint8_t archive_id[16];
} mp_pack_manifest;

void mp_pack_manifest_free(mp_pack_manifest *m);

// Salida del empaquetado. Los archivos llegan en flujo: begin, write..., end.
typedef struct mp_pack_out {
    mp_zpk_sink zpk;
    int (*file_begin)(void *ctx, const char *rel, int role, uint64_t size, int64_t mtime, mp_err *e);
    int (*file_write)(void *ctx, const void *p, size_t n, mp_err *e);
    int (*file_end)(void *ctx, const uint8_t sha[32], mp_err *e);
    int (*finish)(void *ctx, const mp_pack_manifest *m, mp_err *e);
    void (*destroy)(void *ctx);
    void *ctx;
} mp_pack_out;

typedef struct {
    uint64_t loose_files, loose_bytes, packed_files, packed_bytes, stored_bytes, volumes;
    int runtime_without_zstd;   // el libSceAmpr.sprx final no parece leer packs zstd
    char title_id[16];          // el TITLE_ID de sce_sys/param.json ("" si no lo da)
} mp_pack_result;

int mp_pack_run(const char *src_path, mp_pack_out *out, const mp_pack_opts *o, mp_pack_result *r,
                mp_err *e);

// Carpeta lista para la consola. La carpeta no debe existir o debe estar vacía.
int mp_pack_out_dir(const char *out_dir, mp_pack_out *out, mp_err *e);

// ---- verify -------------------------------------------------------------------------------

typedef struct {
    uint64_t files_ok, files_bad, bytes_checked;
} mp_verify_result;

// Comprueba todo el .p5z: cada archivo guardado y cada archivo empaquetado contra el SHA-256 del
// manifiesto. Devuelve 0 si pudo comprobar (los fallos van en files_bad y se listan en stderr).
int mp_verify_p5z(const char *path, unsigned threads, int progress, mp_verify_result *r, mp_err *e);
// Comprueba una carpeta empaquetada (packs, bloques y ampr_emu.index). Con against (el origen),
// compara además cada archivo del juego original por SHA-256.
int mp_verify_folder(const char *dir, const char *against, unsigned threads, int progress,
                     mp_verify_result *r, mp_err *e);

#endif
