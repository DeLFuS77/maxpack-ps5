// SPDX-License-Identifier: GPL-3.0-or-later
// Todo lo que depende del sistema operativo. Las rutas siempre son UTF-8 con '/' o '\'.
#ifndef MP_PLATFORM_H
#define MP_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

typedef struct mp_file mp_file;

// Abre solo para lectura. 0 = bien, -1 = error.
int mp_file_open_read(const char *utf8_path, mp_file **out, mp_err *e);
int mp_file_size(mp_file *f, uint64_t *out, mp_err *e);
// Lectura posicional y segura entre hilos; *got < len solo al llegar al final del archivo.
int mp_file_pread(mp_file *f, void *buf, size_t len, uint64_t off, size_t *got, mp_err *e);
void mp_file_close(mp_file *f);

int mp_write_file(const char *utf8_path, const void *data, size_t len, mp_err *e);

// Archivo de salida nuevo (error si ya existe): se escribe al final y se pueden corregir bytes
// ya escritos (p. ej. cabeceras que solo se conocen al acabar). Un solo hilo por archivo.
typedef struct mp_wfile mp_wfile;
int mp_wfile_create(const char *utf8_path, mp_wfile **out, mp_err *e);
int mp_wfile_reopen(const char *utf8_path, mp_wfile **out, mp_err *e);   // existente, sin truncar
int mp_wfile_write(mp_wfile *f, const void *p, size_t n, mp_err *e);
int mp_wfile_pwrite(mp_wfile *f, const void *p, size_t n, uint64_t off, mp_err *e);
uint64_t mp_wfile_size(const mp_wfile *f);
int mp_wfile_close(mp_wfile *f, mp_err *e);   // también libera f si falla
int mp_wfile_sync(mp_wfile *f, mp_err *e);    // fuerza los datos al disco (fsync)
int mp_set_mtime(const char *utf8_path, int64_t unix_seconds, mp_err *e);
int mp_remove_file(const char *utf8_path);   // 0 si se borró
int mp_stat(const char *utf8_path, uint64_t *size, int64_t *mtime);   // 0 si existe
// 1 si path es dir o está dentro de dir (por identidad en el disco, recorriendo sus carpetas
// padre aunque path todavía no exista).
int mp_path_is_inside(const char *path, const char *dir);
int mp_dir_is_empty(const char *utf8_path);    // 1 vacía, 0 con contenido, -1 no existe
int mp_make_dirs(const char *utf8_path, mp_err *e);
// 0 = no existe, 1 = archivo, 2 = carpeta.
int mp_path_kind(const char *utf8_path);

// Recorre la carpeta de forma recursiva. rel_utf8 es relativa a la raíz y usa '/'.
// No sigue enlaces simbólicos ni puntos de reanálisis. Si cb devuelve != 0, se detiene
// y mp_dir_walk devuelve ese valor.
typedef int (*mp_dir_cb)(void *ctx, const char *rel_utf8, int is_dir, uint64_t size,
                         int64_t mtime_unix);
int mp_dir_walk(const char *root_utf8, mp_dir_cb cb, void *ctx, mp_err *e);

// Un solo nivel de la carpeta, sin "." ni "..", sin enlaces. -1 si no es una carpeta.
typedef int (*mp_list_cb)(void *ctx, const char *name, int is_dir, uint64_t size);
int mp_dir_list(const char *dir_utf8, mp_list_cb cb, void *ctx, mp_err *e);
// Renombra un archivo o carpeta dentro del mismo sistema de archivos; falla si 'to' existe.
int mp_rename(const char *from_utf8, const char *to_utf8, mp_err *e);
// Borra una carpeta entera (sin seguir enlaces) o un archivo. 0 también si no existe.
int mp_remove_tree(const char *utf8_path, mp_err *e);
// Bytes libres para el usuario en el sistema de archivos de path (que debe existir).
int mp_disk_free(const char *utf8_path, uint64_t *free_bytes, mp_err *e);

typedef struct mp_thread mp_thread;
int mp_thread_start(mp_thread **t, void (*fn)(void *), void *arg);
void mp_thread_join(mp_thread *t);

typedef struct mp_mutex mp_mutex;
mp_mutex *mp_mutex_new(void);
void mp_mutex_lock(mp_mutex *m);
void mp_mutex_unlock(mp_mutex *m);
void mp_mutex_free(mp_mutex *m);

uint64_t mp_now_ns(void);
unsigned mp_cpu_count(void);
void mp_sleep_ms(unsigned ms);

// En Windows sustituye argc/argv por los argumentos reales en UTF-8 (argv llega en la página
// de códigos ANSI y pierde caracteres como ñ o emoji). En el resto no hace nada.
void mp_args_utf8(int *argc, char ***argv);

// Ruta absoluta en UTF-8 de un archivo o carpeta, exista o no (malloc). NULL si no se puede.
char *mp_full_path(const char *utf8_path);
// 1 si las dos rutas existen y son el mismo archivo o carpeta (misma identidad en el disco,
// no el mismo texto: no la engañan mayúsculas, "..", barras distintas ni enlaces).
int mp_same_file(const char *a, const char *b);

// Copia de una cadena con malloc (strdup no es C11).
char *mp_strdup(const char *s);

#endif
