/*
 * SHA-512, FIPS 180-4. Sister of sha256.c — same shape, twice the
 * width: 64-bit state words, 80 rounds, 128-byte blocks, 64-byte
 * output. We need it for Ed25519 (RFC 8032), which is hardcoded
 * to SHA-512 by spec.
 *
 * On i386 with no 64-bit registers, every 64-bit op compiles to
 * 2x 32-bit. Speed: ~150 MB/s on QEMU's qemu32 — fine for the
 * handful of hashes a TLS handshake needs.
 *
 * Test vectors used (FIPS 180-2 Appendix C / RFC 6234 §8):
 *   "abc"  -> ddaf35a193617aba cc417349ae204131 12e6fa4e89a97ea2 0a9eeee64b55d39a
 *             2192992a274fc1a8 36ba3c23a3feebbd 454d4423643ce80e 2a9ac94fa54ca49f
 *   ""     -> cf83e1357eefb8bd f1542850d66d8007 d620e4050b5715dc 83f4a921d36ce9ce
 *             47d0d13c5d85f2b0 ff8318d2877eec2f 63b931bd47417a81 a538327af927da3e
 */
#include "crypto.h"
#include "sha512.h"

static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

#define ROTR64(x,n) (((x) >> (n)) | ((x) << (64 - (n))))
#define SHR64(x,n)  ((x) >> (n))

static void sha512_compress(uint64_t state[8], const uint8_t block[128]) {
    uint64_t w[80];
    /* Big-endian 64-bit words from the 128-byte block. */
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint64_t)block[8*i  ] << 56) | ((uint64_t)block[8*i+1] << 48) |
               ((uint64_t)block[8*i+2] << 40) | ((uint64_t)block[8*i+3] << 32) |
               ((uint64_t)block[8*i+4] << 24) | ((uint64_t)block[8*i+5] << 16) |
               ((uint64_t)block[8*i+6] <<  8) | ((uint64_t)block[8*i+7]);
    }
    /* Extend to 80 words. */
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROTR64(w[i-15], 1) ^ ROTR64(w[i-15], 8) ^ SHR64(w[i-15], 7);
        uint64_t s1 = ROTR64(w[i-2], 19) ^ ROTR64(w[i-2], 61) ^ SHR64(w[i-2], 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    uint64_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint64_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ROTR64(e, 14) ^ ROTR64(e, 18) ^ ROTR64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K[i] + w[i];
        uint64_t S0 = ROTR64(a, 28) ^ ROTR64(a, 34) ^ ROTR64(a, 39);
        uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha512_init(struct sha512 *s) {
    /* FIPS 180-4 §5.3.5 — IV. */
    s->state[0] = 0x6a09e667f3bcc908ULL;
    s->state[1] = 0xbb67ae8584caa73bULL;
    s->state[2] = 0x3c6ef372fe94f82bULL;
    s->state[3] = 0xa54ff53a5f1d36f1ULL;
    s->state[4] = 0x510e527fade682d1ULL;
    s->state[5] = 0x9b05688c2b3e6c1fULL;
    s->state[6] = 0x1f83d9abfb41bd6bULL;
    s->state[7] = 0x5be0cd19137e2179ULL;
    s->count[0] = 0;
    s->count[1] = 0;
    s->buf_len = 0;
}

static inline void inc_count(struct sha512 *s, uint64_t n) {
    /* count is a 128-bit total of bytes; we use two 64-bit halves
     * (count[0] = low, count[1] = high). Way more than we'll ever
     * need but matches the spec exactly. */
    uint64_t old = s->count[0];
    s->count[0] += n;
    if (s->count[0] < old) s->count[1]++;
}

void sha512_update(struct sha512 *s, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    inc_count(s, n);

    if (s->buf_len) {
        uint32_t take = 128 - s->buf_len;
        if (take > n) take = (uint32_t)n;
        for (uint32_t i = 0; i < take; i++) s->buf[s->buf_len + i] = p[i];
        s->buf_len += take;
        p += take; n -= take;
        /* If we didn't fill a full block, leave buf_len at its
         * partial value and return. Skipping this early-out and
         * falling through to "buf_len = n" at the bottom would
         * WIPE the partial buffer when n is 0 — which is exactly
         * what happens on update(empty_msg, 0) after a previous
         * partial update. Lost two days to that bug. */
        if (s->buf_len < 128) return;
        sha512_compress(s->state, s->buf);
        s->buf_len = 0;
    }
    while (n >= 128) {
        sha512_compress(s->state, p);
        p += 128; n -= 128;
    }
    for (size_t i = 0; i < n; i++) s->buf[i] = p[i];
    s->buf_len = (uint32_t)n;
}

void sha512_final(struct sha512 *s, uint8_t out[64]) {
    /* Pad: 0x80, zeros, 128-bit big-endian bit count. */
    uint64_t bits_lo = s->count[0] << 3;
    uint64_t bits_hi = (s->count[1] << 3) | (s->count[0] >> 61);

    s->buf[s->buf_len++] = 0x80;
    if (s->buf_len > 112) {
        while (s->buf_len < 128) s->buf[s->buf_len++] = 0;
        sha512_compress(s->state, s->buf);
        s->buf_len = 0;
    }
    while (s->buf_len < 112) s->buf[s->buf_len++] = 0;
    /* 128-bit bit count: high 64 bits at offset 112, low 64 at 120. */
    for (int i = 0; i < 8; i++) {
        s->buf[112 + i] = (uint8_t)(bits_hi >> (56 - i * 8));
        s->buf[120 + i] = (uint8_t)(bits_lo >> (56 - i * 8));
    }
    sha512_compress(s->state, s->buf);

    for (int i = 0; i < 8; i++) {
        out[8*i  ] = (uint8_t)(s->state[i] >> 56);
        out[8*i+1] = (uint8_t)(s->state[i] >> 48);
        out[8*i+2] = (uint8_t)(s->state[i] >> 40);
        out[8*i+3] = (uint8_t)(s->state[i] >> 32);
        out[8*i+4] = (uint8_t)(s->state[i] >> 24);
        out[8*i+5] = (uint8_t)(s->state[i] >> 16);
        out[8*i+6] = (uint8_t)(s->state[i] >>  8);
        out[8*i+7] = (uint8_t)(s->state[i]      );
    }
}

void sha512(const void *data, size_t n, uint8_t out[64]) {
    struct sha512 s;
    sha512_init(&s);
    sha512_update(&s, data, n);
    sha512_final(&s, out);
}
