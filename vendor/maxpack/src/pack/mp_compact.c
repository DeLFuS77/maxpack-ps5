// SPDX-License-Identifier: GPL-3.0-or-later
// compact (mp_grow.h): a new .p5z with what a .p5z uses and nothing else, nothing compressed again. Its
// records are copied as they are stored, in a fresh pack's order; the chunks of its packs go to new
// volumes, back to back in the index's order (mp_zpk_relayout), with the ampr_zstd.index that goes with
// them. What comes out is a .p5z of version 1, the one a fresh pack of the same dump with the same
// volume limit writes.
#include "mp_grow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_p5z_internal.h"

int mp_compact(const char *in, const char *out, unsigned threads, uint64_t volume_limit, int progress, uint64_t *out_size,
               mp_err *e) {
    *out_size = 0;
    if (mp_path_kind(out) != 0) {
        mp_err_set(e, "the output .p5z already exists: %s", out);
        return -1;
    }
    int what = 0;
    if (mp_p5z_recover(in, &what, e) != 0) return -1;
    if (what == MP_RECOVER_BUSY) {
        mp_err_set(e, "another MaxPack is changing %s now", in);
        return -1;
    }
    mp_p5z *p = NULL;
    mp_zpk *z = NULL;
    mp_pack_out w;
    memset(&w, 0, sizeof w);
    uint8_t *zidx = NULL, aid[16];
    size_t zlen = 0;
    int rc = -1, laid = 0;
    if (mp_p5z_open(in, &p, e) != 0) return -1;
    uint64_t total = 0;
    size_t zrec = SIZE_MAX;
    uint32_t nvol = 0;
    for (size_t i = 0; i < p->nfiles; i++) {
        if (p->files[i].role == MP_ROLE_ZPK_INDEX) zrec = i;
        if (p->files[i].role != MP_ROLE_VOLUME) total += p->files[i].data_len;
        nvol += p->files[i].role == MP_ROLE_VOLUME;
    }
    if (zrec != SIZE_MAX) {
        if (mp_p5z_zpk(p, &z, e) != 0) goto done;
        total += mp_zpk_stored_bytes(z);
    }
    // the volumes are copied as its packs lay them out: a volume they do not have, or a packed file with no
    // packs, would be left out of the copy without a word
    if ((z ? mp_zpk_volume_count(z) : 0) != nvol) {
        mp_err_set(e, "invalid .p5z: its volumes are not those of its packs");
        goto done;
    }
    for (size_t i = 0; !z && i < p->man.n; i++)
        if (p->man.v[i].flags & MP_MANIFEST_PACKED) {
            mp_err_set(e, "invalid .p5z: its manifest has files in packs it does not have (%s)", p->man.v[i].path);
            goto done;
        }
    if (mp_pack_out_p5z(out, threads, &w, e) != 0) goto done;
    mp_progress prog;
    mp_progress_init(&prog, "Copying the .p5z", total, progress);
    // the records of both versions are in a fresh pack's order: loose files, volumes, ampr_emu.index, the
    // runtime, the originals, ampr_zstd.index
    for (size_t i = 0; i < p->nfiles; i++) {
        const p5z_file *f = &p->files[i];
        if (mp_cancel_check(e) != 0) goto stop;
        if ((f->role == MP_ROLE_VOLUME || f->role == MP_ROLE_ZPK_INDEX) && !laid && z) {
            laid = 1;
            if (w.packs_begin && w.packs_begin(w.ctx, e) != 0) goto stop;
            if (mp_zpk_relayout(z, volume_limit, &w.zpk, &prog, &zidx, &zlen, aid, e) != 0) goto stop;
        }
        if (f->role == MP_ROLE_VOLUME) continue;
        if (f->role == MP_ROLE_ZPK_INDEX) {
            if (mp_pack_put_buffer(&w, f->path, MP_ROLE_ZPK_INDEX, zidx, zlen, f->mtime, e) != 0) goto stop;
            mp_progress_add(&prog, f->data_len);
            continue;
        }
        if (p5z_out_copy(&w, p, i, e) != 0) goto stop;
        mp_progress_add(&prog, f->data_len);
        mp_progress_maybe_print(&prog);
    }
    if (prog.print) fprintf(stderr, "\rCopying the .p5z: 100.0 %%\n");
    mp_pack_manifest m = p->man;   // its records are p's; only the archive_id is the new packs'
    if (laid) memcpy(m.archive_id, aid, 16);
    if (mp_cancel_check(e) != 0 || w.finish(w.ctx, &m, e) != 0) goto stop;
    rc = 0;
stop:
    mp_progress_free(&prog);
done:
    if (w.destroy) w.destroy(w.ctx);   // a .p5z not finished is deleted
    if (rc == 0 && mp_stat(out, out_size, NULL) != 0) *out_size = 0;
    free(zidx);
    mp_zpk_close(z);
    mp_p5z_close(p);
    return rc;
}
