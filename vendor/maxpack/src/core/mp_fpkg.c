// SPDX-License-Identifier: GPL-3.0-or-later
// A PS5 debug package's header, CNT and outer PFS (see mp_fpkg.h). Ported from pearlxcore/
// ProsperoPkgTool (MIT): Containers/ProsperoPackageReader.cs, Containers/OuterPfsReader.cs,
// Containers/ProsperoPackageContent.cs, Crypto/ProsperoKeys.cs, Crypto/ProsperoEntryCipher.cs and
// Content/ProsperoSceSysMedia.cs (the entry names); fthdgn-gh/Alembic's src/core/fpkg.cpp
// (GPL-3.0-or-later) as the second reading.
//
// FIH (little-endian): 0x05 signed byte (0x00 debug, 0x80 retail), 0x06 u16 version (3), 0x10 the
// outer PFS's offset, 0x18 its size, 0x20 0x10000 + the superblock's offset in it, 0x50 where the inner
// PFS's superblock is in the mount (64 KiB blocks), 0x58 the CNT's offset.
// CNT (big-endian): 0x10 entry count, 0x18 entry table offset, 0x40 content ID (36 characters), 0x74
// content type (0x20 a game, 0x21 additional content); rows of 0x20 bytes {id, name offset, flags1,
// flags2, data offset, data size, 8 zero bytes}; flags1 bit 31 = AES-CBC encrypted, key index in
// flags2 bits 12-15.
// Outer PFS: 64 KiB blocks, data first; the superblock plaintext (seed at 0x370, its SHA3 at 0x380),
// every other block one AES-128-XTS unit: pfs_image.dat's data with the block index as the sector,
// metadata with 0x800000000000 | index. S32 inodes of 0x2C8 bytes; inode 2 is uroot.
#include "mp_fpkg.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_platform.h"

#define CNT_HEADER 0x5A0
#define MAX_ENTRIES 0x10000
#define MAX_NAMES (16u << 20)
#define PFS_MAGIC 20130315ull
#define SIGNED_SECTOR 0x800000000000ull
#define S32_INODE 0x2C8
#define S32_DIRECT 0x64
#define S32_INDIRECT 0x214
#define SIG_STRIDE 36   // a 32-byte digest and a 32-bit block index
#define TREE_ENTRIES (MP_FPKG_BLOCK / SIG_STRIDE)
#define MAX_INODE_BLOCKS 16
#define MAX_UROOT (1u << 20)
#define MAX_NAPS (256u << 20)
#define MAX_ENTRY (64u << 20)   // an encrypted entry is decrypted whole

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static int oom(mp_err *e) {
    mp_err_set(e, "out of memory");
    return -1;
}

static int in_range(uint64_t off, uint64_t len, uint64_t total) { return off <= total && len <= total - off; }

static int read_exact(mp_image *m, void *buf, size_t len, uint64_t off, mp_err *e) {
    size_t got = 0;
    if (mp_image_pread(m, buf, len, off, &got, e) != 0) return -1;
    if (got != len) {
        mp_err_set(e, "truncated package: %s ends before its data", m->name);
        return -1;
    }
    return 0;
}

// ---- the names of the CNT entries (ProsperoSceSysMedia.cs's map, inverted) --------------------

static const struct {
    uint32_t id;
    const char *name;
} NAMES[] = {
    {0x0400, "license.dat"}, {0x0401, "license.info"}, {0x0402, "nptitle.dat"}, {0x0403, "npbind.dat"},
    {0x0404, "selfinfo.dat"}, {0x0406, "imageinfo.dat"}, {0x0407, "target-deltainfo.dat"},
    {0x0408, "origin-deltainfo.dat"}, {0x0409, "psreserved.dat"}, {0x1000, "param.sfo"},
    {0x1001, "playgo-chunk.dat"}, {0x1002, "playgo-chunk.sha"}, {0x1003, "playgo-manifest.xml"},
    {0x1004, "pronunciation.xml"}, {0x1005, "pronunciation.sig"}, {0x1006, "pic1.png"},
    {0x1007, "pubtoolinfo.dat"}, {0x1008, "app/playgo-chunk.dat"}, {0x1009, "app/playgo-chunk.sha"},
    {0x100A, "app/playgo-manifest.xml"}, {0x100B, "shareparam.json"}, {0x100C, "shareoverlayimage.png"},
    {0x100D, "save_data.png"}, {0x100E, "shareprivacyguardimage.png"}, {0x1200, "icon0.png"},
    {0x1220, "pic0.png"}, {0x1240, "snd0.at9"}, {0x1260, "changeinfo/changeinfo.xml"}, {0x1280, "icon0.dds"},
    {0x12A0, "pic0.dds"}, {0x12C0, "pic1.dds"}, {0x1480, "trophy2/trophy00.ucp"}, {0x14A0, "uds/uds00.ucp"},
    {0x2000, "param.json"}, {0x2010, "playgo-hash-table.dat"}, {0x2011, "playgo-ficm.dat"},
    {0x2020, "uds/npbind.dat"}, {0x2021, "trophy2/npbind.dat"}, {0x2040, "pic2.png"}, {0x2060, "pic2.dds"},
};

// The name of an entry the names table leaves out, into buf ("" if it is none MaxPack knows).
static const char *known_name(uint32_t id, char *buf, size_t n) {
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (NAMES[i].id == id) return NAMES[i].name;
    buf[0] = 0;
    if (id >= 0x1201 && id < 0x1201 + 31) snprintf(buf, n, "icon0_%02u.png", id - 0x1201);
    else if (id >= 0x1281 && id < 0x1281 + 31) snprintf(buf, n, "icon0_%02u.dds", id - 0x1281);
    else if (id >= 0x1241 && id < 0x1241 + 31) snprintf(buf, n, "pic1_%02u.png", id - 0x1241);
    else if (id >= 0x12C1 && id < 0x12C1 + 31) snprintf(buf, n, "pic1_%02u.dds", id - 0x12C1);
    else if (id >= 0x1261 && id < 0x1261 + 31) snprintf(buf, n, "changeinfo/changeinfo_%02u.xml", id - 0x1261);
    else if (id >= 0x1600 && id < 0x1600 + 10) snprintf(buf, n, "keymap_rp/%02u.png", id - 0x1600 + 1);
    else if (id >= 0x1610 && id < 0x1610 + 16 * 31 && (id - 0x1610) % 16 < 10)
        snprintf(buf, n, "keymap_rp/%02u/%02u.png", (id - 0x1610) / 16, (id - 0x1610) % 16 + 1);
    else if (id >= 0x1400 && id < 0x1400 + 100) snprintf(buf, n, "trophy/trophy%02u.trp", id - 0x1400);
    return buf;
}

const char *mp_fpkg_entry_path(const mp_fpkg *p, size_t i) {
    const mp_cnt_entry *en = &p->entries[i];
    // digests, keys, metas, names and the image digests are the package's, not the game's files
    if (en->id < 0x400 || en->id == 0x40A) return NULL;
    return en->name && en->name[0] ? en->name : NULL;
}

// ---- keys ------------------------------------------------------------------------------------

// SHA3( SHA3(BE32(index)) || SHA3(content ID NUL-padded to 48) || passcode ).
static void compute_keys(const mp_fpkg *p, uint32_t index, uint8_t out[32]) {
    uint8_t be[4] = {(uint8_t)(index >> 24), (uint8_t)(index >> 16), (uint8_t)(index >> 8), (uint8_t)index};
    uint8_t cid[48] = {0}, data[96];
    memcpy(cid, p->content_id, 36);
    mp_sha3_256(be, 4, data);
    mp_sha3_256(cid, sizeof cid, data + 32);
    memcpy(data + 64, p->passcode, MP_FPKG_PASSCODE_LEN);
    mp_sha3_256(data, sizeof data, out);
}

// ---- the header and the CNT ------------------------------------------------------------------

static int read_cnt(mp_fpkg *p, mp_err *e) {
    const char *name = p->file->name;
    uint64_t fsize = p->file->size;
    uint8_t h[CNT_HEADER];
    if (!in_range(p->cnt_off, CNT_HEADER, fsize)) {
        mp_err_set(e, "%s: damaged package (its CNT header is outside it)", name);
        return -1;
    }
    if (read_exact(p->file, h, sizeof h, p->cnt_off, e) != 0) return -1;
    if (memcmp(h, "\x7F" "CNT", 4) != 0) {
        mp_err_set(e, "%s: damaged package (no CNT header where its FIH header says)", name);
        return -1;
    }
    uint32_t count = be32(h + 0x10), table = be32(h + 0x18);
    memcpy(p->content_id, h + 0x40, 36);
    p->content_id[36] = 0;
    p->content_type = be32(h + 0x74);
    for (int i = 0; i < 36; i++)
        if (h[0x40 + i] < 0x21 || h[0x40 + i] > 0x7E) {
            mp_err_set(e, "%s: damaged package (it has no valid content ID)", name);
            return -1;
        }
    if (p->content_type == 0x21 && !(p->accept & MP_FPKG_DLC)) {
        mp_err_set(e, "%s: additional content (DLC), not a game: give it to pack with its game, with --dlc", name);
        return -1;
    }
    if (p->content_type == 0x20 && !(p->accept & MP_FPKG_GAME)) {
        mp_err_set(e, "%s: a game's package, not additional content (DLC)", name);
        return -1;
    }
    if (p->content_type != 0x20 && p->content_type != 0x21) {
        if (p->accept & MP_FPKG_GAME)
            mp_err_set(e, "%s: not a game's package (content type 0x%" PRIX32 "): MaxPack packs a full game", name,
                       p->content_type);
        else
            mp_err_set(e, "%s: not a package of additional content (DLC) (content type 0x%" PRIX32 ")", name,
                       p->content_type);
        return -1;
    }
    uint64_t avail = fsize - p->cnt_off;
    if (count == 0 || count > MAX_ENTRIES || !in_range(table, (uint64_t)count * 32, avail)) {
        mp_err_set(e, "%s: damaged package (its CNT entry table is out of range)", name);
        return -1;
    }
    uint8_t *t = malloc((size_t)count * 32);
    p->entries = calloc(count, sizeof *p->entries);
    if (!t || !p->entries) {
        free(t);
        return oom(e);
    }
    p->nentries = count;
    int rc = read_exact(p->file, t, (size_t)count * 32, p->cnt_off + table, e);
    long names = -1;
    for (uint32_t i = 0; rc == 0 && i < count; i++) {
        const uint8_t *r = t + 32 * i;
        mp_cnt_entry *en = &p->entries[i];
        en->id = be32(r);
        en->name_off = be32(r + 4);
        en->flags1 = be32(r + 8);
        en->flags2 = be32(r + 12);
        en->data_off = be32(r + 16);
        en->data_size = be32(r + 20);
        memcpy(en->row, r, 32);
        if (en->id == 0x200 && names < 0) names = (long)i;
    }
    free(t);
    if (rc != 0) return -1;
    if (names >= 0) {   // the names table (0x200): NUL-terminated names, by offset
        const mp_cnt_entry *nt = &p->entries[names];
        if (nt->data_size && nt->data_size <= MAX_NAMES && in_range(nt->data_off, nt->data_size, avail)) {
            uint8_t *blob = malloc(nt->data_size);
            if (!blob) return oom(e);
            if (read_exact(p->file, blob, nt->data_size, p->cnt_off + nt->data_off, e) != 0) {
                free(blob);
                return -1;
            }
            for (size_t i = 0; i < p->nentries; i++) {
                mp_cnt_entry *en = &p->entries[i];
                if (en->name_off == 0 || en->name_off >= nt->data_size) continue;
                const uint8_t *s = blob + en->name_off;
                const uint8_t *z = memchr(s, 0, nt->data_size - en->name_off);
                if (!z || z == s) continue;
                en->name = malloc((size_t)(z - s) + 1);
                if (!en->name) {
                    free(blob);
                    return oom(e);
                }
                memcpy(en->name, s, (size_t)(z - s));
                en->name[z - s] = 0;
            }
            free(blob);
        }
    }
    for (size_t i = 0; i < p->nentries; i++) {   // the known names of the entries stored without one
        mp_cnt_entry *en = &p->entries[i];
        if (en->name || en->id < 0x400) continue;
        char buf[64];
        const char *k = known_name(en->id, buf, sizeof buf);
        if (k[0] && !(en->name = mp_strdup(k))) return oom(e);
    }
    return 0;
}

int mp_fpkg_entry(mp_fpkg *p, size_t i, uint8_t **out, size_t *len, mp_err *e) {
    const mp_cnt_entry *en = &p->entries[i];
    int enc = (en->flags1 & 0x80000000u) != 0;
    uint64_t stored = enc ? ((uint64_t)en->data_size + 15) / 16 * 16 : en->data_size;
    *out = NULL;
    *len = 0;
    if (stored > MAX_ENTRY || !in_range(p->cnt_off + en->data_off, stored, p->file->size) ||
        p->cnt_off + en->data_off < p->cnt_off) {
        mp_err_set(e, "%s: damaged package (the entry of sce_sys/%s is outside it)", p->file->name,
                   en->name ? en->name : "?");
        return -1;
    }
    uint8_t *b = malloc(stored ? (size_t)stored : 1);
    if (!b) return oom(e);
    if (read_exact(p->file, b, (size_t)stored, p->cnt_off + en->data_off, e) != 0) {
        free(b);
        return -1;
    }
    if (enc) {
        // key and IV from SHA3(the table row || the package key of its index): key = [16:32], IV = [0:16]
        uint8_t seed_in[64], seed[32];
        memcpy(seed_in, en->row, 32);
        compute_keys(p, (en->flags2 >> 12) & 0xF, seed_in + 32);
        mp_sha3_256(seed_in, sizeof seed_in, seed);
        mp_aes128 k;
        mp_aes128_init(&k, seed + 16);
        mp_cbc_decrypt(&k, seed, b, (size_t)stored);
    }
    *out = b;
    *len = en->data_size;
    return 0;
}

// ---- the outer PFS ---------------------------------------------------------------------------

typedef struct {
    uint16_t mode;
    uint64_t size;
    uint32_t blocks;
    int32_t direct[12], indirect[5];
    int64_t mtime;
} s32_inode;

// One 64 KiB block of the outer PFS, decrypted (signed: the metadata's sector numbering).
static int outer_block(mp_fpkg *p, uint32_t index, int is_signed, uint8_t *out, mp_err *e) {
    if (index >= p->nblocks) {
        mp_err_set(e, "%s: damaged package (a block of its PFS is out of range)", p->file->name);
        return -1;
    }
    if (read_exact(p->file, out, MP_FPKG_BLOCK, p->pfs_off + (uint64_t)index * MP_FPKG_BLOCK, e) != 0) return -1;
    if (index == p->sb || p->plain) return 0;
    mp_xts_decrypt(&p->data_key, &p->tweak_key, is_signed ? (SIGNED_SECTOR | index) : index, 0, out, MP_FPKG_BLOCK);
    return 0;
}

static void parse_inode(const uint8_t *d, s32_inode *n) {
    n->mode = le16(d);
    n->size = le64(d + 8);
    n->blocks = le32(d + 0x60);
    n->mtime = (int64_t)le64(d + 0x20);
    for (int k = 0; k < 12; k++) n->direct[k] = (int32_t)le32(d + S32_DIRECT + k * SIG_STRIDE + 32);
    for (int k = 0; k < 5; k++) n->indirect[k] = (int32_t)le32(d + S32_INDIRECT + k * SIG_STRIDE + 32);
}

typedef struct {
    mp_fpkg *p;
    uint32_t *out;
    uint32_t n, want;
    uint8_t *node;   // one block per level
} walker;

static int bad_tree(walker *w, mp_err *e) {
    mp_err_set(e, "%s: damaged package (a file of its PFS points outside it)", w->p->file->name);
    return -1;
}

// The data blocks under an indirect block of the given depth (1: its entries are data blocks).
static int walk_tree(walker *w, int64_t root, int depth, mp_err *e) {
    if (w->n >= w->want) return 0;
    if (root <= 0 || root >= w->p->nblocks) return bad_tree(w, e);
    uint8_t *node = w->node + (size_t)(depth - 1) * MP_FPKG_BLOCK;
    if (outer_block(w->p, (uint32_t)root, 1, node, e) != 0) return -1;
    for (size_t i = 0; i < TREE_ENTRIES && w->n < w->want; i++) {
        int32_t child = (int32_t)le32(node + i * SIG_STRIDE + 32);
        if (depth == 1) {
            if (child < 0 || (uint32_t)child >= w->p->nblocks) return bad_tree(w, e);
            w->out[w->n++] = (uint32_t)child;
        } else {
            if (child <= 0) return bad_tree(w, e);
            if (walk_tree(w, child, depth - 1, e) != 0) return -1;   // the level below has its own block
        }
    }
    return 0;
}

// The blocks of a file (12 direct, then the five indirect trees: ib[k] is the root of a tree of depth k+1).
static int resolve(mp_fpkg *p, const s32_inode *n, uint32_t **out, mp_err *e) {
    *out = NULL;
    if (n->blocks == 0 || n->blocks > p->nblocks) {
        mp_err_set(e, "%s: damaged package (a file of its PFS has more blocks than the package)", p->file->name);
        return -1;
    }
    walker w = {p, malloc((size_t)n->blocks * sizeof(uint32_t)), 0, n->blocks, malloc(5 * (size_t)MP_FPKG_BLOCK)};
    if (!w.out || !w.node) {
        free(w.out);
        free(w.node);
        return oom(e);
    }
    int rc = 0;
    for (int k = 0; k < 12 && w.n < w.want; k++) {
        if (n->direct[k] < 0 || (uint32_t)n->direct[k] >= p->nblocks) {
            rc = bad_tree(&w, e);
            break;
        }
        w.out[w.n++] = (uint32_t)n->direct[k];
    }
    for (int level = 0; rc == 0 && level < 5 && w.n < w.want; level++)
        if (n->indirect[level] > 0) rc = walk_tree(&w, n->indirect[level], level + 1, e);
    free(w.node);
    if (rc == 0 && w.n < w.want) {
        mp_err_set(e, "%s: damaged package (the blocks of a file of its PFS are incomplete)", p->file->name);
        rc = -1;
    }
    if (rc != 0) {
        free(w.out);
        return -1;
    }
    *out = w.out;
    return 0;
}

// Reads a whole file of the outer PFS (at most max bytes).
static int read_outer_file(mp_fpkg *p, const s32_inode *n, int is_signed, size_t max, uint8_t **out, size_t *len,
                           mp_err *e) {
    *out = NULL;
    uint32_t *blocks = NULL;
    if (resolve(p, n, &blocks, e) != 0) return -1;
    uint64_t size = n->size < (uint64_t)n->blocks * MP_FPKG_BLOCK ? n->size : (uint64_t)n->blocks * MP_FPKG_BLOCK;
    if (size > max) {
        free(blocks);
        mp_err_set(e, "%s: damaged package (a file of its PFS is too large)", p->file->name);
        return -1;
    }
    size_t nb = (size_t)((size + MP_FPKG_BLOCK - 1) / MP_FPKG_BLOCK);
    uint8_t *b = malloc(nb ? nb * MP_FPKG_BLOCK : 1);
    if (!b) {
        free(blocks);
        return oom(e);
    }
    for (size_t i = 0; i < nb; i++)
        if (outer_block(p, blocks[i], is_signed, b + i * MP_FPKG_BLOCK, e) != 0) {
            free(b);
            free(blocks);
            return -1;
        }
    free(blocks);
    *out = b;
    *len = (size_t)size;
    return 0;
}

static int wrong_key(mp_fpkg *p, mp_err *e) {
    if (!p->icv_ok)
        mp_err_set(e, "%s: damaged package (its PFS does not decrypt to a valid root folder, and its superblock's "
                      "checksum does not match)",
                   p->file->name);
    else if (p->default_passcode)
        mp_err_set(e, "%s: this package was made with another passcode; give it with --passcode", p->file->name);
    else
        mp_err_set(e, "%s: this package was made with another passcode than the one given; give the right one with "
                      "--passcode",
                   p->file->name);
    return -1;
}

static int open_outer(mp_fpkg *p, uint64_t sb_field, mp_err *e) {
    const char *name = p->file->name;
    uint8_t *blk = malloc(MP_FPKG_BLOCK);
    if (!blk) return oom(e);
    int found = 0;
    // the superblock: where the FIH says, else the first block with the PFS signature
    if (sb_field >= MP_FPKG_BLOCK && (sb_field - MP_FPKG_BLOCK) % MP_FPKG_BLOCK == 0 &&
        (sb_field - MP_FPKG_BLOCK) / MP_FPKG_BLOCK < p->nblocks) {
        p->sb = (uint32_t)((sb_field - MP_FPKG_BLOCK) / MP_FPKG_BLOCK);
        if (read_exact(p->file, blk, 16, p->pfs_off + (uint64_t)p->sb * MP_FPKG_BLOCK, e) != 0) goto fail;
        found = le64(blk + 8) == PFS_MAGIC;
    }
    for (uint32_t i = 0; !found && i < p->nblocks; i++) {
        if (read_exact(p->file, blk, 16, p->pfs_off + (uint64_t)i * MP_FPKG_BLOCK, e) != 0) goto fail;
        if (le64(blk + 8) == PFS_MAGIC) {
            p->sb = i;
            found = 1;
        }
    }
    if (!found) {
        mp_err_set(e, "%s: damaged package (its PFS has no superblock)", name);
        goto fail;
    }
    if (read_exact(p->file, blk, MP_FPKG_BLOCK, p->pfs_off + (uint64_t)p->sb * MP_FPKG_BLOCK, e) != 0) goto fail;
    const uint8_t *seed = blk + 0x370;
    p->plain = !memcmp(seed, "PPRPLAIN-NOAUTH!", 16);
    {
        uint8_t region[0x5A0], icv[32];
        memcpy(region, blk, sizeof region);
        memset(region + 0x380, 0, 32);
        mp_sha3_256(region, sizeof region, icv);
        p->icv_ok = p->plain || !memcmp(icv, blk + 0x380, 32);
    }
    if (!p->plain) {
        // EKPFS from the passcode; inner = HMAC(EKPFS, seed); keys = HMAC(inner, LE32(1) || seed):
        // the tweak key is the first half, the data key the second.
        uint8_t ekpfs[32], inner[32], msg[20] = {1, 0, 0, 0}, keys[32];
        compute_keys(p, 1, ekpfs);
        mp_hmac_sha256(ekpfs, 32, seed, 16, inner);
        memcpy(msg + 4, seed, 16);
        mp_hmac_sha256(inner, 32, msg, sizeof msg, keys);
        mp_aes128_init(&p->tweak_key, keys);
        mp_aes128_init(&p->data_key, keys + 16);
    }
    // the inode table: after the superblock and the indirect blocks of its own signature
    uint32_t indirect = 0;
    for (int i = 0; i < 5; i++)
        if ((int64_t)le64(blk + 0x50 + 0x244 + i * 40 + 8) > 0) indirect++;
    uint64_t icount = le64(blk + 0x30), iblocks = le64(blk + 0x40), first = (uint64_t)p->sb + 1 + indirect;
    if (icount < 3 || iblocks == 0 || iblocks > MAX_INODE_BLOCKS || icount > iblocks * (MP_FPKG_BLOCK / S32_INODE) ||
        first + iblocks > p->nblocks) {
        mp_err_set(e, "%s: damaged package (the inode table of its PFS is out of range)", name);
        goto fail;
    }
    uint8_t *tab = malloc((size_t)iblocks * MP_FPKG_BLOCK);
    if (!tab) {
        oom(e);
        goto fail;
    }
    for (uint64_t b = 0; b < iblocks; b++)
        if (outer_block(p, (uint32_t)(first + b), 1, tab + b * MP_FPKG_BLOCK, e) != 0) {
            free(tab);
            goto fail;
        }
    // the inodes are packed across the blocks (94 of 0x2C8 bytes in each, the rest unused)
    s32_inode uroot, pfs_image, naps_file;
    memset(&pfs_image, 0, sizeof pfs_image);
    memset(&naps_file, 0, sizeof naps_file);
    size_t per = MP_FPKG_BLOCK / S32_INODE;
#define INODE_AT(i) (tab + ((i) / per) * MP_FPKG_BLOCK + ((i) % per) * S32_INODE)
    parse_inode(INODE_AT(2), &uroot);
    if ((uroot.mode & 0xF000) != 0x4000 || uroot.size == 0 || uroot.size > MAX_UROOT || uroot.blocks == 0 ||
        uroot.blocks > p->nblocks) {
        free(tab);
        wrong_key(p, e);
        goto fail;
    }
    p->mtime = uroot.mtime;
    uint8_t *dir = NULL;
    size_t dlen = 0;
    if (read_outer_file(p, &uroot, 1, MAX_UROOT, &dir, &dlen, e) != 0) {
        free(tab);
        goto fail;
    }
    int have_image = 0, have_naps = 0;
    for (size_t pos = 0; pos + 16 <= dlen;) {
        uint32_t ino = le32(dir + pos), nlen = le32(dir + pos + 8), esize = le32(dir + pos + 12);
        if (esize < 16 || esize > dlen - pos || nlen > esize - 16) break;
        const uint8_t *nm = dir + pos + 16;
        if (ino < icount && ino < iblocks * per) {
            if (nlen == 13 && !memcmp(nm, "pfs_image.dat", 13)) {
                parse_inode(INODE_AT(ino), &pfs_image);
                have_image = 1;
            } else if (nlen == 19 && !memcmp(nm, "naps_pkg_layout.dat", 19)) {
                parse_inode(INODE_AT(ino), &naps_file);
                have_naps = 1;
            }
        }
        pos += esize;
    }
#undef INODE_AT
    free(dir);
    free(tab);
    (void)pfs_image;
    if (!have_image || !have_naps) {
        if (!have_image && !have_naps && !p->icv_ok)
            wrong_key(p, e);
        else
            mp_err_set(e, "%s: damaged package (its PFS has no %s)", name,
                       have_image ? "naps_pkg_layout.dat" : "pfs_image.dat");
        goto fail;
    }
    if ((naps_file.mode & 0xF000) != 0x8000 ||
        read_outer_file(p, &naps_file, 1, MAX_NAPS, &p->naps, &p->naps_len, e) != 0) {
        if ((naps_file.mode & 0xF000) != 0x8000)
            mp_err_set(e, "%s: damaged package (naps_pkg_layout.dat is not a file)", name);
        goto fail;
    }
    // pfs_image.dat's data: the blocks before naps_pkg_layout.dat (data first: from block 0 up to the
    // superblock, less the map's own block), as the reference reads it
    if (p->sb < 2) {
        mp_err_set(e, "%s: damaged package (its PFS has no room for pfs_image.dat)", name);
        goto fail;
    }
    free(blk);
    return 0;
fail:
    free(blk);
    return -1;
}

typedef struct {
    mp_fpkg *p;
} xts_view;

// pfs_image.dat: bytes [off, off + len) of the blocks [0, sb - 1), decrypted. Reads outside 16-byte
// boundaries go through a buffer of whole AES blocks.
static int view_read(mp_image *m, void *buf, size_t len, uint64_t off, mp_err *e) {
    mp_fpkg *p = ((xts_view *)m->impl)->p;
    uint64_t a = off & ~15ull, b = (off + len + 15) & ~15ull;
    uint8_t *work = buf;
    int temp = a != off || b != off + len;
    if (temp && !(work = malloc((size_t)(b - a)))) return oom(e);
    if (read_exact(p->file, work, (size_t)(b - a), p->pfs_off + a, e) != 0) {
        if (temp) free(work);
        return -1;
    }
    if (!p->plain)
        for (uint64_t at = a; at < b;) {
            uint64_t unit = at / MP_FPKG_BLOCK, end = (unit + 1) * MP_FPKG_BLOCK;
            if (end > b) end = b;
            mp_xts_decrypt(&p->data_key, &p->tweak_key, unit, at - unit * MP_FPKG_BLOCK, work + (at - a),
                           (size_t)(end - at));
            at = end;
        }
    if (temp) {
        memcpy(buf, work + (off - a), len);
        free(work);
    }
    return 0;
}

static void view_close(mp_image *m) { free(m->impl); }

int mp_fpkg_inner_image(mp_fpkg *p, mp_image **out, mp_err *e) {
    *out = NULL;
    mp_image *m = calloc(1, sizeof *m);
    xts_view *v = calloc(1, sizeof *v);
    char *nm = malloc(strlen(p->file->name) + 32);
    if (!m || !v || !nm) {
        free(m);
        free(v);
        free(nm);
        return oom(e);
    }
    sprintf(nm, "%s (pfs_image.dat)", p->file->name);
    v->p = p;
    m->name = nm;
    m->size = (uint64_t)(p->sb - 1) * MP_FPKG_BLOCK;
    m->impl = v;
    m->read = view_read;
    m->close = view_close;
    *out = m;
    return 0;
}

void mp_fpkg_close(mp_fpkg *p) {
    if (!p) return;
    for (size_t i = 0; i < p->nentries; i++) free(p->entries[i].name);
    free(p->entries);
    free(p->naps);
    mp_image_close(p->file);
    free(p);
}

int mp_fpkg_passcode_ok(const char *passcode) {
    size_t n = 0;
    for (; passcode[n]; n++)
        if ((unsigned char)passcode[n] < 0x21 || (unsigned char)passcode[n] > 0x7E) return 0;
    return n == MP_FPKG_PASSCODE_LEN;
}

int mp_fpkg_open(mp_image *img, const char *passcode, int accept, mp_fpkg **out, mp_err *e) {
    *out = NULL;
    const char *name = img->name;
    uint8_t h[0x100] = {0};
    size_t got = 0;
    if (mp_image_pread(img, h, sizeof h, 0, &got, e) != 0) {
        mp_image_close(img);
        return -1;
    }
    if (got >= 4 && !memcmp(h, "\x7F" "CNT", 4)) {
        mp_err_set(e, "%s: a PS4 package (or the metadata of one): MaxPack is for PS5 games", name);
        mp_image_close(img);
        return -1;
    }
    if (got >= 4 && !memcmp(h, "\x7F" "LIH", 4)) {
        mp_err_set(e, "%s: a patch (update) package: it only has what the update changes; MaxPack packs a full "
                      "game (install the patch on the console, or pack the game with it applied)",
                   name);
        mp_image_close(img);
        return -1;
    }
    if (got < sizeof h || memcmp(h, "\x7F" "FIH", 4) || img->size < MP_FPKG_BLOCK) {
        mp_err_set(e, "%s: not a PS5 package, or a damaged one (it has no FIH header)", name);
        mp_image_close(img);
        return -1;
    }
    if (h[5] == 0x80) {
        mp_err_set(e, "%s: a retail package: retail packages need keys this tool does not have (MaxPack reads the "
                      "debug packages, FPKG, that homebrew tools make)",
                   name);
        mp_image_close(img);
        return -1;
    }
    if (h[5] != 0) {
        mp_err_set(e, "%s: a package of a kind MaxPack does not know (FIH byte 0x%02X)", name, h[5]);
        mp_image_close(img);
        return -1;
    }
    if (passcode && passcode[0] && !mp_fpkg_passcode_ok(passcode)) {
        mp_err_set(e, "the passcode must be 32 characters (letters, digits or signs, no spaces)");
        mp_image_close(img);
        return -1;
    }
    mp_fpkg *p = calloc(1, sizeof *p);
    if (!p) {
        mp_image_close(img);
        return oom(e);
    }
    p->file = img;
    p->accept = accept;
    snprintf(p->passcode, sizeof p->passcode, "%s", passcode && passcode[0] ? passcode : "00000000000000000000000000000000");
    p->default_passcode = !strcmp(p->passcode, "00000000000000000000000000000000");
    p->pfs_off = le64(h + 0x10);
    p->pfs_size = le64(h + 0x18);
    p->inner_meta_base = le64(h + 0x50);
    p->cnt_off = le64(h + 0x58);
    if (p->pfs_off < MP_FPKG_BLOCK || p->pfs_size < 2 * MP_FPKG_BLOCK || !in_range(p->pfs_off, p->pfs_size, img->size) ||
        p->pfs_size / MP_FPKG_BLOCK > UINT32_MAX) {
        mp_err_set(e, "%s: damaged package (its PFS is out of range)", name);
        mp_fpkg_close(p);
        return -1;
    }
    p->nblocks = (uint32_t)(p->pfs_size / MP_FPKG_BLOCK);
    if (read_cnt(p, e) != 0 || open_outer(p, le64(h + 0x20), e) != 0) {
        mp_fpkg_close(p);
        return -1;
    }
    *out = p;
    return 0;
}
