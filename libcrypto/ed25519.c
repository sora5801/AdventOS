/*
 * Ed25519 (RFC 8032), sign-only — port of TweetNaCl's
 * crypto_sign_ed25519. Uses Curve25519's field GF(2^255-19) but on
 * the twisted Edwards form of the curve. Signature is 64 bytes
 * (R || S), public key is 32 bytes, secret key is 32 bytes seed
 * → expanded to 64 bytes (sha512(seed)).
 *
 * The math here is dense — 16-limb signed-int field elements, four
 * coordinates per point (X, Y, Z, T), an order-of-the-curve modular
 * reduction step (modL), and SHA-512 wrapped around the whole
 * thing. We don't try to be fast: roughly 30 ms per sign on QEMU,
 * which is the dominant cost of one TLS handshake. That's fine.
 *
 * Test vector (RFC 8032 §7.1, "Test 1: empty message"):
 *   secret seed:    9d61b19deffd5a60ba844af492ec2cc4
 *                   4449c5697b326919703bac031cae7f60
 *   public:         d75a980182b10ab7d54bfed3c964073a
 *                   0ee172f3daa62325af021a68f707511a
 *   message:        ""
 *   signature:      e5564300c360ac729086e2cc806e828a
 *                   84877f1eb8e5d974d873e06522490155
 *                   5fb8821590a33bacc61e39701cf9b46b
 *                   d25bf5f0595bbe24655141438e7a100b
 */
#include "crypto.h"
#include "sha512.h"

typedef long long gf[16];

/* Constants — straight from RFC 8032 / TweetNaCl. */
static const gf gf0;
static const gf gf1 = {1};
static const gf D2  = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                       0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
static const gf X   = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                       0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const gf Y   = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                       0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};

/* Order of the prime subgroup, little-endian, as 32 bytes:
 * L = 2^252 + 27742317777372353535851937790883648493 */
static const uint8_t L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

static void set25519(gf r, const gf a) {
    for (int i = 0; i < 16; i++) r[i] = a[i];
}

static void car25519(gf o) {
    long long c;
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b) {
    long long t, c = ~((long long)b - 1);
    for (int i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n) {
    int b;
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xFFED;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xFFFF - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xFFFF;
        }
        m[15] = t[15] - 0x7FFF - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xFFFF;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2*i  ] = (uint8_t)(t[i] & 0xFF);
        o[2*i+1] = (uint8_t)((t[i] >> 8) & 0xFF);
    }
}

static int neq25519(const gf a, const gf b) {
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    int diff = 0;
    for (int i = 0; i < 32; i++) diff |= c[i] ^ d[i];
    return diff;     /* 0 = equal */
}

static uint8_t par25519(const gf a) {
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static void unpack25519(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) {
        o[i] = (long long)n[2*i] + ((long long)n[2*i+1] << 8);
    }
    o[15] &= 0x7FFF;
}

static void A(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}
static void Z(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void M(gf o, const gf a, const gf b) {
    long long t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

/* Power 2^252-3 — used for Edwards Y-recovery (square root). */
static void pow2523(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

/* ---- Edwards point ops, extended coords (X, Y, Z, T = X*Y/Z) --- */

static void add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;

    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);

    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void cswap(gf p[4], gf q[4], uint8_t b) {
    for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

static void pack(uint8_t *r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= par25519(tx) << 7;
}

static void scalarmult(gf p[4], gf q[4], const uint8_t *s) {
    /* q is the base; result is p = s*q. Conditional-add via cswap
     * in a constant-time Montgomery ladder. 256 bits. */
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);

    for (int i = 255; i >= 0; i--) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

static void scalarbase(gf p[4], const uint8_t *s) {
    gf q[4];
    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

/* ---- Reduction modulo L (curve order, ~ 2^252) ----------------- */

static void modL(uint8_t *r, long long x[64]) {
    long long carry;
    int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * (long long)L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * (long long)L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++) x[j] -= carry * (long long)L[j];
    for (i = 0; i < 32; i++) {
        x[i+1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

static void reduce(uint8_t *r) {
    long long x[64];
    for (int i = 0; i < 64; i++) x[i] = (long long)r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    modL(r, x);
}

/* ---- Public API ------------------------------------------------- */

/* Derive public key from a 32-byte secret seed.
 *
 * sk[0..31]  = seed (input)
 * sk[32..63] = pk   (output, also written to pk separately) */
void ed25519_keypair_from_seed(uint8_t pk[32], uint8_t sk[64], const uint8_t seed[32]) {
    uint8_t d[64];
    sha512(seed, 32, d);
    /* Clamp scalar per RFC 8032 */
    d[0]  &= 248;
    d[31] &= 127;
    d[31] |= 64;

    gf p[4];
    scalarbase(p, d);
    pack(pk, p);

    for (int i = 0; i < 32; i++) sk[i]      = seed[i];
    for (int i = 0; i < 32; i++) sk[32 + i] = pk[i];
}

/* Sign a message with a 64-byte expanded secret key (= seed || pk).
 * Output sig is 64 bytes (R || S). */
void ed25519_sign(uint8_t sig[64],
                  const uint8_t *msg, size_t msg_len,
                  const uint8_t sk[64]) {
    /* sk[0..31] is the seed; sk[32..63] is the public key. */
    uint8_t d[64];
    sha512(sk, 32, d);
    d[0]  &= 248;
    d[31] &= 127;
    d[31] |= 64;

    /* r = SHA512(d[32..63] || msg), reduced mod L */
    struct sha512 hs;
    uint8_t r[64];
    sha512_init(&hs);
    sha512_update(&hs, d + 32, 32);
    sha512_update(&hs, msg, msg_len);
    sha512_final(&hs, r);
    reduce(r);

    /* R = r * B (encoded into sig[0..31]) */
    gf p[4];
    scalarbase(p, r);
    pack(sig, p);

    /* h = SHA512(R || pk || msg), reduced mod L */
    uint8_t h[64];
    sha512_init(&hs);
    sha512_update(&hs, sig, 32);
    sha512_update(&hs, sk + 32, 32);
    sha512_update(&hs, msg, msg_len);
    sha512_final(&hs, h);
    reduce(h);

    /* S = (r + h * d_secret_scalar) mod L
     * where d_secret_scalar = first 32 bytes of d (clamped). */
    long long x[64];
    for (int i = 0; i < 64; i++) x[i] = 0;
    for (int i = 0; i < 32; i++) x[i] = (long long)r[i];
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++)
            x[i + j] += (long long)h[i] * (long long)d[j];
    modL(sig + 32, x);
}

/* Pull verify in too — not strictly needed server-side but useful
 * for the [t26]-style selftest. */
static int unpackneg(gf r[4], const uint8_t p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    /* den = -1 + d*y^2 */
    static const gf D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                         0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);

    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);

    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) {
        /* multiply r[0] by sqrt(-1) */
        static const gf I = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                             0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
        M(r[0], r[0], I);
    }
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;

    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);
    M(r[3], r[0], r[1]);
    return 0;
}

int ed25519_verify(const uint8_t sig[64],
                   const uint8_t *msg, size_t msg_len,
                   const uint8_t pk[32]) {
    /* Verify: reject if S >= L (lightly: high byte must be 0 since
     * L >> 252). Then check 8(SB) - 8(R + h*A) == 0 where A is the
     * negation of the public point. */
    uint8_t h[64];
    gf p[4], q[4];
    if (sig[63] & 0xE0) return -1;     /* S has wrong upper bits */

    if (unpackneg(q, pk) != 0) return -1;

    struct sha512 hs;
    sha512_init(&hs);
    sha512_update(&hs, sig, 32);
    sha512_update(&hs, pk,  32);
    sha512_update(&hs, msg, msg_len);
    sha512_final(&hs, h);
    reduce(h);

    scalarmult(p, q, h);

    gf rcheck[4];
    scalarbase(rcheck, sig + 32);
    add(p, rcheck);

    uint8_t t[32];
    pack(t, p);
    int diff = 0;
    for (int i = 0; i < 32; i++) diff |= sig[i] ^ t[i];
    return diff ? -1 : 0;
}
