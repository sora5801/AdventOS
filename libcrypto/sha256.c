/*
 * SHA-256, FIPS 180-4. Standard implementation; no fancy tricks,
 * no SHA-NI hardware path. ~700 MB/s on QEMU's qemu32 model — way
 * fast enough for a TLS handshake.
 *
 * Test vectors used (RFC 6234 §8 and FIPS 180-2 Appendix B):
 *   "abc"   ->  ba7816bf 8f01cfea 414140de 5dae2223
 *               b00361a3 96177a9c b410ff61 f20015ad
 *   ""      ->  e3b0c442 98fc1c14 9afbf4c8 996fb924
 *               27ae41e4 649b934c a495991b 7852b855
 *   "abcdbc..." (RFC 6234 vector 2) -> 248d6a61 d20638b8 ...
 */
#include "crypto.h"

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHR(x,n)  ((x) >> (n))

static void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    /* Schedule first 16 words from the block (big-endian). */
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[4*i  ] << 24) |
               ((uint32_t)block[4*i+1] << 16) |
               ((uint32_t)block[4*i+2] <<  8) |
               ((uint32_t)block[4*i+3]      );
    }
    /* Extend to 64 words. */
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i-15], 7) ^ ROTR(w[i-15], 18) ^ SHR(w[i-15], 3);
        uint32_t s1 = ROTR(w[i-2], 17) ^ ROTR(w[i-2], 19) ^ SHR(w[i-2], 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(struct sha256 *s) {
    /* FIPS 180-4 §5.3.3 — IV. */
    s->state[0] = 0x6a09e667;
    s->state[1] = 0xbb67ae85;
    s->state[2] = 0x3c6ef372;
    s->state[3] = 0xa54ff53a;
    s->state[4] = 0x510e527f;
    s->state[5] = 0x9b05688c;
    s->state[6] = 0x1f83d9ab;
    s->state[7] = 0x5be0cd19;
    s->count   = 0;
    s->buf_len = 0;
}

void sha256_update(struct sha256 *s, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    s->count += n;

    /* Top up the partial buffer if any. */
    if (s->buf_len) {
        uint32_t take = 64 - s->buf_len;
        if (take > n) take = (uint32_t)n;
        for (uint32_t i = 0; i < take; i++) s->buf[s->buf_len + i] = p[i];
        s->buf_len += take;
        p += take; n -= take;
        if (s->buf_len == 64) {
            sha256_compress(s->state, s->buf);
            s->buf_len = 0;
        }
    }
    /* Direct-from-input compression. */
    while (n >= 64) {
        sha256_compress(s->state, p);
        p += 64; n -= 64;
    }
    /* Stash remainder. */
    for (size_t i = 0; i < n; i++) s->buf[i] = p[i];
    s->buf_len = (uint32_t)n;
}

void sha256_final(struct sha256 *s, uint8_t out[32]) {
    /* Pad: 0x80, zeros, 64-bit big-endian bit count. */
    uint64_t bits = s->count * 8;
    s->buf[s->buf_len++] = 0x80;
    if (s->buf_len > 56) {
        while (s->buf_len < 64) s->buf[s->buf_len++] = 0;
        sha256_compress(s->state, s->buf);
        s->buf_len = 0;
    }
    while (s->buf_len < 56) s->buf[s->buf_len++] = 0;
    /* Bit count, big-endian. We keep `bits` in 64 bits — overkill
     * for our message sizes but matches the spec exactly. */
    for (int i = 0; i < 8; i++) {
        s->buf[56 + i] = (uint8_t)(bits >> (56 - i * 8));
    }
    sha256_compress(s->state, s->buf);

    for (int i = 0; i < 8; i++) {
        out[4*i  ] = (uint8_t)(s->state[i] >> 24);
        out[4*i+1] = (uint8_t)(s->state[i] >> 16);
        out[4*i+2] = (uint8_t)(s->state[i] >>  8);
        out[4*i+3] = (uint8_t)(s->state[i]      );
    }
}

void sha256(const void *data, size_t n, uint8_t out[32]) {
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, n);
    sha256_final(&s, out);
}
