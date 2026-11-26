// SPDX-License-Identifier: GPL-3.0-or-later
// Everything that depends on the operating system. Paths are always UTF-8 with '/' or '\'.
#ifndef MP_PLATFORM_H
#define MP_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

typedef struct mp_file mp_file;

// Opens read-only. 0 = ok, -1 = error.
int mp_file_open_read(const char *utf8_path, mp_file **out, mp_err *e);
int mp_file_size(mp_file *f, uint64_t *out, mp_err *e);
// Positional read, safe across threads; *got < len only on reaching the end of the file.
int mp_file_pread(mp_file *f, void *buf, size_t len, uint64_t off, size_t *got, mp_err *e);
void mp_file_close(mp_file *f);
// Opens read-only so that reads come from the disk, not from the system's cache of the file (to
// measure the disk): Windows reads past the cache, Linux first drops what of the file it has cached
// (what was written and synced), macOS does not keep what is read. Every read must be at an offset
// and of a length that are multiples of 4096, into memory aligned to 4096.
int mp_file_open_read_uncached(const char *utf8_path, mp_file **out, mp_err *e);

int mp_write_file(const char *utf8_path, const void *data, size_t len, mp_err *e);

// New output file (error if it already exists): written at the end, and bytes already written
// can be corrected (e.g. headers only known at the end). One thread per file.
typedef struct mp_wfile mp_wfile;
int mp_wfile_create(const char *utf8_path, mp_wfile **out, mp_err *e);
int mp_wfile_reopen(const char *utf8_path, mp_wfile **out, mp_err *e);   // existing, not truncated
// The same as the only writer of the file: others may still read it, but no one else writes it while f is
// open (another MaxPack asking gets -1 with *busy = 1; any other failure, *busy = 0).
int mp_wfile_reopen_locked(const char *utf8_path, mp_wfile **out, int *busy, mp_err *e);
// Cuts the file to size bytes (or makes it that long); what is written next goes there.
int mp_wfile_truncate(mp_wfile *f, uint64_t size, mp_err *e);
int mp_wfile_write(mp_wfile *f, const void *p, size_t n, mp_err *e);
int mp_wfile_pwrite(mp_wfile *f, const void *p, size_t n, uint64_t off, mp_err *e);
uint64_t mp_wfile_size(const mp_wfile *f);
int mp_wfile_close(mp_wfile *f, mp_err *e);   // also frees f on failure
int mp_wfile_sync(mp_wfile *f, mp_err *e);    // forces the data to disk (fsync)
// Asks the system not to keep in memory what is written through f (macOS; elsewhere nothing is
// needed, see mp_file_open_read_uncached). 0, or -1 if the system refuses.
int mp_wfile_uncached(mp_wfile *f);
int mp_set_mtime(const char *utf8_path, int64_t unix_seconds, mp_err *e);
int mp_remove_file(const char *utf8_path);   // 0 if deleted
int mp_stat(const char *utf8_path, uint64_t *size, int64_t *mtime);   // 0 if it exists
// 1 if path is dir or is inside dir (by identity on disk, walking up its parent folders
// even if path does not exist yet).
int mp_path_is_inside(const char *path, const char *dir);
int mp_dir_is_empty(const char *utf8_path);    // 1 empty, 0 has contents, -1 does not exist
int mp_make_dirs(const char *utf8_path, mp_err *e);
// 0 = does not exist, 1 = file, 2 = folder.
int mp_path_kind(const char *utf8_path);

// Walks the folder recursively. rel_utf8 is relative to the root and uses '/'.
// Does not follow symbolic links or reparse points. If cb returns != 0, it stops
// and mp_dir_walk returns that value.
typedef int (*mp_dir_cb)(void *ctx, const char *rel_utf8, int is_dir, uint64_t size,
                         int64_t mtime_unix);
int mp_dir_walk(const char *root_utf8, mp_dir_cb cb, void *ctx, mp_err *e);

// A single level of the folder, without "." or "..", without links. -1 if it is not a folder.
typedef int (*mp_list_cb)(void *ctx, const char *name, int is_dir, uint64_t size);
int mp_dir_list(const char *dir_utf8, mp_list_cb cb, void *ctx, mp_err *e);
// Renames a file or folder within the same file system; fails if 'to' exists.
int mp_rename(const char *from_utf8, const char *to_utf8, mp_err *e);
// Deletes a whole folder (without following links) or a file. 0 also if it does not exist.
int mp_remove_tree(const char *utf8_path, mp_err *e);
// Bytes free for the user on the file system of path (which must exist).
int mp_disk_free(const char *utf8_path, uint64_t *free_bytes, mp_err *e);

typedef struct mp_thread mp_thread;
int mp_thread_start(mp_thread **t, void (*fn)(void *), void *arg);
void mp_thread_join(mp_thread *t);

typedef struct mp_mutex mp_mutex;
mp_mutex *mp_mutex_new(void);
void mp_mutex_lock(mp_mutex *m);
void mp_mutex_unlock(mp_mutex *m);
void mp_mutex_free(mp_mutex *m);

// Condition variable, used with an mp_mutex held by the caller. mp_cond_wait releases the mutex while
// it waits and takes it again before returning; it may also return without a signal, so the condition
// is checked again in a loop.
typedef struct mp_cond mp_cond;
mp_cond *mp_cond_new(void);   // NULL if it cannot be created
void mp_cond_wait(mp_cond *c, mp_mutex *m);
void mp_cond_signal(mp_cond *c);      // wakes one waiting thread
void mp_cond_broadcast(mp_cond *c);   // wakes all of them
void mp_cond_free(mp_cond *c);

uint64_t mp_now_ns(void);
unsigned mp_cpu_count(void);
void mp_sleep_ms(unsigned ms);

// On Windows replaces argc/argv with the real arguments in UTF-8 (argv arrives in the ANSI code
// page and loses characters like é or emoji). Elsewhere it does nothing.
void mp_args_utf8(int *argc, char ***argv);

// Absolute UTF-8 path of a file or folder, whether or not it exists (malloc). NULL if not possible.
char *mp_full_path(const char *utf8_path);
// 1 if both paths exist and are the same file or folder (same identity on disk,
// not the same text: case, "..", different slashes and links do not fool it).
int mp_same_file(const char *a, const char *b);

// Copy of a string with malloc (strdup is not C11).
char *mp_strdup(const char *s);

#endif
