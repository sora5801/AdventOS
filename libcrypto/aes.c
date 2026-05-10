/*
 * AES-128 (FIPS 197) — encryption only. Uses the classic 4-table
 * construction: precomputed Te0..Te3 lookup tables fold SubBytes
 * + ShiftRows + MixColumns into 4 KiB of read-only data plus four
 * lookups + four XORs per column per round.
 *
 * No hardware AES-NI path; QEMU emulates fine. ~25 MB/s on qemu32.
 *
 * Test vector (FIPS 197 Appendix C.1):
 *   key = 00010203 04050607 08090a0b 0c0d0e0f
 *   in  = 00112233 44556677 8899aabb ccddeeff
 *   out = 69c4e0d8 6a7b0430 d8cdb780 70b4c55a
 */
#include "crypto.h"

/* S-box from FIPS 197 §5.1.1. */
static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

/* Round constants for key expansion (10 rounds for AES-128). */
static const uint8_t Rcon[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

static inline uint32_t rotr8(uint32_t x) { return (x >> 8) | (x << 24); }

/* xtime: multiply by x (= 0x02) in GF(2^8) with reduction polynomial
 * x^8 + x^4 + x^3 + x + 1 (= 0x11b). */
static inline uint8_t xtime(uint8_t b) {
    return (uint8_t)((b << 1) ^ ((b & 0x80) ? 0x1b : 0));
}

/* Build Te0 lazily (saves 4 KiB ROM). Te0[i] = MixColumns([s, s, s*2, s*3])
 * with s = sbox[i], packed big-endian. Te1..Te3 are byte-rotations. */
static uint32_t Te0[256];
static int      Te_built;

static void build_Te(void) {
    if (Te_built) return;
    for (int i = 0; i < 256; i++) {
        uint8_t s = sbox[i];
        uint8_t s2 = xtime(s);
        uint8_t s3 = (uint8_t)(s2 ^ s);
        /* Big-endian packing: t = [s2, s, s, s3]
         * MixColumns first column processes [2*a, a, a, 3*a]^T
         * but the encrypt loop does the column re-assembly itself. */
        Te0[i] = ((uint32_t)s2 << 24) |
                 ((uint32_t)s  << 16) |
                 ((uint32_t)s  <<  8) |
                 ((uint32_t)s3      );
    }
    Te_built = 1;
}

#define Te1(x) rotr8(Te0[x])
#define Te2(x) rotr8(Te1(x))
#define Te3(x) rotr8(Te2(x))

/* Read big-endian 32-bit. */
static inline uint32_t LD32BE(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) | ((uint32_t)p[3]);
}
static inline void ST32BE(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)(v);
}

void aes128_set_key(struct aes128 *ctx, const uint8_t key[16]) {
    build_Te();
    uint32_t *rk = ctx->rk;
    rk[0] = LD32BE(key);
    rk[1] = LD32BE(key + 4);
    rk[2] = LD32BE(key + 8);
    rk[3] = LD32BE(key + 12);
    for (int i = 1; i <= 10; i++) {
        uint32_t t = rk[i*4 - 1];
        /* RotWord + SubWord + Rcon */
        t = ((uint32_t)sbox[(t >> 16) & 0xFF] << 24) |
            ((uint32_t)sbox[(t >>  8) & 0xFF] << 16) |
            ((uint32_t)sbox[(t      ) & 0xFF] <<  8) |
            ((uint32_t)sbox[(t >> 24) & 0xFF]      );
        t ^= ((uint32_t)Rcon[i]) << 24;
        rk[i*4    ] = rk[(i-1)*4    ] ^ t;
        rk[i*4 + 1] = rk[(i-1)*4 + 1] ^ rk[i*4    ];
        rk[i*4 + 2] = rk[(i-1)*4 + 2] ^ rk[i*4 + 1];
        rk[i*4 + 3] = rk[(i-1)*4 + 3] ^ rk[i*4 + 2];
    }
}

void aes128_encrypt(const struct aes128 *ctx, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t *rk = ctx->rk;

    uint32_t s0 = LD32BE(in     ) ^ rk[0];
    uint32_t s1 = LD32BE(in +  4) ^ rk[1];
    uint32_t s2 = LD32BE(in +  8) ^ rk[2];
    uint32_t s3 = LD32BE(in + 12) ^ rk[3];

    /* 9 full rounds. */
    uint32_t t0, t1, t2, t3;
    for (int r = 1; r < 10; r++) {
        t0 = Te0[(s0 >> 24)       ] ^ Te1((s1 >> 16) & 0xFF) ^
             Te2((s2 >>  8) & 0xFF) ^ Te3((s3       ) & 0xFF) ^ rk[r*4    ];
        t1 = Te0[(s1 >> 24)       ] ^ Te1((s2 >> 16) & 0xFF) ^
             Te2((s3 >>  8) & 0xFF) ^ Te3((s0       ) & 0xFF) ^ rk[r*4 + 1];
        t2 = Te0[(s2 >> 24)       ] ^ Te1((s3 >> 16) & 0xFF) ^
             Te2((s0 >>  8) & 0xFF) ^ Te3((s1       ) & 0xFF) ^ rk[r*4 + 2];
        t3 = Te0[(s3 >> 24)       ] ^ Te1((s0 >> 16) & 0xFF) ^
             Te2((s1 >>  8) & 0xFF) ^ Te3((s2       ) & 0xFF) ^ rk[r*4 + 3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }

    /* Final round: SubBytes + ShiftRows + AddRoundKey (no MixColumns). */
    t0 = ((uint32_t)sbox[(s0 >> 24)       ] << 24) |
         ((uint32_t)sbox[(s1 >> 16) & 0xFF] << 16) |
         ((uint32_t)sbox[(s2 >>  8) & 0xFF] <<  8) |
         ((uint32_t)sbox[(s3       ) & 0xFF]      );
    t1 = ((uint32_t)sbox[(s1 >> 24)       ] << 24) |
         ((uint32_t)sbox[(s2 >> 16) & 0xFF] << 16) |
         ((uint32_t)sbox[(s3 >>  8) & 0xFF] <<  8) |
         ((uint32_t)sbox[(s0       ) & 0xFF]      );
    t2 = ((uint32_t)sbox[(s2 >> 24)       ] << 24) |
         ((uint32_t)sbox[(s3 >> 16) & 0xFF] << 16) |
         ((uint32_t)sbox[(s0 >>  8) & 0xFF] <<  8) |
         ((uint32_t)sbox[(s1       ) & 0xFF]      );
    t3 = ((uint32_t)sbox[(s3 >> 24)       ] << 24) |
         ((uint32_t)sbox[(s0 >> 16) & 0xFF] << 16) |
         ((uint32_t)sbox[(s1 >>  8) & 0xFF] <<  8) |
         ((uint32_t)sbox[(s2       ) & 0xFF]      );

    ST32BE(out     , t0 ^ rk[40]);
    ST32BE(out +  4, t1 ^ rk[41]);
    ST32BE(out +  8, t2 ^ rk[42]);
    ST32BE(out + 12, t3 ^ rk[43]);
}
