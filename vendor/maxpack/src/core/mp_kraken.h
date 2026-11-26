// SPDX-License-Identifier: GPL-3.0-or-later
// The Oodle Kraken ("newLZ") blocks inside a PS5 package's inner image: header-stripped, up to
// 256 KiB decoded, as one or two sub-chunks of 128 KiB. A port of pearlxcore/ProsperoPkgTool's
// Compression/KrakenDecoder.cs (MIT; see THIRD_PARTY_NOTICES), checked against fthdgn-gh/Alembic's
// src/core/kraken.cpp (GPL-3.0-or-later, a port of the same). Entropy-coded arrays may be raw or
// Huffman (one or two splits of three streams); the TANS, RLE and recursive arrays are reported as
// unsupported (the package writers do not use them). Every index taken from the input is checked:
// a damaged block fails cleanly.
#ifndef MP_KRAKEN_H
#define MP_KRAKEN_H

#include <stddef.h>
#include <stdint.h>

#define MP_KRAKEN_CHUNK 0x20000u   // one sub-chunk
#define MP_KRAKEN_BLOCK 0x40000u   // a block: at most two sub-chunks

// The flags of a block (low nibble: the first sub-chunk; high nibble: the second).
#define MP_KRAKEN_SUB0 0x01       // delta literals (else raw literals)
#define MP_KRAKEN_NEWLZ0 0x02     // newLZ (else a bare entropy-coded array)
#define MP_KRAKEN_SUB1 0x10
#define MP_KRAKEN_NEWLZ1 0x20
#define MP_KRAKEN_RESTART1 0x40   // the second sub-chunk starts its own window (with a seed)

enum { MP_KRAKEN_OK = 0, MP_KRAKEN_MALFORMED = -1, MP_KRAKEN_UNSUPPORTED = -2, MP_KRAKEN_NOMEM = -3 };

// What one decoding needs (about 1.2 MiB); one per thread.
typedef struct mp_kraken mp_kraken;
mp_kraken *mp_kraken_new(void);
void mp_kraken_free(mp_kraken *k);   // k may be NULL

// Decodes one block of dst_len bytes (1 to 256 KiB) into dst. first_chunk: the compressed size of
// the first sub-chunk, used only when the block has two (dst_len > 128 KiB). MP_KRAKEN_OK or one of
// the errors; on an error dst holds anything.
int mp_kraken_decode(mp_kraken *k, const uint8_t *src, size_t src_len, int flags, size_t first_chunk, uint8_t *dst,
                     size_t dst_len);

#endif
