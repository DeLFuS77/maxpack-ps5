// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_report_out.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "mp_platform.h"
#include "mp_version.h"

static const char *const cfg_label[MP_CFG_COUNT] = {
    "v1 level 5 (Forge)",        "v1 level 9",
    "v1 level 19",               "v1 level 22",
    "v2 1 MiB block, l. 19", "v2 dictionary, l. 19",
    "zlib 64 KiB (≈ .ffpfsc)",
};

static double finite_or_zero(double v) { return isfinite(v) ? v : 0.0; }

static uint64_t console_total(const mp_report *r, int c) {
    return r->loose_bytes + r->est_packed[c] + (mp_cfg_is_v1((mp_cfg)c) ? r->v1_index_bytes : 0);
}

static double mb_per_s(const mp_cfg_stat *s) {
    return s->ns ? finite_or_zero((double)s->raw / ((double)s->ns / 1e9) / 1e6) : 0.0;
}

// ---- Text ----------------------------------------------------------------------------------

// Human-readable size in decimal units.
static const char *human(uint64_t b, char buf[32]) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int u = 0;
    while (v >= 1000.0 && u < 4) {
        v /= 1000.0;
        u++;
    }
    if (u == 0)
        snprintf(buf, 32, "%" PRIu64 " B", b);
    else
        snprintf(buf, 32, "%.2f %s", v, units[u]);
    return buf;
}

static const char *duration(uint64_t s, char buf[32]) {
    if (s >= 3600)
        snprintf(buf, 32, "%" PRIu64 " h %02" PRIu64 " min", s / 3600, (s / 60) % 60);
    else if (s >= 60)
        snprintf(buf, 32, "%" PRIu64 " min %02" PRIu64 " s", s / 60, s % 60);
    else
        snprintf(buf, 32, "%" PRIu64 " s", s);
    return buf;
}

static const char *pct(double v, char buf[16]) {
    snprintf(buf, 16, "%.1f%%", finite_or_zero(v));
    return buf;
}

// Visible columns of a UTF-8 string (printf pads by bytes, not by characters).
static int utf8_cols(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

static void put_padded(FILE *out, const char *s, int width) {
    fputs(s, out);
    for (int i = utf8_cols(s); i < width; i++) fputc(' ', out);
}

static const char *bin_label(const mp_size_bin *b, char buf[48]) {
    static const struct { uint64_t v; const char *s; } names[] = {
        {0, "0 B"},           {1, "1 B"},           {4096, "4 KiB"},     {4097, "4 KiB"},
        {65536, "64 KiB"},    {65537, "64 KiB"},    {262144, "256 KiB"}, {262145, "256 KiB"},
        {1u << 20, "1 MiB"},  {(1u << 20) + 1, "1 MiB"},  {64ull << 20, "64 MiB"},
        {(64ull << 20) + 1, "64 MiB"}, {4ull << 30, "4 GiB"}, {(4ull << 30) + 1, "4 GiB"},
    };
    const char *lo = "?", *hi = "?";
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        if (names[i].v == b->lo) lo = names[i].s;
        if (names[i].v == b->hi) hi = names[i].s;
    }
    if (b->hi == 0)
        snprintf(buf, 48, "empty");
    else if (b->hi == UINT64_MAX)
        snprintf(buf, 48, "over %s", lo);
    else if (b->lo <= 1)
        snprintf(buf, 48, "up to %s", hi);
    else
        snprintf(buf, 48, "%s – %s", lo, hi);
    return buf;
}

int mp_report_write_text(const mp_report *r, FILE *out) {
    char a[32], b[32], c[32], d[16];
    const char *kind = r->kind_text[0] ? r->kind_text : r->kind == MP_SRC_EXFAT ? "exFAT image" : "folder";
    fprintf(out, "MaxPack %s — analysis of %s (%s)\n", MP_VERSION,
            r->source_name ? r->source_name : "?", kind);
    char line[256];
    fprintf(out, "%" PRIu64 " files in %" PRIu64 " folders · %s\n", r->files, r->dirs, human(r->total_bytes, a));
    mp_complete_summary(&r->complete, line, sizeof line);
    fprintf(out, "Completeness: %s\n", line);
    mp_complete_extra_line(&r->complete, line, sizeof line);
    if (line[0]) fprintf(out, "  %s\n", line);
    fprintf(out, "\n");
    fprintf(out, "Loose (goes to the console as is):  %8" PRIu64 " files  %s\n", r->loose_files,
            human(r->loose_bytes, a));
    fprintf(out, "Packable:                           %8" PRIu64 " files  %s\n", r->packable_files,
            human(r->packable_bytes, a));
    if (r->full || r->sample_rate >= 1.0)
        fprintf(out, "Sample: everything (%s in %" PRIu64 " 1 MiB windows), %u threads\n\n",
                human(r->sample_bytes, a), r->sample_windows, r->threads);
    else
        fprintf(out, "Sample: %s (%s in %" PRIu64 " 1 MiB windows), %u threads\n\n",
                pct(100.0 * r->sample_rate, d), human(r->sample_bytes, a), r->sample_windows,
                r->threads);

    put_padded(out, "Configuration", 28);
    fprintf(out, " %12s %15s %9s %10s %14s\n", "Packed", "Console total", "Saved",
            "MB/s/thread", "Est. time");
    for (int k = 0; k < MP_CFG_COUNT; k++) {
        uint64_t tot = console_total(r, k);
        double save = r->total_bytes ? 100.0 * (1.0 - (double)tot / (double)r->total_bytes) : 0.0;
        char speed[16];
        snprintf(speed, sizeof speed, "%.1f", mb_per_s(&r->sampled[k]));
        put_padded(out, cfg_label[k], 28);
        fprintf(out, " %12s %15s %9s %10s %14s\n", human(r->est_packed[k], a), human(tot, b),
                pct(save, d), speed, duration(r->est_seconds[k], c));
    }
    fprintf(out, "Saved = 1 - console total / original size. Console total = loose files + "
                 "packed (+ indices in v1).\n");
    // a compressed image takes less than the game in it (a package, more or less): say what Saved is against
    if (r->image_size && r->kind == MP_SRC_FPKG)
        fprintf(out, "Saved is against the game in it (%s), not the package file (%s).\n", human(r->total_bytes, a),
                human(r->image_size, b));
    else if (r->image_size)
        fprintf(out, "Saved is against the game in it (%s), not the image file, which is compressed (%s).\n",
                human(r->total_bytes, a), human(r->image_size, b));
    fprintf(out, "\n");

    if (r->dedup_done)
        fprintf(out, "Exact deduplication (whole game): %" PRIu64 " of %" PRIu64
                     " 256 KiB blocks repeated (%s) · %" PRIu64 " identical files (%s)\n",
                r->dedup.dup_blocks, r->dedup.blocks, human(r->dedup.dup_block_bytes, a),
                r->dedup.dup_files, human(r->dedup.dup_file_bytes, b));
    else
        fprintf(out, "Deduplication: not measured (--no-dedup)\n");
    char x[32];
    fprintf(out, "Files < 64 KiB: %" PRIu64 " (%s) · one by one %s · grouped %s · with "
                 "dictionary %s\n",
            r->small.files, human(r->small.raw, a), human(r->small.individual, b),
            human(r->small.grouped, c), human(r->small.individual_dict, x));
    fprintf(out, "Trained dictionaries: blocks %d/2, small files %d/2\n", r->block_dicts,
            r->small.dicts);
    fprintf(out, "Loose files for the .p5z (zstd 19, long window): %s -> %s\n",
            human(r->loose_bytes, a), human(r->loose_compressed, b));
    fprintf(out, ".p5z estimate (v1 level 19): %s", human(r->p5z_estimate, a));
    if (r->image_size) {
        double change = 100.0 * ((double)r->p5z_estimate / (double)r->image_size - 1.0);
        fprintf(out, " (%s %s than the %s file)", pct(fabs(change), d), change > 0 ? "larger" : "smaller",
                r->kind == MP_SRC_FPKG ? "package" : "image");
    }
    fprintf(out, "\n");
    if (r->xport_done) {
        const mp_xport_stat *xs = &r->xport;
        double vs = r->p5z_estimate ? 100.0 * (1.0 - (double)r->p5z_transport / (double)r->p5z_estimate)
                                    : 0.0;
        char sp[16];
        snprintf(sp, sizeof sp, "%.1f",
                 xs->ns ? (double)xs->sample_raw / ((double)xs->ns / 1e9) / 1e6 : 0.0);
        fprintf(out, "Transport (.p5z for the PC only; the PS5 would recompress to v1 on install):\n");
        fprintf(out, "  packable with zstd 19, long window, 32 MiB frames: %s -> %s (v1 level 19: %s)\n",
                human(r->packable_bytes, a), human(xs->est_packed, b), human(r->est_packed[MP_CFG_V1_L19], c));
        fprintf(out, "  sample %s in %" PRIu64 " runs · %s MB/s/thread · est. time %s with %u threads\n",
                human(xs->sample_bytes, a), xs->frames, sp, duration(xs->est_seconds, b), xs->threads);
        fprintf(out, "  estimated transport .p5z: %s (%s smaller than the normal .p5z)\n",
                human(r->p5z_transport, a), pct(vs, d));
    }
    fputc('\n', out);

    fprintf(out, "Size distribution\n");
    for (int k = 0; k < MP_SIZE_BINS; k++) {
        char lab[48];
        fputs("  ", out);
        put_padded(out, bin_label(&r->bins[k], lab), 22);
        fprintf(out, " %10" PRIu64 " files  %s\n", r->bins[k].files, human(r->bins[k].bytes, a));
    }
    fprintf(out, "\nAnalysis time: %s\n", duration(r->elapsed_ns / 1000000000ull, a));
    return ferror(out) ? -1 : 0;
}

// ---- JSON ----------------------------------------------------------------------------------

typedef struct {
    char *p;
    size_t n, cap;
    int oom;
} sb;

static void sb_add(sb *s, const char *fmt, ...) {
    if (s->oom) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int k = vsnprintf(s->p + s->n, s->cap - s->n, fmt, ap);
        va_end(ap);
        if (k < 0) {
            s->oom = 1;
            return;
        }
        if ((size_t)k < s->cap - s->n) {
            s->n += (size_t)k;
            return;
        }
        size_t cap = (s->cap + (size_t)k + 1) * 2;
        char *q = realloc(s->p, cap);
        if (!q) {
            s->oom = 1;
            return;
        }
        s->p = q;
        s->cap = cap;
    }
}

static void sb_str(sb *s, const char *v) {
    sb_add(s, "\"");
    for (const unsigned char *p = (const unsigned char *)(v ? v : ""); *p; p++) {
        if (*p == '"' || *p == '\\')
            sb_add(s, "\\%c", *p);
        else if (*p < 0x20)
            sb_add(s, "\\u%04x", *p);
        else
            sb_add(s, "%c", *p);
    }
    sb_add(s, "\"");
}

static void sb_num(sb *s, double v) { sb_add(s, "%.6g", finite_or_zero(v)); }

char *mp_report_json(const mp_report *r) {
    sb s = {malloc(4096), 0, 4096, 0};
    if (!s.p) return NULL;
    s.p[0] = 0;
    sb_add(&s, "{\n  \"maxpack\": \"%s\",\n  \"source\": {\"name\": ", MP_VERSION);
    sb_str(&s, r->source_name);
    // A folder is "dir" (as before 1.0.5); an image, mp_source_kind_name's word, and for a container
    // the image inside.
    const char *kind = r->kind == MP_SRC_DIR || !r->kind_name[0] ? (r->kind == MP_SRC_EXFAT ? "exfat" : "dir")
                                                                   : r->kind_name;
    sb_add(&s, ", \"kind\": \"%s\", \"inner\": \"%s\"},\n", kind, r->inner_name);
    sb_add(&s,
           "  \"files\": {\"total\": %" PRIu64 ", \"dirs\": %" PRIu64 ", \"bytes\": %" PRIu64
           ", \"loose_files\": %" PRIu64 ", \"loose_bytes\": %" PRIu64
           ", \"packable_files\": %" PRIu64 ", \"packable_bytes\": %" PRIu64 ",\n"
           "    \"size_bins\": [",
           r->files, r->dirs, r->total_bytes, r->loose_files, r->loose_bytes, r->packable_files,
           r->packable_bytes);
    for (int k = 0; k < MP_SIZE_BINS; k++)
        sb_add(&s, "%s\n      {\"min\": %" PRIu64 ", \"max\": %" PRIu64 ", \"files\": %" PRIu64
                   ", \"bytes\": %" PRIu64 "}",
               k ? "," : "", r->bins[k].lo, r->bins[k].hi == UINT64_MAX ? 0 : r->bins[k].hi,
               r->bins[k].files, r->bins[k].bytes);
    sb_add(&s, "]},\n  \"sample\": {\"rate\": ");
    sb_num(&s, r->sample_rate);
    sb_add(&s, ", \"bytes\": %" PRIu64 ", \"windows\": %" PRIu64 ", \"threads\": %u, \"full\": %s},\n",
           r->sample_bytes, r->sample_windows, r->threads, r->full ? "true" : "false");
    sb_add(&s, "  \"configs\": {");
    for (int k = 0; k < MP_CFG_COUNT; k++) {
        sb_add(&s, "%s\n    \"%s\": {\"sample_raw\": %" PRIu64 ", \"sample_stored\": %" PRIu64
                   ", \"est_packed\": %" PRIu64 ", \"est_console_total\": %" PRIu64
                   ", \"est_seconds\": %" PRIu64 ", \"mb_per_s_per_thread\": ",
               k ? "," : "", mp_cfg_name((mp_cfg)k), r->sampled[k].raw, r->sampled[k].stored,
               r->est_packed[k], console_total(r, k), r->est_seconds[k]);
        sb_num(&s, mb_per_s(&r->sampled[k]));
        sb_add(&s, "}");
    }
    sb_add(&s, "},\n  \"dedup\": ");
    if (r->dedup_done)
        sb_add(&s, "{\"blocks\": %" PRIu64 ", \"dup_blocks\": %" PRIu64 ", \"dup_block_bytes\": %" PRIu64
                   ", \"dup_files\": %" PRIu64 ", \"dup_file_bytes\": %" PRIu64 "},\n",
               r->dedup.blocks, r->dedup.dup_blocks, r->dedup.dup_block_bytes, r->dedup.dup_files,
               r->dedup.dup_file_bytes);
    else
        sb_add(&s, "null,\n");
    sb_add(&s, "  \"transport\": ");
    if (r->xport_done) {
        const mp_xport_stat *x = &r->xport;
        sb_add(&s, "{\"frame_bytes\": %u, \"frames\": %" PRIu64 ", \"sample_bytes\": %" PRIu64
                   ", \"sample_raw\": %" PRIu64 ", \"sample_stored\": %" PRIu64
                   ", \"est_packed\": %" PRIu64 ", \"est_seconds\": %" PRIu64 ", \"threads\": %u"
                   ", \"p5z_estimate\": %" PRIu64 ", \"rate\": ",
               MP_LONG_FRAME, x->frames, x->sample_bytes, x->sample_raw, x->sample_stored,
               x->est_packed, x->est_seconds, x->threads, r->p5z_transport);
        sb_num(&s, x->rate);
        sb_add(&s, "},\n");
    } else {
        sb_add(&s, "null,\n");
    }
    sb_add(&s,
           "  \"small_files\": {\"files\": %" PRIu64 ", \"raw\": %" PRIu64 ", \"individual\": %" PRIu64
           ", \"grouped\": %" PRIu64 ", \"individual_dict\": %" PRIu64 ", \"dicts\": %d},\n",
           r->small.files, r->small.raw, r->small.individual, r->small.grouped,
           r->small.individual_dict, r->small.dicts);
    mp_jbuf cj = {0};
    mp_complete_json(&r->complete, 1, &cj);
    if (cj.oom || !cj.p)
        s.oom = 1;
    else
        sb_add(&s, "  \"completeness\": %s,\n", cj.p);
    mp_jbuf_free(&cj);
    sb_add(&s,
           "  \"block_dicts\": %d,\n  \"loose_compressed\": %" PRIu64 ",\n  \"v1_index_bytes\": %" PRIu64
           ",\n  \"p5z_estimate\": %" PRIu64 ",\n  \"elapsed_seconds\": ",
           r->block_dicts, r->loose_compressed, r->v1_index_bytes, r->p5z_estimate);
    sb_num(&s, (double)r->elapsed_ns / 1e9);
    sb_add(&s, "\n}\n");
    if (s.oom) {
        free(s.p);
        return NULL;
    }
    return s.p;
}

int mp_report_write_json(const mp_report *r, const char *utf8_path, mp_err *e) {
    char *j = mp_report_json(r);
    if (!j) {
        mp_err_set(e, "out of memory");
        return -1;
    }
    int rc = mp_write_file(utf8_path, j, strlen(j), e);
    free(j);
    return rc;
}
