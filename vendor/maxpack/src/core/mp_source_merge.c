// SPDX-License-Identifier: GPL-3.0-or-later
// A source made of others (mp_source_merge in mp_source.h): a game's files, those of other sources
// under a folder each, and one file whose bytes are given. Each read goes to the source the file is in.
#include <stdlib.h>
#include <string.h>

#include "mp_platform.h"
#include "mp_source_impl.h"

typedef struct {
    mp_source **src;   // [0] the base, then the parts
    size_t nsrc;
    uint32_t *from;    // of each entry: the source it is in (nsrc: the file given as bytes)
    size_t *at;        // and its index there
    uint8_t *extra;
    size_t extra_len;
} merged;

typedef struct {
    mp_entry en;
    uint32_t from;
    size_t at;
} mrec;

static int cmp_rec(const void *a, const void *b) { return strcmp(((const mrec *)a)->en.path, ((const mrec *)b)->en.path); }

static int merged_read(mp_source *s, size_t i, uint64_t off, void *buf, size_t len, mp_err *e) {
    merged *m = s->impl;
    if (m->from[i] == m->nsrc) {
        memcpy(buf, m->extra + off, len);
        return 0;
    }
    return mp_source_read(m->src[m->from[i]], m->at[i], off, buf, len, e);
}

static void merged_free(merged *m) {
    if (!m) return;
    for (size_t k = 0; k < m->nsrc; k++) mp_source_close(m->src[k]);
    free(m->src);
    free(m->from);
    free(m->at);
    free(m->extra);
    free(m);
}

static void merged_close(mp_source *s) { merged_free(s->impl); }

static const char *merged_id(const mp_source *s) { return mp_source_package_id(((const merged *)s->impl)->src[0]); }

int mp_source_merge(mp_source *base, mp_source *const *parts, const char *const *prefixes, size_t nparts,
                    int (*skip)(const char *path), const char *extra_path, const uint8_t *extra, size_t extra_len, int64_t extra_mtime,
                    mp_source **out, mp_err *e) {
    *out = NULL;
    merged *m = calloc(1, sizeof *m);
    mp_source *s = calloc(1, sizeof *s);
    if (m) m->src = calloc(nparts + 1, sizeof *m->src);
    if (!m || !s || !m->src) {
        free(s);
        if (m) free(m->src);
        free(m);
        mp_source_close(base);
        for (size_t k = 0; k < nparts; k++) mp_source_close(parts[k]);
        mp_err_set(e, "out of memory");
        return -1;
    }
    m->src[0] = base;
    for (size_t k = 0; k < nparts; k++) m->src[k + 1] = parts[k];
    m->nsrc = nparts + 1;
    size_t n = extra_path ? 1 : 0, ndirs = base->ndirs;
    for (size_t k = 0; k < m->nsrc; k++) n += m->src[k]->n;
    for (size_t k = 0; k < nparts; k++) ndirs += parts[k]->ndirs + 1;
    mrec *v = calloc(n ? n : 1, sizeof *v);
    size_t got = 0;
    int rc = v ? 0 : -1;
    for (size_t k = 0; rc == 0 && k < m->nsrc; k++) {
        const char *prefix = k ? prefixes[k - 1] : "";
        size_t pl = strlen(prefix);
        for (size_t i = 0; rc == 0 && i < m->src[k]->n; i++) {
            const mp_entry *en = &m->src[k]->entries[i];
            if (k && skip && skip(en->path)) continue;
            size_t len = strlen(en->path);
            char *path = malloc(pl + 1 + len + 1);
            if (!path) {
                rc = -1;
                break;
            }
            if (pl) {
                memcpy(path, prefix, pl);
                path[pl] = '/';
                memcpy(path + pl + 1, en->path, len + 1);
            } else {
                memcpy(path, en->path, len + 1);
            }
            v[got].en = *en;
            v[got].en.path = path;
            v[got].from = (uint32_t)k;
            v[got].at = i;
            got++;
        }
    }
    if (rc == 0 && extra_path) {
        if (!(v[got].en.path = mp_strdup(extra_path)) || (extra_len && !(m->extra = malloc(extra_len)))) {
            free(v[got].en.path);
            rc = -1;
        } else {
            if (extra_len) memcpy(m->extra, extra, extra_len);
            m->extra_len = extra_len;
            v[got].en.size = extra_len;
            v[got].en.mtime = extra_mtime;
            v[got].from = (uint32_t)m->nsrc;
            got++;
        }
    }
    if (rc != 0) mp_err_set(e, "out of memory");
    if (rc == 0 && got > 1) qsort(v, got, sizeof *v, cmp_rec);
    for (size_t i = 1; rc == 0 && i < got; i++)
        if (!strcmp(v[i - 1].en.path, v[i].en.path)) {
            mp_err_set(e, "%s is twice", v[i].en.path);
            rc = -1;
        }
    mp_entry *entries = NULL;
    if (rc == 0) {
        entries = malloc((got ? got : 1) * sizeof *entries);
        m->from = malloc((got ? got : 1) * sizeof *m->from);
        m->at = malloc((got ? got : 1) * sizeof *m->at);
        if (!entries || !m->from || !m->at) {
            mp_err_set(e, "out of memory");
            rc = -1;
        }
    }
    if (rc != 0) {
        for (size_t i = 0; v && i < got; i++) free(v[i].en.path);
        free(v);
        free(entries);
        free(s);
        merged_free(m);
        return -1;
    }
    unsigned readers = 0;
    for (size_t i = 0; i < got; i++) {
        entries[i] = v[i].en;
        m->from[i] = v[i].from;
        m->at[i] = v[i].at;
    }
    for (size_t k = 0; k < m->nsrc; k++)
        if (mp_source_readers(m->src[k]) > readers) readers = mp_source_readers(m->src[k]);
    free(v);
    s->kind = base->kind;
    s->inner = base->inner;
    s->container = base->container;
    s->compressed = base->compressed;
    s->dlc_package = base->dlc_package;
    s->readers = readers;
    s->package_id = base->package_id ? merged_id : NULL;
    s->entries = entries;
    s->n = got;
    s->ndirs = ndirs;
    s->read = merged_read;
    s->close = merged_close;
    s->impl = m;
    *out = s;
    return 0;
}
