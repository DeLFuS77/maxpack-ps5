// SPDX-License-Identifier: GPL-3.0-or-later
// Source of a game: a folder, an image (exFAT, UFS2 .ffpkg, PFS .ffpfs, or an .ffpfsc: a PFS image
// that holds one of the others, usually PFSC-compressed) or a PS5 debug package (FPKG, .pkg), seen
// as a list of files.
#ifndef MP_SOURCE_H
#define MP_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"

typedef struct {
    char *path;      // UTF-8, relative, with '/', no leading slash
    uint64_t size;
    int64_t mtime;   // Unix seconds
} mp_entry;

typedef enum { MP_SRC_DIR = 1, MP_SRC_EXFAT = 2, MP_SRC_UFS2 = 3, MP_SRC_PFS = 4, MP_SRC_FPKG = 5 } mp_source_kind;

typedef struct mp_source mp_source;

// Detects the type: folder, or an image or package by its signature (exFAT, UFS2, PFS, FPKG; the
// extension is only a hint for the message when it is none). A PFS image whose only file is another
// image is a container: the files are those of the image inside. A package is decrypted with the
// passcode mp_source_set_passcode gave (the default, 32 '0', if none); retail packages, patches,
// additional content and PS4 packages are refused, each with its message.
int mp_source_open(const char *utf8_path, mp_source **out, mp_err *e);
// What mp_source_open_as takes of packages: a game's, additional content's (a DLC), or either. A
// folder or an image opens the same whatever it is asked for.
enum { MP_OPEN_GAME = 1, MP_OPEN_DLC = 2, MP_OPEN_ANY = 3 };
// mp_source_open for the packages `what` says (mp_source_open is MP_OPEN_GAME).
int mp_source_open_as(const char *utf8_path, int what, mp_source **out, mp_err *e);
// 1 if it is a package of additional content (CNT content type 0x21).
int mp_source_is_dlc_package(const mp_source *s);
// A source made of others: the files of base, those of each of parts[0..nparts) under its folder
// prefixes[k] ("addcont0": "addcont0/data/a.bin"), and, when extra_path is not NULL, one file of the
// extra_len bytes given (copied) with that date. A file of a part for which skip (when not NULL) gives 1
// is left out. It takes base and every part, closed with it or on failure. Its kind, package ID and
// whether it is compressed are base's; its reading threads, the most any of them takes. A path that
// would be twice stops it ("<path> is twice").
int mp_source_merge(mp_source *base, mp_source *const *parts, const char *const *prefixes, size_t nparts,
                    int (*skip)(const char *path), const char *extra_path, const uint8_t *extra, size_t extra_len, int64_t extra_mtime,
                    mp_source **out, mp_err *e);
// The same with a passcode for a package (NULL or "" = the default).
int mp_source_open_passcode(const char *utf8_path, const char *passcode, mp_source **out, mp_err *e);
// The passcode mp_source_open uses for packages from now on (NULL or "" = the default). For a
// command-line process: set it once, before opening sources. 0, or -1 if it is not 32 printable
// ASCII characters without spaces.
int mp_source_set_passcode(const char *passcode);
// The kind of the file opened (MP_SRC_PFS for a container).
mp_source_kind mp_source_kind_of(const mp_source *s);
// "folder", "exfat", "ufs2", "pfs", "pfsc" for a PFS image that holds another image (.ffpfsc), or
// "fpkg" for a PS5 debug package.
const char *mp_source_kind_name(const mp_source *s);
// For "pfsc": the kind of the image inside that holds the files ("exfat", "ufs2" or "pfs"; the
// innermost when they nest); "" otherwise.
const char *mp_source_inner_name(const mp_source *s);
// For a package: the content ID its header gives ("UP0000-PPSA00000_00-..."); "" otherwise.
const char *mp_source_package_id(const mp_source *s);
// In words, for reports: "folder", "exFAT image", "UFS2 image", "PFS image", "PFS container with an
// exFAT image inside, compressed", "PS5 package (FPKG), compressed".
const char *mp_source_kind_text(const mp_source *s, char *buf, size_t n);
// 1 if reading the files decompresses PFSC blocks (or a package's Kraken blocks).
int mp_source_compressed(const mp_source *s);
// Reading threads the source can keep busy (a read-ahead uses up to this many): more when reading
// decompresses, since then each thread is busy inflating rather than waiting for the disk.
unsigned mp_source_readers(const mp_source *s);
size_t mp_source_count(const mp_source *s);
// Sorted by strcmp of the path's bytes.
const mp_entry *mp_source_entry(const mp_source *s, size_t i);
size_t mp_source_dir_count(const mp_source *s);
// Index of the entry with that exact path (binary search; entries are sorted), or -1.
long mp_source_find(const mp_source *s, const char *path);
// Reads exactly len bytes; off + len must be <= size. Safe across threads.
int mp_source_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e);
void mp_source_close(mp_source *s);

#endif
