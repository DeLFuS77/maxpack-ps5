// SPDX-License-Identifier: GPL-3.0-or-later
// AES-128 with 32-bit tables (the S-box, its inverse and one round table for each direction, the
// other three columns by rotation), and with the CPU's AES instructions on 64-bit x86 PCs that have
// them; XTS, CBC, SHA3-256 (Keccak-f[1600]) and HMAC-SHA256. Written for MaxPack from FIPS-197,
// IEEE 1619, SP 800-38A, FIPS-202 and RFC 2104.
#include "mp_crypto.h"

#include <string.h>

#include "mp_hash.h"

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};
static const uint8_t ISBOX[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};
static const uint32_t TE0[256] = {
    0xa56363c6, 0x847c7cf8, 0x997777ee, 0x8d7b7bf6, 0x0df2f2ff, 0xbd6b6bd6, 0xb16f6fde, 0x54c5c591,
    0x50303060, 0x03010102, 0xa96767ce, 0x7d2b2b56, 0x19fefee7, 0x62d7d7b5, 0xe6abab4d, 0x9a7676ec,
    0x45caca8f, 0x9d82821f, 0x40c9c989, 0x877d7dfa, 0x15fafaef, 0xeb5959b2, 0xc947478e, 0x0bf0f0fb,
    0xecadad41, 0x67d4d4b3, 0xfda2a25f, 0xeaafaf45, 0xbf9c9c23, 0xf7a4a453, 0x967272e4, 0x5bc0c09b,
    0xc2b7b775, 0x1cfdfde1, 0xae93933d, 0x6a26264c, 0x5a36366c, 0x413f3f7e, 0x02f7f7f5, 0x4fcccc83,
    0x5c343468, 0xf4a5a551, 0x34e5e5d1, 0x08f1f1f9, 0x937171e2, 0x73d8d8ab, 0x53313162, 0x3f15152a,
    0x0c040408, 0x52c7c795, 0x65232346, 0x5ec3c39d, 0x28181830, 0xa1969637, 0x0f05050a, 0xb59a9a2f,
    0x0907070e, 0x36121224, 0x9b80801b, 0x3de2e2df, 0x26ebebcd, 0x6927274e, 0xcdb2b27f, 0x9f7575ea,
    0x1b090912, 0x9e83831d, 0x742c2c58, 0x2e1a1a34, 0x2d1b1b36, 0xb26e6edc, 0xee5a5ab4, 0xfba0a05b,
    0xf65252a4, 0x4d3b3b76, 0x61d6d6b7, 0xceb3b37d, 0x7b292952, 0x3ee3e3dd, 0x712f2f5e, 0x97848413,
    0xf55353a6, 0x68d1d1b9, 0x00000000, 0x2cededc1, 0x60202040, 0x1ffcfce3, 0xc8b1b179, 0xed5b5bb6,
    0xbe6a6ad4, 0x46cbcb8d, 0xd9bebe67, 0x4b393972, 0xde4a4a94, 0xd44c4c98, 0xe85858b0, 0x4acfcf85,
    0x6bd0d0bb, 0x2aefefc5, 0xe5aaaa4f, 0x16fbfbed, 0xc5434386, 0xd74d4d9a, 0x55333366, 0x94858511,
    0xcf45458a, 0x10f9f9e9, 0x06020204, 0x817f7ffe, 0xf05050a0, 0x443c3c78, 0xba9f9f25, 0xe3a8a84b,
    0xf35151a2, 0xfea3a35d, 0xc0404080, 0x8a8f8f05, 0xad92923f, 0xbc9d9d21, 0x48383870, 0x04f5f5f1,
    0xdfbcbc63, 0xc1b6b677, 0x75dadaaf, 0x63212142, 0x30101020, 0x1affffe5, 0x0ef3f3fd, 0x6dd2d2bf,
    0x4ccdcd81, 0x140c0c18, 0x35131326, 0x2fececc3, 0xe15f5fbe, 0xa2979735, 0xcc444488, 0x3917172e,
    0x57c4c493, 0xf2a7a755, 0x827e7efc, 0x473d3d7a, 0xac6464c8, 0xe75d5dba, 0x2b191932, 0x957373e6,
    0xa06060c0, 0x98818119, 0xd14f4f9e, 0x7fdcdca3, 0x66222244, 0x7e2a2a54, 0xab90903b, 0x8388880b,
    0xca46468c, 0x29eeeec7, 0xd3b8b86b, 0x3c141428, 0x79dedea7, 0xe25e5ebc, 0x1d0b0b16, 0x76dbdbad,
    0x3be0e0db, 0x56323264, 0x4e3a3a74, 0x1e0a0a14, 0xdb494992, 0x0a06060c, 0x6c242448, 0xe45c5cb8,
    0x5dc2c29f, 0x6ed3d3bd, 0xefacac43, 0xa66262c4, 0xa8919139, 0xa4959531, 0x37e4e4d3, 0x8b7979f2,
    0x32e7e7d5, 0x43c8c88b, 0x5937376e, 0xb76d6dda, 0x8c8d8d01, 0x64d5d5b1, 0xd24e4e9c, 0xe0a9a949,
    0xb46c6cd8, 0xfa5656ac, 0x07f4f4f3, 0x25eaeacf, 0xaf6565ca, 0x8e7a7af4, 0xe9aeae47, 0x18080810,
    0xd5baba6f, 0x887878f0, 0x6f25254a, 0x722e2e5c, 0x241c1c38, 0xf1a6a657, 0xc7b4b473, 0x51c6c697,
    0x23e8e8cb, 0x7cdddda1, 0x9c7474e8, 0x211f1f3e, 0xdd4b4b96, 0xdcbdbd61, 0x868b8b0d, 0x858a8a0f,
    0x907070e0, 0x423e3e7c, 0xc4b5b571, 0xaa6666cc, 0xd8484890, 0x05030306, 0x01f6f6f7, 0x120e0e1c,
    0xa36161c2, 0x5f35356a, 0xf95757ae, 0xd0b9b969, 0x91868617, 0x58c1c199, 0x271d1d3a, 0xb99e9e27,
    0x38e1e1d9, 0x13f8f8eb, 0xb398982b, 0x33111122, 0xbb6969d2, 0x70d9d9a9, 0x898e8e07, 0xa7949433,
    0xb69b9b2d, 0x221e1e3c, 0x92878715, 0x20e9e9c9, 0x49cece87, 0xff5555aa, 0x78282850, 0x7adfdfa5,
    0x8f8c8c03, 0xf8a1a159, 0x80898909, 0x170d0d1a, 0xdabfbf65, 0x31e6e6d7, 0xc6424284, 0xb86868d0,
    0xc3414182, 0xb0999929, 0x772d2d5a, 0x110f0f1e, 0xcbb0b07b, 0xfc5454a8, 0xd6bbbb6d, 0x3a16162c,
};
static const uint32_t TD0[256] = {
    0x50a7f451, 0x5365417e, 0xc3a4171a, 0x965e273a, 0xcb6bab3b, 0xf1459d1f, 0xab58faac, 0x9303e34b,
    0x55fa3020, 0xf66d76ad, 0x9176cc88, 0x254c02f5, 0xfcd7e54f, 0xd7cb2ac5, 0x80443526, 0x8fa362b5,
    0x495ab1de, 0x671bba25, 0x980eea45, 0xe1c0fe5d, 0x02752fc3, 0x12f04c81, 0xa397468d, 0xc6f9d36b,
    0xe75f8f03, 0x959c9215, 0xeb7a6dbf, 0xda595295, 0x2d83bed4, 0xd3217458, 0x2969e049, 0x44c8c98e,
    0x6a89c275, 0x78798ef4, 0x6b3e5899, 0xdd71b927, 0xb64fe1be, 0x17ad88f0, 0x66ac20c9, 0xb43ace7d,
    0x184adf63, 0x82311ae5, 0x60335197, 0x457f5362, 0xe07764b1, 0x84ae6bbb, 0x1ca081fe, 0x942b08f9,
    0x58684870, 0x19fd458f, 0x876cde94, 0xb7f87b52, 0x23d373ab, 0xe2024b72, 0x578f1fe3, 0x2aab5566,
    0x0728ebb2, 0x03c2b52f, 0x9a7bc586, 0xa50837d3, 0xf2872830, 0xb2a5bf23, 0xba6a0302, 0x5c8216ed,
    0x2b1ccf8a, 0x92b479a7, 0xf0f207f3, 0xa1e2694e, 0xcdf4da65, 0xd5be0506, 0x1f6234d1, 0x8afea6c4,
    0x9d532e34, 0xa055f3a2, 0x32e18a05, 0x75ebf6a4, 0x39ec830b, 0xaaef6040, 0x069f715e, 0x51106ebd,
    0xf98a213e, 0x3d06dd96, 0xae053edd, 0x46bde64d, 0xb58d5491, 0x055dc471, 0x6fd40604, 0xff155060,
    0x24fb9819, 0x97e9bdd6, 0xcc434089, 0x779ed967, 0xbd42e8b0, 0x888b8907, 0x385b19e7, 0xdbeec879,
    0x470a7ca1, 0xe90f427c, 0xc91e84f8, 0x00000000, 0x83868009, 0x48ed2b32, 0xac70111e, 0x4e725a6c,
    0xfbff0efd, 0x5638850f, 0x1ed5ae3d, 0x27392d36, 0x64d90f0a, 0x21a65c68, 0xd1545b9b, 0x3a2e3624,
    0xb1670a0c, 0x0fe75793, 0xd296eeb4, 0x9e919b1b, 0x4fc5c080, 0xa220dc61, 0x694b775a, 0x161a121c,
    0x0aba93e2, 0xe52aa0c0, 0x43e0223c, 0x1d171b12, 0x0b0d090e, 0xadc78bf2, 0xb9a8b62d, 0xc8a91e14,
    0x8519f157, 0x4c0775af, 0xbbdd99ee, 0xfd607fa3, 0x9f2601f7, 0xbcf5725c, 0xc53b6644, 0x347efb5b,
    0x7629438b, 0xdcc623cb, 0x68fcedb6, 0x63f1e4b8, 0xcadc31d7, 0x10856342, 0x40229713, 0x2011c684,
    0x7d244a85, 0xf83dbbd2, 0x1132f9ae, 0x6da129c7, 0x4b2f9e1d, 0xf330b2dc, 0xec52860d, 0xd0e3c177,
    0x6c16b32b, 0x99b970a9, 0xfa489411, 0x2264e947, 0xc48cfca8, 0x1a3ff0a0, 0xd82c7d56, 0xef903322,
    0xc74e4987, 0xc1d138d9, 0xfea2ca8c, 0x360bd498, 0xcf81f5a6, 0x28de7aa5, 0x268eb7da, 0xa4bfad3f,
    0xe49d3a2c, 0x0d927850, 0x9bcc5f6a, 0x62467e54, 0xc2138df6, 0xe8b8d890, 0x5ef7392e, 0xf5afc382,
    0xbe805d9f, 0x7c93d069, 0xa92dd56f, 0xb31225cf, 0x3b99acc8, 0xa77d1810, 0x6e639ce8, 0x7bbb3bdb,
    0x097826cd, 0xf418596e, 0x01b79aec, 0xa89a4f83, 0x656e95e6, 0x7ee6ffaa, 0x08cfbc21, 0xe6e815ef,
    0xd99be7ba, 0xce366f4a, 0xd4099fea, 0xd67cb029, 0xafb2a431, 0x31233f2a, 0x3094a5c6, 0xc066a235,
    0x37bc4e74, 0xa6ca82fc, 0xb0d090e0, 0x15d8a733, 0x4a9804f1, 0xf7daec41, 0x0e50cd7f, 0x2ff69117,
    0x8dd64d76, 0x4db0ef43, 0x544daacc, 0xdf0496e4, 0xe3b5d19e, 0x1b886a4c, 0xb81f2cc1, 0x7f516546,
    0x04ea5e9d, 0x5d358c01, 0x737487fa, 0x2e410bfb, 0x5a1d67b3, 0x52d2db92, 0x335610e9, 0x1347d66d,
    0x8c61d79a, 0x7a0ca137, 0x8e14f859, 0x893c13eb, 0xee27a9ce, 0x35c961b7, 0xede51ce1, 0x3cb1477a,
    0x59dfd29c, 0x3f73f255, 0x79ce1418, 0xbf37c773, 0xeacdf753, 0x5baafd5f, 0x146f3ddf, 0x86db4478,
    0x81f3afca, 0x3ec468b9, 0x2c342438, 0x5f40a3c2, 0x72c31d16, 0x0c25e2bc, 0x8b493c28, 0x41950dff,
    0x7101a839, 0xdeb30c08, 0x9ce4b4d8, 0x90c15664, 0x6184cb7b, 0x70b632d5, 0x745c6c48, 0x4257b8d0,
};

static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#define B(x, k) (((x) >> (8 * (k))) & 0xFF)
#define TE(x, k) ((k) ? rotl(TE0[B(x, k)], 8 * (k)) : TE0[B(x, 0)])
#define TD(x, k) ((k) ? rotl(TD0[B(x, k)], 8 * (k)) : TD0[B(x, 0)])

// InvMixColumns of one word: TD0 of the S-box gives the inverse matrix's column for each byte.
static uint32_t inv_mix(uint32_t w) {
    return TD0[SBOX[B(w, 0)]] ^ rotl(TD0[SBOX[B(w, 1)]], 8) ^ rotl(TD0[SBOX[B(w, 2)]], 16) ^
           rotl(TD0[SBOX[B(w, 3)]], 24);
}

void mp_aes128_init(mp_aes128 *a, const uint8_t key[16]) {
    uint32_t *w = a->ek;
    uint8_t rcon = 1;
    for (int i = 0; i < 4; i++) w[i] = ld32(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        uint32_t t = w[i - 1];
        if (i % 4 == 0) {
            t = (t >> 8) | (t << 24);   // RotWord on little-endian words
            t = (uint32_t)SBOX[B(t, 0)] | (uint32_t)SBOX[B(t, 1)] << 8 | (uint32_t)SBOX[B(t, 2)] << 16 |
                (uint32_t)SBOX[B(t, 3)] << 24;
            t ^= rcon;
            rcon = (uint8_t)(rcon << 1 ^ (rcon & 0x80 ? 0x1B : 0));
        }
        w[i] = w[i - 4] ^ t;
    }
    // The equivalent inverse cipher: the rounds in reverse, the middle ones through InvMixColumns.
    for (int r = 0; r <= 10; r++)
        for (int j = 0; j < 4; j++) {
            uint32_t k = a->ek[4 * (10 - r) + j];
            a->dk[4 * r + j] = r == 0 || r == 10 ? k : inv_mix(k);
        }
}

static void enc_block(const mp_aes128 *a, const uint8_t *in, uint8_t *out) {
    const uint32_t *k = a->ek;
    uint32_t c0 = ld32(in) ^ k[0], c1 = ld32(in + 4) ^ k[1], c2 = ld32(in + 8) ^ k[2], c3 = ld32(in + 12) ^ k[3];
    for (int r = 1; r < 10; r++) {
        k += 4;
        uint32_t t0 = TE(c0, 0) ^ TE(c1, 1) ^ TE(c2, 2) ^ TE(c3, 3) ^ k[0];
        uint32_t t1 = TE(c1, 0) ^ TE(c2, 1) ^ TE(c3, 2) ^ TE(c0, 3) ^ k[1];
        uint32_t t2 = TE(c2, 0) ^ TE(c3, 1) ^ TE(c0, 2) ^ TE(c1, 3) ^ k[2];
        uint32_t t3 = TE(c3, 0) ^ TE(c0, 1) ^ TE(c1, 2) ^ TE(c2, 3) ^ k[3];
        c0 = t0;
        c1 = t1;
        c2 = t2;
        c3 = t3;
    }
    k += 4;
#define SB(x, k) ((uint32_t)SBOX[B(x, k)] << (8 * (k)))
    st32(out, (SB(c0, 0) | SB(c1, 1) | SB(c2, 2) | SB(c3, 3)) ^ k[0]);
    st32(out + 4, (SB(c1, 0) | SB(c2, 1) | SB(c3, 2) | SB(c0, 3)) ^ k[1]);
    st32(out + 8, (SB(c2, 0) | SB(c3, 1) | SB(c0, 2) | SB(c1, 3)) ^ k[2]);
    st32(out + 12, (SB(c3, 0) | SB(c0, 1) | SB(c1, 2) | SB(c2, 3)) ^ k[3]);
#undef SB
}

static void dec_block(const mp_aes128 *a, const uint8_t *in, uint8_t *out) {
    const uint32_t *k = a->dk;
    uint32_t c0 = ld32(in) ^ k[0], c1 = ld32(in + 4) ^ k[1], c2 = ld32(in + 8) ^ k[2], c3 = ld32(in + 12) ^ k[3];
    for (int r = 1; r < 10; r++) {
        k += 4;
        uint32_t t0 = TD(c0, 0) ^ TD(c3, 1) ^ TD(c2, 2) ^ TD(c1, 3) ^ k[0];
        uint32_t t1 = TD(c1, 0) ^ TD(c0, 1) ^ TD(c3, 2) ^ TD(c2, 3) ^ k[1];
        uint32_t t2 = TD(c2, 0) ^ TD(c1, 1) ^ TD(c0, 2) ^ TD(c3, 3) ^ k[2];
        uint32_t t3 = TD(c3, 0) ^ TD(c2, 1) ^ TD(c1, 2) ^ TD(c0, 3) ^ k[3];
        c0 = t0;
        c1 = t1;
        c2 = t2;
        c3 = t3;
    }
    k += 4;
#define IB(x, k) ((uint32_t)ISBOX[B(x, k)] << (8 * (k)))
    st32(out, (IB(c0, 0) | IB(c3, 1) | IB(c2, 2) | IB(c1, 3)) ^ k[0]);
    st32(out + 4, (IB(c1, 0) | IB(c0, 1) | IB(c3, 2) | IB(c2, 3)) ^ k[1]);
    st32(out + 8, (IB(c2, 0) | IB(c1, 1) | IB(c0, 2) | IB(c3, 3)) ^ k[2]);
    st32(out + 12, (IB(c3, 0) | IB(c2, 1) | IB(c1, 2) | IB(c0, 3)) ^ k[3]);
#undef IB
}

// ---- the CPU's AES instructions (AES-NI) ----------------------------------------------------
// On the same 64-bit x86 PCs as mp_hash's SHA instructions; the console build (and any other CPU)
// takes the portable code above.
#if (defined(_M_X64) || defined(__x86_64__)) && (defined(_WIN32) || defined(__linux__) || defined(__APPLE__))
#define MP_AES_NI 1
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#define MP_AES_TARGET
#else
#include <cpuid.h>
#define MP_AES_TARGET __attribute__((target("aes,sse2")))
#endif

static int cpu_has_aes(void) {
#ifdef _MSC_VER
    int r[4];
    __cpuid(r, 1);
    return ((unsigned)r[2] >> 25) & 1;
#else
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    return (c >> 25) & 1;   // leaf 1, ECX bit 25
#endif
}

// The round keys are the bytes of ek and dk as they are in memory (little-endian words).
MP_AES_TARGET static void crypt_ni(const uint32_t *ks, int dec, const uint8_t *in, uint8_t *out, size_t n) {
    __m128i k[11];
    for (int r = 0; r <= 10; r++) k[r] = _mm_loadu_si128((const __m128i *)(ks + 4 * r));
    for (; n >= 8; n -= 8, in += 128, out += 128) {
        __m128i x[8];
        for (int i = 0; i < 8; i++) x[i] = _mm_xor_si128(_mm_loadu_si128((const __m128i *)(in + 16 * i)), k[0]);
        if (dec) {
            for (int r = 1; r < 10; r++)
                for (int i = 0; i < 8; i++) x[i] = _mm_aesdec_si128(x[i], k[r]);
            for (int i = 0; i < 8; i++) x[i] = _mm_aesdeclast_si128(x[i], k[10]);
        } else {
            for (int r = 1; r < 10; r++)
                for (int i = 0; i < 8; i++) x[i] = _mm_aesenc_si128(x[i], k[r]);
            for (int i = 0; i < 8; i++) x[i] = _mm_aesenclast_si128(x[i], k[10]);
        }
        for (int i = 0; i < 8; i++) _mm_storeu_si128((__m128i *)(out + 16 * i), x[i]);
    }
    for (; n; n--, in += 16, out += 16) {
        __m128i x = _mm_xor_si128(_mm_loadu_si128((const __m128i *)in), k[0]);
        if (dec) {
            for (int r = 1; r < 10; r++) x = _mm_aesdec_si128(x, k[r]);
            x = _mm_aesdeclast_si128(x, k[10]);
        } else {
            for (int r = 1; r < 10; r++) x = _mm_aesenc_si128(x, k[r]);
            x = _mm_aesenclast_si128(x, k[10]);
        }
        _mm_storeu_si128((__m128i *)out, x);
    }
}

// The XTS tweak times alpha (x in GF(2^128), the 128-bit number little-endian): every 32-bit lane
// shifted left, with the top bit of the lane below carried in (and the top bit of the whole number
// folded back as 0x87).
MP_AES_TARGET static __m128i mul_alpha(__m128i t) {
    __m128i carry = _mm_srai_epi32(_mm_shuffle_epi32(t, 0x93), 31);
    carry = _mm_and_si128(carry, _mm_set_epi32(1, 1, 1, 0x87));
    return _mm_xor_si128(_mm_slli_epi32(t, 1), carry);
}

MP_AES_TARGET static void xts_ni(const mp_aes128 *a, int dec, const uint8_t t0[16], uint8_t *buf, size_t n) {
    __m128i k[11];
    const uint32_t *ks = dec ? a->dk : a->ek;
    for (int r = 0; r <= 10; r++) k[r] = _mm_loadu_si128((const __m128i *)(ks + 4 * r));
    __m128i t = _mm_loadu_si128((const __m128i *)t0);
    for (; n >= 128; n -= 128, buf += 128) {
        __m128i tw[8], x[8];
        for (int i = 0; i < 8; i++) {
            tw[i] = t;
            t = mul_alpha(t);
            x[i] = _mm_xor_si128(_mm_xor_si128(_mm_loadu_si128((const __m128i *)(buf + 16 * i)), tw[i]), k[0]);
        }
        if (dec) {
            for (int r = 1; r < 10; r++)
                for (int i = 0; i < 8; i++) x[i] = _mm_aesdec_si128(x[i], k[r]);
            for (int i = 0; i < 8; i++) x[i] = _mm_aesdeclast_si128(x[i], k[10]);
        } else {
            for (int r = 1; r < 10; r++)
                for (int i = 0; i < 8; i++) x[i] = _mm_aesenc_si128(x[i], k[r]);
            for (int i = 0; i < 8; i++) x[i] = _mm_aesenclast_si128(x[i], k[10]);
        }
        for (int i = 0; i < 8; i++) _mm_storeu_si128((__m128i *)(buf + 16 * i), _mm_xor_si128(x[i], tw[i]));
    }
    for (; n >= 16; n -= 16, buf += 16) {
        __m128i x = _mm_xor_si128(_mm_xor_si128(_mm_loadu_si128((const __m128i *)buf), t), k[0]);
        if (dec) {
            for (int r = 1; r < 10; r++) x = _mm_aesdec_si128(x, k[r]);
            x = _mm_aesdeclast_si128(x, k[10]);
        } else {
            for (int r = 1; r < 10; r++) x = _mm_aesenc_si128(x, k[r]);
            x = _mm_aesenclast_si128(x, k[10]);
        }
        _mm_storeu_si128((__m128i *)buf, _mm_xor_si128(x, t));
        t = mul_alpha(t);
    }
}
#endif

// Which code runs: -1 not decided yet, 0 portable, 1 the CPU's instructions (as in mp_hash.c).
#ifdef _MSC_VER
static volatile long g_accel = -1;
#define ACCEL_LOAD() g_accel
#define ACCEL_STORE(v) (g_accel = (v))
#else
static int g_accel = -1;
#define ACCEL_LOAD() __atomic_load_n(&g_accel, __ATOMIC_RELAXED)
#define ACCEL_STORE(v) __atomic_store_n(&g_accel, (v), __ATOMIC_RELAXED)
#endif

int mp_aes_accel(void) {
    int a = (int)ACCEL_LOAD();
    if (a < 0) {
#ifdef MP_AES_NI
        a = cpu_has_aes();
#else
        a = 0;
#endif
        ACCEL_STORE(a);
    }
    return a;
}

void mp_aes_use_accel(int on) { ACCEL_STORE(on ? -1 : 0); }

void mp_aes128_encrypt(const mp_aes128 *a, const uint8_t *in, uint8_t *out, size_t nblocks) {
#ifdef MP_AES_NI
    if (mp_aes_accel()) {
        crypt_ni(a->ek, 0, in, out, nblocks);
        return;
    }
#endif
    for (; nblocks; nblocks--, in += 16, out += 16) enc_block(a, in, out);
}

void mp_aes128_decrypt(const mp_aes128 *a, const uint8_t *in, uint8_t *out, size_t nblocks) {
#ifdef MP_AES_NI
    if (mp_aes_accel()) {
        crypt_ni(a->dk, 1, in, out, nblocks);
        return;
    }
#endif
    for (; nblocks; nblocks--, in += 16, out += 16) dec_block(a, in, out);
}

// ---- XTS ------------------------------------------------------------------------------------

static uint64_t ld64(const uint8_t *p) { return (uint64_t)ld32(p) | (uint64_t)ld32(p + 4) << 32; }
static void st64(uint8_t *p, uint64_t v) {
    st32(p, (uint32_t)v);
    st32(p + 4, (uint32_t)(v >> 32));
}

static void alpha(uint8_t t[16]) {
    uint64_t lo = ld64(t), hi = ld64(t + 8);
    uint64_t carry = hi >> 63;
    hi = hi << 1 | lo >> 63;
    lo = lo << 1 ^ (carry ? 0x87 : 0);
    st64(t, lo);
    st64(t + 8, hi);
}

static void xts(const mp_aes128 *data, const mp_aes128 *tweak, uint64_t sector, uint64_t at, uint8_t *buf, size_t n,
                int dec) {
    uint8_t t[16] = {0};
    st64(t, sector);
    mp_aes128_encrypt(tweak, t, t, 1);
    for (uint64_t q = at / 16; q; q--) alpha(t);
#ifdef MP_AES_NI
    if (mp_aes_accel()) {
        xts_ni(data, dec, t, buf, n);
        return;
    }
#endif
    for (; n >= 16; n -= 16, buf += 16) {
        for (int i = 0; i < 16; i++) buf[i] ^= t[i];
        if (dec)
            dec_block(data, buf, buf);
        else
            enc_block(data, buf, buf);
        for (int i = 0; i < 16; i++) buf[i] ^= t[i];
        alpha(t);
    }
}

void mp_xts_decrypt(const mp_aes128 *data, const mp_aes128 *tweak, uint64_t sector, uint64_t at, uint8_t *buf,
                    size_t n) {
    xts(data, tweak, sector, at, buf, n, 1);
}

void mp_xts_encrypt(const mp_aes128 *data, const mp_aes128 *tweak, uint64_t sector, uint64_t at, uint8_t *buf,
                    size_t n) {
    xts(data, tweak, sector, at, buf, n, 0);
}

// ---- CBC ------------------------------------------------------------------------------------

void mp_cbc_decrypt(const mp_aes128 *a, const uint8_t iv[16], uint8_t *buf, size_t n) {
    uint8_t prev[16], cur[16];
    memcpy(prev, iv, 16);
    for (; n >= 16; n -= 16, buf += 16) {
        memcpy(cur, buf, 16);
        mp_aes128_decrypt(a, buf, buf, 1);
        for (int i = 0; i < 16; i++) buf[i] ^= prev[i];
        memcpy(prev, cur, 16);
    }
}

void mp_cbc_encrypt(const mp_aes128 *a, const uint8_t iv[16], uint8_t *buf, size_t n) {
    const uint8_t *prev = iv;
    for (; n >= 16; n -= 16, buf += 16) {
        for (int i = 0; i < 16; i++) buf[i] ^= prev[i];
        mp_aes128_encrypt(a, buf, buf, 1);
        prev = buf;
    }
}

// ---- SHA3-256 -------------------------------------------------------------------------------

#define SHA3_RATE 136

static const uint64_t RC[24] = {
    0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808aull, 0x8000000080008000ull,
    0x000000000000808bull, 0x0000000080000001ull, 0x8000000080008081ull, 0x8000000000008009ull,
    0x000000000000008aull, 0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000aull,
    0x000000008000808bull, 0x800000000000008bull, 0x8000000000008089ull, 0x8000000000008003ull,
    0x8000000000008002ull, 0x8000000000000080ull, 0x000000000000800aull, 0x800000008000000aull,
    0x8000000080008081ull, 0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull,
};
static const int RHO[24] = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
static const int PI[24] = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

static uint64_t rotl64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

static void keccak_f(uint64_t s[25]) {
    for (int r = 0; r < 24; r++) {
        uint64_t c[5];
        for (int x = 0; x < 5; x++) c[x] = s[x] ^ s[x + 5] ^ s[x + 10] ^ s[x + 15] ^ s[x + 20];
        for (int x = 0; x < 5; x++) {
            uint64_t d = c[(x + 4) % 5] ^ rotl64(c[(x + 1) % 5], 1);
            for (int y = 0; y < 25; y += 5) s[y + x] ^= d;
        }
        uint64_t t = s[1];
        for (int i = 0; i < 24; i++) {
            uint64_t u = s[PI[i]];
            s[PI[i]] = rotl64(t, RHO[i]);
            t = u;
        }
        for (int y = 0; y < 25; y += 5) {
            uint64_t b[5];
            for (int x = 0; x < 5; x++) b[x] = s[y + x];
            for (int x = 0; x < 5; x++) s[y + x] = b[x] ^ (~b[(x + 1) % 5] & b[(x + 2) % 5]);
        }
        s[0] ^= RC[r];
    }
}

void mp_sha3_init(mp_sha3 *s) {
    memset(s->st, 0, sizeof s->st);
    s->n = 0;
}

// XORs v into byte i of the state (the lanes are little-endian).
static void absorb_byte(mp_sha3 *s, size_t i, uint8_t v) { s->st[i / 8] ^= (uint64_t)v << (8 * (i % 8)); }

void mp_sha3_update(mp_sha3 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        if (s->n == 0 && n >= SHA3_RATE) {
            for (int i = 0; i < SHA3_RATE / 8; i++) s->st[i] ^= ld64(p + 8 * i);
            keccak_f(s->st);
            p += SHA3_RATE;
            n -= SHA3_RATE;
            continue;
        }
        absorb_byte(s, s->n++, *p++);
        n--;
        if (s->n == SHA3_RATE) {
            keccak_f(s->st);
            s->n = 0;
        }
    }
}

void mp_sha3_final(mp_sha3 *s, uint8_t out[32]) {
    absorb_byte(s, s->n, 0x06);
    absorb_byte(s, SHA3_RATE - 1, 0x80);
    keccak_f(s->st);
    for (int i = 0; i < 4; i++) st64(out + 8 * i, s->st[i]);
}

void mp_sha3_256(const void *p, size_t n, uint8_t out[32]) {
    mp_sha3 s;
    mp_sha3_init(&s);
    mp_sha3_update(&s, p, n);
    mp_sha3_final(&s, out);
}

// ---- HMAC-SHA256 ----------------------------------------------------------------------------

void mp_hmac_sha256(const void *key, size_t klen, const void *msg, size_t n, uint8_t out[32]) {
    uint8_t k[64] = {0}, pad[64], inner[32];
    mp_sha256 h;
    if (klen > 64) {
        mp_sha256_init(&h);
        mp_sha256_update(&h, key, klen);
        mp_sha256_final(&h, k);
    } else if (klen) {
        memcpy(k, key, klen);
    }
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    mp_sha256_init(&h);
    mp_sha256_update(&h, pad, 64);
    mp_sha256_update(&h, msg, n);
    mp_sha256_final(&h, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    mp_sha256_init(&h);
    mp_sha256_update(&h, pad, 64);
    mp_sha256_update(&h, inner, 32);
    mp_sha256_final(&h, out);
}
