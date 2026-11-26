// SPDX-License-Identifier: GPL-3.0-or-later
#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#endif

#include "mp_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *mp_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *join_rel(const char *prefix, const char *name) {
    size_t a = strlen(prefix), b = strlen(name);
    char *r = malloc(a + b + 2);
    if (!r) return NULL;
    if (a) {
        memcpy(r, prefix, a);
        r[a] = '/';
        memcpy(r + a + 1, name, b + 1);
    } else {
        memcpy(r, name, b + 1);
    }
    return r;
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static wchar_t *utf8_to_w(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n);
    return w;
}

static char *w_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc((size_t)n);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

// UTF-8 path -> absolute wide path with the \\?\ prefix (allows paths longer than 260 characters).
static wchar_t *long_path(const char *utf8) {
    wchar_t *w = utf8_to_w(utf8);
    if (!w) return NULL;
    for (wchar_t *p = w; *p; p++)
        if (*p == L'/') *p = L'\\';
    if (wcsncmp(w, L"\\\\?\\", 4) == 0) return w;
    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    if (n == 0) {
        free(w);
        return NULL;
    }
    wchar_t *full = malloc(((size_t)n + 8) * sizeof(wchar_t));
    if (!full) {
        free(w);
        return NULL;
    }
    wcscpy(full, L"\\\\?\\");
    DWORD m = GetFullPathNameW(w, n, full + 4, NULL);
    free(w);
    if (m == 0 || m >= n) {
        free(full);
        return NULL;
    }
    if (full[4] == L'\\' && full[5] == L'\\') {   // \\server\share -> \\?\UNC\server\share
        size_t rest = wcslen(full + 5);
        wchar_t *u = malloc((rest + 8) * sizeof(wchar_t));
        if (!u) {
            free(full);
            return NULL;
        }
        wcscpy(u, L"\\\\?\\UNC");
        wcscat(u, full + 5);
        free(full);
        full = u;
    }
    return full;
}

static void win_err(mp_err *e, const char *what, const char *path, DWORD code) {
    wchar_t *wmsg = NULL;
    char text[300] = "";
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, code, 0, (LPWSTR)&wmsg, 0, NULL) && wmsg) {
        char *m = w_to_utf8(wmsg);
        if (m) {
            size_t k = strlen(m);
            while (k && (m[k - 1] == '\n' || m[k - 1] == '\r' || m[k - 1] == ' ' || m[k - 1] == '.'))
                m[--k] = 0;
            snprintf(text, sizeof text, "%s", m);
            free(m);
        }
        LocalFree(wmsg);
    }
    if (!text[0]) snprintf(text, sizeof text, "code %lu", (unsigned long)code);
    mp_err_set(e, "%s %s: %s", what, path, text);
}

struct mp_file {
    HANDLE h;
};

static int open_read(const char *path, DWORD flags, mp_file **out, mp_err *e) {
    *out = NULL;
    wchar_t *w = long_path(path);
    if (!w) {
        mp_err_set(e, "invalid path: %s", path);
        return -1;
    }
    // FILE_FLAG_OVERLAPPED allows simultaneous reads from several threads with the same handle.
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | flags, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        win_err(e, "cannot open", path, GetLastError());
        return -1;
    }
    mp_file *f = malloc(sizeof *f);
    if (!f) {
        CloseHandle(h);
        mp_err_set(e, "out of memory");
        return -1;
    }
    f->h = h;
    *out = f;
    return 0;
}

int mp_file_open_read(const char *path, mp_file **out, mp_err *e) { return open_read(path, 0, out, e); }

int mp_file_open_read_uncached(const char *path, mp_file **out, mp_err *e) {
    return open_read(path, FILE_FLAG_NO_BUFFERING, out, e);
}

int mp_file_size(mp_file *f, uint64_t *out, mp_err *e) {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(f->h, &li)) {
        win_err(e, "cannot read the size of", "file", GetLastError());
        return -1;
    }
    *out = (uint64_t)li.QuadPart;
    return 0;
}

int mp_file_pread(mp_file *f, void *buf, size_t len, uint64_t off, size_t *got, mp_err *e) {
    size_t done = 0;
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ev) {
        win_err(e, "cannot create an event to", "read", GetLastError());
        return -1;
    }
    int rc = 0;
    while (done < len) {
        DWORD want = (DWORD)((len - done) > (1u << 30) ? (1u << 30) : (len - done));
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        uint64_t pos = off + done;
        ov.Offset = (DWORD)pos;
        ov.OffsetHigh = (DWORD)(pos >> 32);
        ov.hEvent = ev;
        DWORD n = 0;
        BOOL ok = ReadFile(f->h, (char *)buf + done, want, &n, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                ok = GetOverlappedResult(f->h, &ov, &n, TRUE);
                if (!ok) err = GetLastError();
            }
            if (!ok) {
                if (err == ERROR_HANDLE_EOF) break;
                win_err(e, "read error in", "file", err);
                rc = -1;
                break;
            }
        }
        if (n == 0) break;
        done += n;
    }
    CloseHandle(ev);
    *got = done;
    return rc;
}

void mp_file_close(mp_file *f) {
    if (!f) return;
    CloseHandle(f->h);
    free(f);
}

int mp_write_file(const char *path, const void *data, size_t len, mp_err *e) {
    wchar_t *w = long_path(path);
    if (!w) {
        mp_err_set(e, "invalid path: %s", path);
        return -1;
    }
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        win_err(e, "cannot create", path, GetLastError());
        return -1;
    }
    size_t done = 0;
    while (done < len) {
        DWORD want = (DWORD)((len - done) > (1u << 30) ? (1u << 30) : (len - done)), n = 0;
        if (!WriteFile(h, (const char *)data + done, want, &n, NULL) || n == 0) {
            win_err(e, "error writing", path, GetLastError());
            CloseHandle(h);
            return -1;
        }
        done += n;
    }
    CloseHandle(h);
    return 0;
}

int mp_path_kind(const char *path) {
    wchar_t *w = long_path(path);
    if (!w) return 0;
    DWORD a = GetFileAttributesW(w);
    free(w);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
}

static int make_one_dir(const char *path, mp_err *e) {
    wchar_t *w = long_path(path);
    if (!w) {
        mp_err_set(e, "invalid path: %s", path);
        return -1;
    }
    BOOL ok = CreateDirectoryW(w, NULL);
    DWORD err = ok ? 0 : GetLastError();
    free(w);
    if (!ok && err != ERROR_ALREADY_EXISTS) {
        win_err(e, "cannot create the folder", path, err);
        return -1;
    }
    return 0;
}

static int64_t filetime_to_unix(FILETIME ft) {
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)((t - 116444736000000000ULL) / 10000000ULL);
}

static int walk_w(const wchar_t *dir, const char *prefix, mp_dir_cb cb, void *ctx, mp_err *e) {
    size_t dl = wcslen(dir);
    wchar_t *pat = malloc((dl + 3) * sizeof(wchar_t));
    if (!pat) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    wcscpy(pat, dir);
    wcscat(pat, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                                FIND_FIRST_EX_LARGE_FETCH);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return 0;
        win_err(e, "cannot list", prefix[0] ? prefix : ".", err);
        return -1;
    }
    int rc = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        char *name = w_to_utf8(fd.cFileName);
        char *rel = name ? join_rel(prefix, name) : NULL;
        free(name);
        if (!rel) {
            mp_err_set(e, "out of memory");
            rc = -1;
            break;
        }
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        uint64_t size = is_dir ? 0 : (((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        rc = cb(ctx, rel, is_dir, size, filetime_to_unix(fd.ftLastWriteTime));
        if (!rc && is_dir) {
            size_t nl = wcslen(fd.cFileName);
            wchar_t *child = malloc((dl + nl + 2) * sizeof(wchar_t));
            if (!child) {
                mp_err_set(e, "out of memory");
                rc = -1;
            } else {
                wcscpy(child, dir);
                wcscat(child, L"\\");
                wcscat(child, fd.cFileName);
                rc = walk_w(child, rel, cb, ctx, e);
                free(child);
            }
        }
        free(rel);
        if (rc) break;
    } while (FindNextFileW(h, &fd));
    if (!rc && GetLastError() != ERROR_NO_MORE_FILES) {
        win_err(e, "error listing", prefix[0] ? prefix : ".", GetLastError());
        rc = -1;
    }
    FindClose(h);
    return rc;
}

int mp_dir_walk(const char *root, mp_dir_cb cb, void *ctx, mp_err *e) {
    wchar_t *w = long_path(root);
    if (!w) {
        mp_err_set(e, "invalid path: %s", root);
        return -1;
    }
    size_t n = wcslen(w);
    while (n > 4 && w[n - 1] == L'\\') w[--n] = 0;
    int rc = walk_w(w, "", cb, ctx, e);
    free(w);
    return rc;
}

struct mp_thread {
    HANDLE h;
    void (*fn)(void *);
    void *arg;
};

static DWORD WINAPI thread_tramp(LPVOID p) {
    mp_thread *t = p;
    t->fn(t->arg);
    return 0;
}

int mp_thread_start(mp_thread **out, void (*fn)(void *), void *arg) {
    mp_thread *t = malloc(sizeof *t);
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, 0, thread_tramp, t, 0, NULL);
    if (!t->h) {
        free(t);
        return -1;
    }
    *out = t;
    return 0;
}

void mp_thread_join(mp_thread *t) {
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

struct mp_mutex {
    SRWLOCK l;
};

mp_mutex *mp_mutex_new(void) {
    mp_mutex *m = malloc(sizeof *m);
    if (m) InitializeSRWLock(&m->l);
    return m;
}
void mp_mutex_lock(mp_mutex *m) { AcquireSRWLockExclusive(&m->l); }
void mp_mutex_unlock(mp_mutex *m) { ReleaseSRWLockExclusive(&m->l); }
void mp_mutex_free(mp_mutex *m) { free(m); }

struct mp_cond {
    CONDITION_VARIABLE cv;
};

mp_cond *mp_cond_new(void) {
    mp_cond *c = malloc(sizeof *c);
    if (c) InitializeConditionVariable(&c->cv);
    return c;
}
void mp_cond_wait(mp_cond *c, mp_mutex *m) { SleepConditionVariableSRW(&c->cv, &m->l, INFINITE, 0); }
void mp_cond_signal(mp_cond *c) { WakeConditionVariable(&c->cv); }
void mp_cond_broadcast(mp_cond *c) { WakeAllConditionVariable(&c->cv); }
void mp_cond_free(mp_cond *c) { free(c); }

uint64_t mp_now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    uint64_t f = (uint64_t)freq.QuadPart, v = (uint64_t)c.QuadPart;
    return (v / f) * 1000000000ULL + (v % f) * 1000000000ULL / f;
}

unsigned mp_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (unsigned)n : 1;
}

void mp_sleep_ms(unsigned ms) { Sleep(ms); }

#include <shellapi.h>

char *mp_full_path(const char *utf8) {
    wchar_t *w = utf8_to_w(utf8);
    if (!w) return NULL;
    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    wchar_t *full = n ? malloc((size_t)n * sizeof(wchar_t)) : NULL;
    char *out = NULL;
    if (full && GetFullPathNameW(w, n, full, NULL) > 0) out = w_to_utf8(full);
    free(full);
    free(w);
    return out;
}

static int file_id(const char *path, BY_HANDLE_FILE_INFORMATION *info) {
    wchar_t *w = long_path(path);
    if (!w) return 0;
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BOOL ok = GetFileInformationByHandle(h, info);
    CloseHandle(h);
    return ok != 0;
}

int mp_same_file(const char *a, const char *b) {
    BY_HANDLE_FILE_INFORMATION x, y;
    if (!file_id(a, &x) || !file_id(b, &y)) return 0;
    return x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh &&
           x.nFileIndexLow == y.nFileIndexLow;
}

struct mp_wfile {
    HANDLE h;
    uint64_t size;
    char *path;
};

int mp_wfile_create(const char *path, mp_wfile **out, mp_err *e) {
    *out = NULL;
    wchar_t *w = long_path(path);
    if (!w) {
        mp_err_set(e, "invalid path: %s", path);
        return -1;
    }
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        win_err(e, "cannot create", path, GetLastError());
        return -1;
    }
    mp_wfile *f = calloc(1, sizeof *f);
    if (!f || !(f->path = mp_strdup(path))) {
        free(f);
        CloseHandle(h);
        mp_err_set(e, "out of memory");
        return -1;
    }
    f->h = h;
    *out = f;
    return 0;
}

int mp_wfile_reopen(const char *path, mp_wfile **out, mp_err *e) {
    *out = NULL;
    wchar_t *w = long_path(path);
    if (!w) {
        mp_err_set(e, "invalid path: %s", path);
        return -1;
    }
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        win_err(e, "cannot open for writing", path, GetLastError());
        return -1;
    }
    LARGE_INTEGER li;
    mp_wfile *f = calloc(1, sizeof *f);
    if (!f || !(f->path = mp_strdup(path)) || !GetFileSizeEx(h, &li)) {
        if (f) free(f->path);
        free(f);
        CloseHandle(h);
        mp_err_set(e, "cannot open %s", path);
        return -1;
    }
    f->h = h;
    f->size = (uint64_t)li.QuadPart;
    *out = f;
    return 0;
}

int mp_wfile_reopen_locked(const char *path, mp_wfile **out, int *busy, mp_err *e) {
    *out = NULL;
    *busy = 0;
    wchar_t *w = long_path(path);
    if (!w) {
        mp_err_set(e, "invalid path: %s", path);
        return -1;
    }
    // GENERIC_WRITE shared only for reading: a second writer meets a sharing violation
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        *busy = code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
        if (*busy)
            mp_err_set(e, "another program is writing %s", path);
        else
            win_err(e, "cannot open for writing", path, code);
        return -1;
    }
    LARGE_INTEGER li;
    mp_wfile *f = calloc(1, sizeof *f);
    if (!f || !(f->path = mp_strdup(path)) || !GetFileSizeEx(h, &li)) {
        if (f) free(f->path);
        free(f);
        CloseHandle(h);
        mp_err_set(e, "cannot open %s", path);
        return -1;
    }
    f->h = h;
    f->size = (uint64_t)li.QuadPart;
    *out = f;
    return 0;
}

int mp_wfile_truncate(mp_wfile *f, uint64_t size, mp_err *e) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx(f->h, li, NULL, FILE_BEGIN) || !SetEndOfFile(f->h)) {
        win_err(e, "cannot cut", f->path, GetLastError());
        return -1;
    }
    f->size = size;
    return 0;
}

int mp_wfile_pwrite(mp_wfile *f, const void *p, size_t n, uint64_t off, mp_err *e) {
    size_t done = 0;
    while (done < n) {
        DWORD want = (DWORD)((n - done) > (1u << 30) ? (1u << 30) : (n - done)), got = 0;
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        uint64_t pos = off + done;
        ov.Offset = (DWORD)pos;
        ov.OffsetHigh = (DWORD)(pos >> 32);
        if (!WriteFile(f->h, (const char *)p + done, want, &got, &ov) || got == 0) {
            win_err(e, "error writing", f->path, GetLastError());
            return -1;
        }
        done += got;
    }
    if (off + n > f->size) f->size = off + n;
    return 0;
}

int mp_wfile_close(mp_wfile *f, mp_err *e) {
    if (!f) return 0;
    int rc = 0;
    if (!CloseHandle(f->h)) {
        win_err(e, "error closing", f->path, GetLastError());
        rc = -1;
    }
    free(f->path);
    free(f);
    return rc;
}

int mp_set_mtime(const char *path, int64_t t, mp_err *e) {
    wchar_t *w = long_path(path);
    if (!w) {
        mp_err_set(e, "invalid path: %s", path);
        return -1;
    }
    HANDLE h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        win_err(e, "cannot change the date of", path, GetLastError());
        return -1;
    }
    uint64_t v = (uint64_t)(t * 10000000LL) + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)v;
    ft.dwHighDateTime = (DWORD)(v >> 32);
    BOOL ok = SetFileTime(h, NULL, NULL, &ft);
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) {
        win_err(e, "cannot change the date of", path, err);
        return -1;
    }
    return 0;
}

int mp_remove_file(const char *path) {
    wchar_t *w = long_path(path);
    if (!w) return -1;
    BOOL ok = DeleteFileW(w);
    free(w);
    return ok ? 0 : -1;
}

int mp_stat(const char *path, uint64_t *size, int64_t *mtime) {
    wchar_t *w = long_path(path);
    if (!w) return -1;
    WIN32_FILE_ATTRIBUTE_DATA a;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    free(w);
    if (!ok) return -1;
    if (size) *size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    if (mtime) *mtime = filetime_to_unix(a.ftLastWriteTime);
    return 0;
}

static int has_entry_cb(void *ctx, const char *rel, int is_dir, uint64_t size, int64_t mtime) {
    (void)ctx;
    (void)rel;
    (void)is_dir;
    (void)size;
    (void)mtime;
    return 1;   // the first entry is enough
}

int mp_dir_is_empty(const char *path) {
    if (mp_path_kind(path) != 2) return -1;
    return mp_dir_walk(path, has_entry_cb, NULL, NULL) == 0 ? 1 : 0;
}

int mp_wfile_sync(mp_wfile *f, mp_err *e) {
    if (!FlushFileBuffers(f->h)) {
        win_err(e, "could not flush to disk", f->path, GetLastError());
        return -1;
    }
    return 0;
}

int mp_wfile_uncached(mp_wfile *f) {
    (void)f;   // reading with FILE_FLAG_NO_BUFFERING goes past what the cache keeps
    return 0;
}

int mp_dir_list(const char *dir, mp_list_cb cb, void *ctx, mp_err *e) {
    if (mp_path_kind(dir) != 2) {
        mp_err_set(e, "not a folder: %s", dir);
        return -1;
    }
    wchar_t *w = long_path(dir);
    size_t dl = w ? wcslen(w) : 0;
    wchar_t *pat = w ? malloc((dl + 3) * sizeof(wchar_t)) : NULL;
    if (!pat) {
        free(w);
        mp_err_set(e, "out of memory");
        return -1;
    }
    wcscpy(pat, w);
    if (dl && pat[dl - 1] != L'\\') wcscat(pat, L"\\");
    wcscat(pat, L"*");
    free(w);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return 0;
        win_err(e, "cannot list", dir, err);
        return -1;
    }
    int rc = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        char *name = w_to_utf8(fd.cFileName);
        if (!name) {
            mp_err_set(e, "out of memory");
            rc = -1;
            break;
        }
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        uint64_t size = is_dir ? 0 : (((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        rc = cb(ctx, name, is_dir, size);
        free(name);
    } while (!rc && FindNextFileW(h, &fd));
    FindClose(h);
    return rc;
}

int mp_rename(const char *from, const char *to, mp_err *e) {
    wchar_t *a = long_path(from), *b = long_path(to);
    int rc = 0;
    if (!a || !b) {
        mp_err_set(e, "invalid path: %s", !a ? from : to);
        rc = -1;
    } else if (!MoveFileExW(a, b, 0)) {
        win_err(e, "cannot rename", from, GetLastError());
        rc = -1;
    }
    free(a);
    free(b);
    return rc;
}

static int remove_tree_w(const wchar_t *path, const char *shown, mp_err *e) {
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, attr & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!DeleteFileW(path)) {
            win_err(e, "cannot delete", shown, GetLastError());
            return -1;
        }
        return 0;
    }
    if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {   // a link is deleted without entering it
        size_t dl = wcslen(path);
        wchar_t *pat = malloc((dl + 3) * sizeof(wchar_t));
        if (!pat) {
            mp_err_set(e, "out of memory");
            return -1;
        }
        wcscpy(pat, path);
        wcscat(pat, L"\\*");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
        free(pat);
        int rc = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                size_t nl = wcslen(fd.cFileName);
                wchar_t *child = malloc((dl + nl + 2) * sizeof(wchar_t));
                if (!child) {
                    mp_err_set(e, "out of memory");
                    rc = -1;
                    break;
                }
                wcscpy(child, path);
                wcscat(child, L"\\");
                wcscat(child, fd.cFileName);
                rc = remove_tree_w(child, shown, e);
                free(child);
            } while (!rc && FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (rc) return rc;
    }
    if (!RemoveDirectoryW(path)) {
        win_err(e, "cannot delete the folder", shown, GetLastError());
        return -1;
    }
    return 0;
}

int mp_remove_tree(const char *path, mp_err *e) {
    wchar_t *w = long_path(path);
    if (!w) {
        mp_err_set(e, "invalid path: %s", path);
        return -1;
    }
    int rc = remove_tree_w(w, path, e);
    free(w);
    return rc;
}

int mp_disk_free(const char *path, uint64_t *free_bytes, mp_err *e) {
    wchar_t *w = long_path(path);
    ULARGE_INTEGER avail = {0};
    BOOL ok = w && GetDiskFreeSpaceExW(w, &avail, NULL, NULL);
    DWORD err = GetLastError();
    free(w);
    if (!ok) {
        win_err(e, "cannot get the free space of", path, err);
        return -1;
    }
    *free_bytes = avail.QuadPart;
    return 0;
}

void mp_args_utf8(int *argc, char ***argv) {
    int n = 0;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return;
    char **v = calloc((size_t)n + 1, sizeof *v);
    if (!v) {
        LocalFree(w);
        return;
    }
    for (int i = 0; i < n; i++)
        if (!(v[i] = w_to_utf8(w[i]))) {
            LocalFree(w);
            return;   // keeps the original arguments (v is lost: only at startup)
        }
    LocalFree(w);
    *argc = n;
    *argv = v;
}

#else /* POSIX */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

struct mp_file {
    int fd;
};

int mp_file_open_read(const char *path, mp_file **out, mp_err *e) {
    *out = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        mp_err_set(e, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    mp_file *f = malloc(sizeof *f);
    if (!f) {
        close(fd);
        mp_err_set(e, "out of memory");
        return -1;
    }
    f->fd = fd;
    *out = f;
    return 0;
}

int mp_file_open_read_uncached(const char *path, mp_file **out, mp_err *e) {
    if (mp_file_open_read(path, out, e) != 0) return -1;
#if defined(__APPLE__)
    fcntl((*out)->fd, F_NOCACHE, 1);
#elif defined(__linux__)
    posix_fadvise((*out)->fd, 0, 0, POSIX_FADV_DONTNEED);   // drops the pages of the file that are clean
#endif
    return 0;
}

int mp_file_size(mp_file *f, uint64_t *out, mp_err *e) {
    struct stat st;
    if (fstat(f->fd, &st) != 0) {
        mp_err_set(e, "cannot read the size: %s", strerror(errno));
        return -1;
    }
    *out = (uint64_t)st.st_size;
    return 0;
}

int mp_file_pread(mp_file *f, void *buf, size_t len, uint64_t off, size_t *got, mp_err *e) {
    size_t done = 0;
    while (done < len) {
        size_t want = len - done > (1u << 30) ? (1u << 30) : len - done;
        ssize_t n = pread(f->fd, (char *)buf + done, want, (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            *got = done;
            mp_err_set(e, "read error: %s", strerror(errno));
            return -1;
        }
        if (n == 0) break;
        done += (size_t)n;
    }
    *got = done;
    return 0;
}

void mp_file_close(mp_file *f) {
    if (!f) return;
    close(f->fd);
    free(f);
}

int mp_write_file(const char *path, const void *data, size_t len, mp_err *e) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        mp_err_set(e, "cannot create %s: %s", path, strerror(errno));
        return -1;
    }
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, (const char *)data + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            mp_err_set(e, "error writing %s: %s", path, strerror(errno));
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    if (close(fd) != 0) {
        mp_err_set(e, "error closing %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int mp_path_kind(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

static int make_one_dir(const char *path, mp_err *e) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        mp_err_set(e, "cannot create the folder %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int walk_p(const char *dir, const char *prefix, mp_dir_cb cb, void *ctx, mp_err *e) {
    DIR *d = opendir(dir);
    if (!d) {
        mp_err_set(e, "cannot list %s: %s", dir, strerror(errno));
        return -1;
    }
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *full = join_rel(dir, de->d_name);
        char *rel = join_rel(prefix, de->d_name);
        struct stat st;
        if (!full || !rel) {
            mp_err_set(e, "out of memory");
            rc = -1;
        } else if (lstat(full, &st) != 0) {
            mp_err_set(e, "cannot read %s: %s", full, strerror(errno));
            rc = -1;
        } else if (S_ISDIR(st.st_mode)) {
            rc = cb(ctx, rel, 1, 0, (int64_t)st.st_mtime);
            if (!rc) rc = walk_p(full, rel, cb, ctx, e);
        } else if (S_ISREG(st.st_mode)) {
            rc = cb(ctx, rel, 0, (uint64_t)st.st_size, (int64_t)st.st_mtime);
        }
        free(full);
        free(rel);
        if (rc) break;
    }
    closedir(d);
    return rc;
}

int mp_dir_walk(const char *root, mp_dir_cb cb, void *ctx, mp_err *e) {
    char *r = mp_strdup(root);
    if (!r) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    size_t n = strlen(r);
    while (n > 1 && r[n - 1] == '/') r[--n] = 0;
    int rc = walk_p(r, "", cb, ctx, e);
    free(r);
    return rc;
}

struct mp_thread {
    pthread_t t;
    void (*fn)(void *);
    void *arg;
};

static void *thread_tramp(void *p) {
    mp_thread *t = p;
    t->fn(t->arg);
    return NULL;
}

int mp_thread_start(mp_thread **out, void (*fn)(void *), void *arg) {
    mp_thread *t = malloc(sizeof *t);
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->t, NULL, thread_tramp, t) != 0) {
        free(t);
        return -1;
    }
    *out = t;
    return 0;
}

void mp_thread_join(mp_thread *t) {
    pthread_join(t->t, NULL);
    free(t);
}

struct mp_mutex {
    pthread_mutex_t m;
};

mp_mutex *mp_mutex_new(void) {
    mp_mutex *m = malloc(sizeof *m);
    if (m) pthread_mutex_init(&m->m, NULL);
    return m;
}
void mp_mutex_lock(mp_mutex *m) { pthread_mutex_lock(&m->m); }
void mp_mutex_unlock(mp_mutex *m) { pthread_mutex_unlock(&m->m); }
void mp_mutex_free(mp_mutex *m) {
    if (!m) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

struct mp_cond {
    pthread_cond_t c;
};

mp_cond *mp_cond_new(void) {
    mp_cond *c = malloc(sizeof *c);
    if (c && pthread_cond_init(&c->c, NULL) != 0) {
        free(c);
        c = NULL;
    }
    return c;
}
void mp_cond_wait(mp_cond *c, mp_mutex *m) { pthread_cond_wait(&c->c, &m->m); }
void mp_cond_signal(mp_cond *c) { pthread_cond_signal(&c->c); }
void mp_cond_broadcast(mp_cond *c) { pthread_cond_broadcast(&c->c); }
void mp_cond_free(mp_cond *c) {
    if (!c) return;
    pthread_cond_destroy(&c->c);
    free(c);
}

uint64_t mp_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

unsigned mp_cpu_count(void) {
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (unsigned)n;
#endif
    return 1;
}

void mp_args_utf8(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
}

char *mp_full_path(const char *path) {
    char *r = realpath(path, NULL);
    if (r) return r;
    // Does not exist: resolved parent folder + name.
    const char *slash = strrchr(path, '/');
    char *dir = slash ? malloc((size_t)(slash - path) + 2) : mp_strdup(".");
    if (!dir) return NULL;
    if (slash) {
        size_t k = (size_t)(slash - path);
        memcpy(dir, path, k ? k : 1);
        dir[k ? k : 1] = 0;
    }
    char *rd = realpath(dir, NULL);
    free(dir);
    if (!rd) return NULL;
    const char *base = slash ? slash + 1 : path;
    char *out = malloc(strlen(rd) + strlen(base) + 2);
    if (out) sprintf(out, "%s/%s", strcmp(rd, "/") ? rd : "", base);
    free(rd);
    return out;
}

int mp_same_file(const char *a, const char *b) {
    struct stat x, y;
    if (stat(a, &x) != 0 || stat(b, &y) != 0) return 0;
    return x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

struct mp_wfile {
    int fd;
    uint64_t size;
    char *path;
};

int mp_wfile_create(const char *path, mp_wfile **out, mp_err *e) {
    *out = NULL;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        mp_err_set(e, "cannot create %s: %s", path, strerror(errno));
        return -1;
    }
    mp_wfile *f = calloc(1, sizeof *f);
    if (!f || !(f->path = mp_strdup(path))) {
        free(f);
        close(fd);
        mp_err_set(e, "out of memory");
        return -1;
    }
    f->fd = fd;
    *out = f;
    return 0;
}

int mp_wfile_reopen(const char *path, mp_wfile **out, mp_err *e) {
    *out = NULL;
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        mp_err_set(e, "cannot open %s for writing: %s", path, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    mp_wfile *f = calloc(1, sizeof *f);
    if (!f || !(f->path = mp_strdup(path))) {
        free(f);
        close(fd);
        mp_err_set(e, "out of memory");
        return -1;
    }
    f->fd = fd;
    f->size = (uint64_t)st.st_size;
    *out = f;
    return 0;
}

int mp_wfile_reopen_locked(const char *path, mp_wfile **out, int *busy, mp_err *e) {
    *out = NULL;
    *busy = 0;
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        mp_err_set(e, "cannot open %s for writing: %s", path, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    // flock locks the open file, not the process: a second open in the same process is refused too
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        *busy = errno == EWOULDBLOCK || errno == EAGAIN;
        if (*busy)
            mp_err_set(e, "another program is writing %s", path);
        else
            mp_err_set(e, "cannot lock %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    mp_wfile *f = calloc(1, sizeof *f);
    if (!f || !(f->path = mp_strdup(path))) {
        free(f);
        close(fd);
        mp_err_set(e, "out of memory");
        return -1;
    }
    f->fd = fd;
    f->size = (uint64_t)st.st_size;
    *out = f;
    return 0;
}

int mp_wfile_truncate(mp_wfile *f, uint64_t size, mp_err *e) {
    if (ftruncate(f->fd, (off_t)size) != 0) {
        mp_err_set(e, "cannot cut %s: %s", f->path, strerror(errno));
        return -1;
    }
    f->size = size;
    return 0;
}

int mp_wfile_pwrite(mp_wfile *f, const void *p, size_t n, uint64_t off, mp_err *e) {
    size_t done = 0;
    while (done < n) {
        ssize_t k = pwrite(f->fd, (const char *)p + done, n - done, (off_t)(off + done));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) {
            mp_err_set(e, "error writing %s: %s", f->path, strerror(errno));
            return -1;
        }
        done += (size_t)k;
    }
    if (off + n > f->size) f->size = off + n;
    return 0;
}

int mp_wfile_close(mp_wfile *f, mp_err *e) {
    if (!f) return 0;
    int rc = 0;
    if (close(f->fd) != 0) {
        mp_err_set(e, "error closing %s: %s", f->path, strerror(errno));
        rc = -1;
    }
    free(f->path);
    free(f);
    return rc;
}

int mp_set_mtime(const char *path, int64_t t, mp_err *e) {
    struct timespec ts[2];
    ts[0].tv_sec = 0;
    ts[0].tv_nsec = UTIME_OMIT;   // the access time is left alone
    ts[1].tv_sec = (time_t)t;
    ts[1].tv_nsec = 0;
    if (utimensat(AT_FDCWD, path, ts, 0) != 0) {
        mp_err_set(e, "cannot change the date of %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int mp_remove_file(const char *path) { return unlink(path) == 0 ? 0 : -1; }

int mp_stat(const char *path, uint64_t *size, int64_t *mtime) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (size) *size = (uint64_t)st.st_size;
    if (mtime) *mtime = (int64_t)st.st_mtime;
    return 0;
}

int mp_dir_is_empty(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *de;
    int empty = 1;
    while ((de = readdir(d)) != NULL)
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
            empty = 0;
            break;
        }
    closedir(d);
    return empty;
}

void mp_sleep_ms(unsigned ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

int mp_wfile_sync(mp_wfile *f, mp_err *e) {
    if (fsync(f->fd) != 0) {
        mp_err_set(e, "could not flush %s to disk: %s", f->path, strerror(errno));
        return -1;
    }
    return 0;
}

int mp_wfile_uncached(mp_wfile *f) {
#if defined(__APPLE__)
    return fcntl(f->fd, F_NOCACHE, 1) == -1 ? -1 : 0;
#else
    (void)f;
    return 0;
#endif
}

int mp_dir_list(const char *dir, mp_list_cb cb, void *ctx, mp_err *e) {
    DIR *d = opendir(dir);
    if (!d) {
        mp_err_set(e, "cannot list %s: %s", dir, strerror(errno));
        return -1;
    }
    int rc = 0;
    struct dirent *de;
    while (!rc && (de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *full = join_rel(dir, de->d_name);
        struct stat st;
        if (!full) {
            mp_err_set(e, "out of memory");
            rc = -1;
        } else if (lstat(full, &st) == 0 && (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) {
            rc = cb(ctx, de->d_name, S_ISDIR(st.st_mode), S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size);
        }
        free(full);
    }
    closedir(d);
    return rc;
}

int mp_rename(const char *from, const char *to, mp_err *e) {
    struct stat st;
    if (lstat(to, &st) == 0) {
        mp_err_set(e, "cannot rename %s: %s already exists", from, to);
        return -1;
    }
    if (rename(from, to) != 0) {
        mp_err_set(e, "cannot rename %s: %s", from, strerror(errno));
        return -1;
    }
    return 0;
}

int mp_remove_tree(const char *path, mp_err *e) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) {   // file or link: deleted without following it
        if (unlink(path) != 0) {
            mp_err_set(e, "cannot delete %s: %s", path, strerror(errno));
            return -1;
        }
        return 0;
    }
    DIR *d = opendir(path);
    if (!d) {
        mp_err_set(e, "cannot list %s: %s", path, strerror(errno));
        return -1;
    }
    int rc = 0;
    struct dirent *de;
    while (!rc && (de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *full = join_rel(path, de->d_name);
        if (!full) {
            mp_err_set(e, "out of memory");
            rc = -1;
        } else {
            rc = mp_remove_tree(full, e);
        }
        free(full);
    }
    closedir(d);
    if (!rc && rmdir(path) != 0) {
        mp_err_set(e, "cannot delete the folder %s: %s", path, strerror(errno));
        rc = -1;
    }
    return rc;
}

int mp_disk_free(const char *path, uint64_t *free_bytes, mp_err *e) {
    struct statvfs s;
    if (statvfs(path, &s) != 0) {
        mp_err_set(e, "cannot get the free space of %s: %s", path, strerror(errno));
        return -1;
    }
    *free_bytes = (uint64_t)s.f_bavail * (uint64_t)s.f_frsize;
    return 0;
}

#endif

int mp_make_dirs(const char *path, mp_err *e) {
    char *p = mp_strdup(path);
    if (!p) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    size_t n = strlen(p);
    int rc = 0;
    for (size_t i = 1; i <= n && !rc; i++) {
        if (i < n && !is_sep(p[i])) continue;
        char saved = p[i];
        p[i] = 0;
        // Prefixes that are not creatable folders are skipped: "C:" or the root.
        if (!(i == 2 && p[1] == ':') && !(i == 1 && is_sep(p[0])) && mp_path_kind(p) != 2)
            rc = make_one_dir(p, e);
        p[i] = saved;
    }
    free(p);
    return rc;
}

int mp_wfile_write(mp_wfile *f, const void *p, size_t n, mp_err *e) {
    return mp_wfile_pwrite(f, p, n, f->size, e);
}

uint64_t mp_wfile_size(const mp_wfile *f) { return f->size; }

// Leaves p at its parent folder (C:\a\b -> C:\a, C:\a -> C:\, /a -> /). 0 if it is already a root.
static int to_parent_path(char *p) {
    size_t n = strlen(p);
    while (n > 0 && is_sep(p[n - 1])) n--;
    size_t end = n;
    while (n > 0 && !is_sep(p[n - 1])) n--;
    if (n == 0 || n == end) return 0;
    size_t sep = n - 1;
    if (sep == 0 || (sep == 2 && p[1] == ':'))
        p[n] = 0;
    else
        p[sep] = 0;
    return 1;
}

int mp_path_is_inside(const char *path, const char *dir) {
    char *p = mp_full_path(path);
    if (!p) return 0;
    int inside = 0;
    do {
        if (mp_same_file(p, dir)) {
            inside = 1;
            break;
        }
    } while (to_parent_path(p));
    free(p);
    return inside;
}
