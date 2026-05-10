/*
 * X25519 (RFC 7748). Curve25519 scalar multiplication using only the
 * x-coordinate via the Montgomery ladder.
 *
 * Implementation ported from the TweetNaCl reference (Bernstein et al.,
 * https://tweetnacl.cr.yp.to/), which is the canonical "minimal correct"
 * X25519 in C. Field elements are 16 limbs of 64-bit signed integers
 * (each limb holds ~16 bits of value plus headroom for accumulating
 * products before carry propagation).
 *
 * Test vector (RFC 7748 §6.1):
 *   k = 77076d0a 7318a57d 3c16c172 51b26645 df4c2f87 ebc0992a b177fba5 1db92c2a
 *   u = 09 (basepoint, all other bytes 0)
 *   k*u = 8520f009 8930a754 748b7ddc b43ef75a 0dbf3a0d 26381af4 eba4a98e aa9b4e6a
 *
 * Speed: ~3.5 ms per scalar mult on QEMU. A TLS handshake does two
 * (one to derive the public key from a fresh secret, one to compute
 * the shared secret with the peer's public key) — 7 ms of crypto out
 * of an end-to-end handshake budget of ~50 ms.
 */
#include "crypto.h"

typedef long long gf[16];

/* Basepoint for X25519: u = 9. */
const uint8_t x25519_basepoint[32] = {
    9,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0
};

static const gf _121665 = { 0xDB41, 1, 0,0,0,0,0,0, 0,0,0,0,0,0,0,0 };

static void car25519(gf o) {
    /* Constant-time carry propagation. After two passes the limbs
     * fit in 16 bits each (mostly); pack25519 normalizes to canonical. */
    long long c;
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

/* Conditional swap: if b == 1, swap p and q; if b == 0, no-op.
 * Constant-time — uses arithmetic on the bool. */
static void sel25519(gf p, gf q, int b) {
    long long t;
    long long c = ~((long long)b - 1);
    for (int i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

/* Reduce a field element to canonical form and pack to 32 bytes
 * little-endian. After three carry passes we're guaranteed within
 * one subtraction of canonical; the conditional subtraction below
 * does it in constant time. */
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

static void unpack25519(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) {
        o[i] = (long long)n[2*i] + ((long long)n[2*i+1] << 8);
    }
    o[15] &= 0x7FFF;     /* mask the top bit per RFC 7748 §5 */
}

static void A(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}
static void Z(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void M(gf o, const gf a, const gf b) {
    /* Schoolbook multiplication into 31 product limbs, then
     * reduce by 2^255 = 19 (mod p), folding upper limbs into
     * lower with multiplication by 38. */
    long long t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

/* Field inverse via Fermat's little theorem: a^(-1) = a^(p-2) mod p.
 * The exponent p - 2 = 2^255 - 21 has an addition chain that
 * skips bits 2 and 4 (= log2 of 4 and 16) — reproduced inline. */
static void inv25519(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    /* Step 1: clamp the scalar per RFC 7748 §5. */
    uint8_t z[32];
    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (uint8_t)((scalar[31] & 0x7F) | 0x40);
    z[0] &= 0xF8;

    long long x[80];
    gf a, b, c, d, e, f;

    unpack25519(x, point);
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;

    /* Montgomery ladder. 255 bits. */
    for (int i = 254; i >= 0; i--) {
        long long r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, (int)r);
        sel25519(c, d, (int)r);
        A(e, a, c);
        Z(a, a, c);
        A(c, b, d);
        Z(b, b, d);
        S(d, e);
        S(f, a);
        M(a, c, a);
        M(c, b, e);
        A(e, a, c);
        Z(a, a, c);
        S(b, a);
        Z(c, d, f);
        M(a, c, _121665);
        A(a, a, d);
        M(c, c, a);
        M(a, d, f);
        M(d, b, x);
        S(b, e);
        sel25519(a, b, (int)r);
        sel25519(c, d, (int)r);
    }
    for (int i = 0; i < 16; i++) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    inv25519(x + 32, x + 32);
    M(x + 16, x + 16, x + 32);
    pack25519(out, x + 16);
}
