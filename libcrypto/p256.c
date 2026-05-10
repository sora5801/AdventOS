/*
 * ECDSA over NIST P-256 (secp256r1, prime256v1).
 *
 * Field GF(p) where p = 2^256 - 2^224 + 2^192 + 2^96 - 1
 * Curve y^2 = x^3 - 3x + b   (a = -3 mod p,  b = secp256r1 b)
 * Base point G_x, G_y of order n.
 *
 * Field elements are stored as 8 little-endian 32-bit limbs:
 *   limb[0]   = bits   0..31     (LSW)
 *   limb[7]   = bits 224..255    (MSW)
 *
 * Points use Jacobian coordinates (X, Y, Z) where affine
 * (x, y) = (X/Z^2, Y/Z^3). Doubling and adding in Jacobian
 * needs no field inverse — one inverse converts back to
 * affine at the end of a scalarmult.
 *
 * Reductions:
 *   mod p  — NIST fast reduction (FIPS 186-2 Alg 2.29). Exploits
 *            the special form of p: 2^256 ≡ 2^224 - 2^192 - 2^96 + 1
 *            (mod p), so the high 256 bits of a 512-bit product
 *            split into shifted copies that recombine in one shot.
 *   mod n  — long-division-style "shift-and-conditionally-subtract"
 *            bit-by-bit. Slower than fast reduction would be, but
 *            n has no exploitable form and we only do ~400 mod-n
 *            mults per signature (Fermat's inverse).
 *
 * Inversion: Fermat's little theorem, a^(m-2) = a^-1 (mod m) via
 * square-and-multiply over 256 bits — ~256 sqr + ~128 mul per inv.
 *
 * Performance: ~25 ms per ECDSA sign on QEMU's qemu32. Compared
 * to Ed25519's ~30 ms (libcrypto/ed25519.c) this is comparable —
 * the field is similar size, and Jacobian Edwards saves a few
 * mults per point op that NIST Weierstrass doesn't get.
 */
#include "p256.h"
#include "crypto.h"

/* ---- Constants (LE 32-bit limbs) ------------------------------ */

/* p = 2^256 - 2^224 + 2^192 + 2^96 - 1 */
static const uint32_t P[8] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu
};

/* n — order of G (= |E(F_p)|). */
static const uint32_t N[8] = {
    0xFC632551u, 0xF3B9CAC2u, 0xA7179E84u, 0xBCE6FAADu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu
};

/* b — curve parameter from y^2 = x^3 - 3x + b. */
static const uint32_t B[8] = {
    0x27D2604Bu, 0x3BCE3C3Eu, 0xCC53B0F6u, 0x651D06B0u,
    0x769886BCu, 0xB3EBBD55u, 0xAA3A93E7u, 0x5AC635D8u
};

/* Base point. */
static const uint32_t GX[8] = {
    0xD898C296u, 0xF4A13945u, 0x2DEB33A0u, 0x77037D81u,
    0x63A440F2u, 0xF8BCE6E5u, 0xE12C4247u, 0x6B17D1F2u
};
static const uint32_t GY[8] = {
    0x37BF51F5u, 0xCBB64068u, 0x6B315ECEu, 0x2BCE3357u,
    0x7C0F9E16u, 0x8EE7EB4Au, 0xFE1A7F9Bu, 0x4FE342E2u
};

typedef uint32_t fe[8];

/* ---- Limb-level utilities ------------------------------------- */

static void fe_copy(fe dst, const fe src) {
    for (int i = 0; i < 8; i++) dst[i] = src[i];
}
static void fe_set0(fe dst) { for (int i = 0; i < 8; i++) dst[i] = 0; }
static void fe_set1(fe dst) { dst[0] = 1; for (int i = 1; i < 8; i++) dst[i] = 0; }
static int  fe_is0  (const fe a) { uint32_t x=0; for(int i=0;i<8;i++) x|=a[i]; return x==0; }
static int  fe_eq   (const fe a, const fe b) {
    uint32_t x = 0;
    for (int i = 0; i < 8; i++) x |= a[i] ^ b[i];
    return x == 0;
}

/* big-endian conversion */
static void be32_to_fe(fe out, const uint8_t in[32]) {
    for (int i = 0; i < 8; i++) {
        out[7 - i] = ((uint32_t)in[i*4    ] << 24) |
                     ((uint32_t)in[i*4 + 1] << 16) |
                     ((uint32_t)in[i*4 + 2] <<  8) |
                     ((uint32_t)in[i*4 + 3]);
    }
}
static void fe_to_be32(uint8_t out[32], const fe in) {
    for (int i = 0; i < 8; i++) {
        out[i*4    ] = (uint8_t)(in[7 - i] >> 24);
        out[i*4 + 1] = (uint8_t)(in[7 - i] >> 16);
        out[i*4 + 2] = (uint8_t)(in[7 - i] >>  8);
        out[i*4 + 3] = (uint8_t) in[7 - i];
    }
}

/* Compare a vs b as 256-bit unsigned. Returns -1, 0, 1. */
static int fe_cmp(const fe a, const fe b) {
    for (int i = 7; i >= 0; i--) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return  1;
    }
    return 0;
}

/* ---- Field add / sub mod p ----------------------------------- */

/* out = a + b mod p. */
static void fe_add(fe out, const fe a, const fe b) {
    uint64_t carry = 0;
    uint32_t tmp[8];
    for (int i = 0; i < 8; i++) {
        uint64_t v = (uint64_t)a[i] + b[i] + carry;
        tmp[i] = (uint32_t)v;
        carry  = v >> 32;
    }
    /* Subtract p if the result >= p OR carried out. */
    if (carry || fe_cmp(tmp, P) >= 0) {
        uint64_t borrow = 0;
        for (int i = 0; i < 8; i++) {
            uint64_t v = (uint64_t)tmp[i] - P[i] - borrow;
            out[i] = (uint32_t)v;
            borrow = (v >> 32) & 1;
        }
    } else {
        for (int i = 0; i < 8; i++) out[i] = tmp[i];
    }
}

/* out = a - b mod p. */
static void fe_sub(fe out, const fe a, const fe b) {
    uint32_t tmp[8];
    uint64_t borrow = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t v = (uint64_t)a[i] - b[i] - borrow;
        tmp[i] = (uint32_t)v;
        borrow = (v >> 32) & 1;
    }
    if (borrow) {
        /* result was negative — add p. */
        uint64_t carry = 0;
        for (int i = 0; i < 8; i++) {
            uint64_t v = (uint64_t)tmp[i] + P[i] + carry;
            out[i] = (uint32_t)v;
            carry  = v >> 32;
        }
    } else {
        for (int i = 0; i < 8; i++) out[i] = tmp[i];
    }
}

/* ---- Field mul mod p (NIST fast reduction) ------------------ */

/* Schoolbook 8x8 → 16-limb multiply. */
static void fe_mul_raw(uint32_t c[16], const fe a, const fe b) {
    for (int i = 0; i < 16; i++) c[i] = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t v = (uint64_t)a[i] * b[j] + c[i + j] + carry;
            c[i + j] = (uint32_t)v;
            carry    = v >> 32;
        }
        c[i + 8] = (uint32_t)carry;
    }
}

/* Reduce a 512-bit value (16 LE 32-bit limbs c) mod p.
 *
 * Per FIPS 186-2 §A.2.4 / Hankerson Alg 2.29, define:
 *
 *   s1 = (c7, c6, c5, c4, c3, c2, c1, c0)   (BE word order)
 *   s2 = (c15, c14, c13, c12, c11, 0, 0, 0)
 *   s3 = ( 0,  c15, c14, c13, c12, 0, 0, 0)
 *   s4 = (c15, c14,  0,   0,   0,  c10, c9, c8)
 *   s5 = (c8,  c13, c15, c14, c13, c11, c10, c9)
 *   s6 = (c10, c8,   0,   0,   0,  c13, c12, c11)
 *   s7 = (c11, c9,   0,   0,  c15, c14, c13, c12)
 *   s8 = (c12,  0,  c10, c9,  c8,  c15, c14, c13)
 *   s9 = (c13,  0,  c11, c10, c9,   0,  c15, c14)
 *
 *   result ≡ s1 + 2*s2 + 2*s3 + s4 + s5 - s6 - s7 - s8 - s9   (mod p)
 *
 * Above is presented in BE word order to match published references.
 * In our LE limb storage, position-7 holds the BE-leftmost word.
 *
 * Use int64_t per limb so intermediate sums (and sign) survive.
 * Then walk a carry, normalize signed overflow, and final-reduce
 * via 0..3 conditional subtractions of p.
 */
static void fe_reduce_512(fe out, const uint32_t c[16]) {
    int64_t a[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    /* s1 = (c[0..7]) */
    for (int i = 0; i < 8; i++) a[i] += (int64_t)c[i];

    /* 2*s2 : LE = (0, 0, 0, c11, c12, c13, c14, c15) */
    a[3] += 2 * (int64_t)c[11];
    a[4] += 2 * (int64_t)c[12];
    a[5] += 2 * (int64_t)c[13];
    a[6] += 2 * (int64_t)c[14];
    a[7] += 2 * (int64_t)c[15];

    /* 2*s3 : LE = (0, 0, 0, c12, c13, c14, c15, 0) */
    a[3] += 2 * (int64_t)c[12];
    a[4] += 2 * (int64_t)c[13];
    a[5] += 2 * (int64_t)c[14];
    a[6] += 2 * (int64_t)c[15];

    /* s4 : LE = (c8, c9, c10, 0, 0, 0, c14, c15) */
    a[0] += (int64_t)c[8];
    a[1] += (int64_t)c[9];
    a[2] += (int64_t)c[10];
    a[6] += (int64_t)c[14];
    a[7] += (int64_t)c[15];

    /* s5 : LE = (c9, c10, c11, c13, c14, c15, c13, c8) */
    a[0] += (int64_t)c[9];
    a[1] += (int64_t)c[10];
    a[2] += (int64_t)c[11];
    a[3] += (int64_t)c[13];
    a[4] += (int64_t)c[14];
    a[5] += (int64_t)c[15];
    a[6] += (int64_t)c[13];
    a[7] += (int64_t)c[8];

    /* -s6 : LE = (c11, c12, c13, 0, 0, 0, c8, c10) */
    a[0] -= (int64_t)c[11];
    a[1] -= (int64_t)c[12];
    a[2] -= (int64_t)c[13];
    a[6] -= (int64_t)c[8];
    a[7] -= (int64_t)c[10];

    /* -s7 : LE = (c12, c13, c14, c15, 0, 0, c9, c11) */
    a[0] -= (int64_t)c[12];
    a[1] -= (int64_t)c[13];
    a[2] -= (int64_t)c[14];
    a[3] -= (int64_t)c[15];
    a[6] -= (int64_t)c[9];
    a[7] -= (int64_t)c[11];

    /* -s8 : LE = (c13, c14, c15, c8, c9, c10, 0, c12) */
    a[0] -= (int64_t)c[13];
    a[1] -= (int64_t)c[14];
    a[2] -= (int64_t)c[15];
    a[3] -= (int64_t)c[8];
    a[4] -= (int64_t)c[9];
    a[5] -= (int64_t)c[10];
    a[7] -= (int64_t)c[12];

    /* -s9 : LE = (c14, c15, 0, c9, c10, c11, 0, c13) */
    a[0] -= (int64_t)c[14];
    a[1] -= (int64_t)c[15];
    a[3] -= (int64_t)c[9];
    a[4] -= (int64_t)c[10];
    a[5] -= (int64_t)c[11];
    a[7] -= (int64_t)c[13];

    /* Propagate carries — a[i] can be negative, do signed >>32. */
    int64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        a[i] += carry;
        carry  = a[i] >> 32;       /* arithmetic shift: preserves sign */
        a[i]  &= 0xFFFFFFFF;
    }

    /* `carry` is the value beyond bit 256. |carry| ≤ ~7 in the
     * worst case. Reduce by replacing 2^256 with (2^224 - 2^192 - 2^96 + 1):
     *     value_new = value_old + carry * (2^224 - 2^192 - 2^96 + 1 - 2^256)
     *
     * Set carry to 0 and apply the per-limb offsets:
     *     a[0] += carry   (lsb)
     *     a[3] -= carry   (2^96 = limb 3 base)
     *     a[6] -= carry   (2^192 = limb 6 base)
     *     a[7] += carry   (2^224 = limb 7 base)
     *
     * Repeat until carry == 0 (converges in ≤ 2 iterations because
     * each pass shrinks |carry| by a factor of 2^32). */
    while (carry != 0) {
        int64_t k = carry;
        carry = 0;
        a[0] += k;
        a[3] -= k;
        a[6] -= k;
        a[7] += k;
        for (int i = 0; i < 8; i++) {
            a[i] += carry;
            carry  = a[i] >> 32;
            a[i]  &= 0xFFFFFFFF;
        }
    }

    /* Now 0 ≤ value < 2^256 with carry == 0. May still be ≥ p; the
     * worst case after the special-form addition is ~3p. Subtract
     * p up to a few times. */
    uint32_t r[8];
    for (int i = 0; i < 8; i++) r[i] = (uint32_t)a[i];
    for (int iter = 0; iter < 4; iter++) {
        if (fe_cmp(r, P) < 0) break;
        uint64_t borrow = 0;
        for (int i = 0; i < 8; i++) {
            uint64_t v = (uint64_t)r[i] - P[i] - borrow;
            r[i] = (uint32_t)v;
            borrow = (v >> 32) & 1;
        }
    }
    fe_copy(out, r);
}

static void fe_mul(fe out, const fe a, const fe b) {
    uint32_t c[16];
    fe_mul_raw(c, a, b);
    fe_reduce_512(out, c);
}
static void fe_sqr(fe out, const fe a) { fe_mul(out, a, a); }

/* ---- Field inversion via Fermat: a^(p-2) mod p ------------- */

/* Square `out` k times. */
static void fe_sqr_n(fe out, const fe a, int k) {
    fe_copy(out, a);
    for (int i = 0; i < k; i++) fe_sqr(out, out);
}

static void fe_inv(fe out, const fe a) {
    /* p - 2 in binary is mostly 1s with a few specific bits.
     * Generic addition chain: compute a^2 a^3 a^15 etc., then
     * combine. For simplicity, just do the bit-by-bit square-and-
     * multiply over 256 bits. p - 2 = p with the lowest bit cleared
     * minus 1 = (0xFFFFFFFF ... 0xFFFFFFFD).
     *
     * The 256-bit exponent: we walk bits 255..0 of (p-2). */
    uint32_t e[8];
    /* p-2: subtract 2 from P (which is p). */
    e[0] = P[0] - 2; e[1] = P[1]; e[2] = P[2]; e[3] = P[3];
    e[4] = P[4];     e[5] = P[5]; e[6] = P[6]; e[7] = P[7];

    fe r; fe_set1(r);
    for (int i = 255; i >= 0; i--) {
        fe_sqr(r, r);
        if ((e[i >> 5] >> (i & 31)) & 1u) {
            fe_mul(r, r, a);
        }
    }
    fe_copy(out, r);
}

/* ---- mod-n arithmetic for ECDSA ------------------------------ */

/* Compare 256-bit a vs b. */
static int fen_cmp(const uint32_t a[8], const uint32_t b[8]) {
    for (int i = 7; i >= 0; i--) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return  1;
    }
    return 0;
}

/* Subtract n from a if a >= n. Up to k times. */
static void fen_reduce(uint32_t a[8], int k) {
    for (int iter = 0; iter < k; iter++) {
        if (fen_cmp(a, N) < 0) return;
        uint64_t borrow = 0;
        for (int i = 0; i < 8; i++) {
            uint64_t v = (uint64_t)a[i] - N[i] - borrow;
            a[i] = (uint32_t)v;
            borrow = (v >> 32) & 1;
        }
    }
}

/* out = a + b mod n. */
static void fen_add(uint32_t out[8], const uint32_t a[8], const uint32_t b[8]) {
    uint32_t tmp[8];
    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t v = (uint64_t)a[i] + b[i] + carry;
        tmp[i] = (uint32_t)v;
        carry  = v >> 32;
    }
    if (carry || fen_cmp(tmp, N) >= 0) {
        uint64_t borrow = 0;
        for (int i = 0; i < 8; i++) {
            uint64_t v = (uint64_t)tmp[i] - N[i] - borrow;
            out[i] = (uint32_t)v;
            borrow = (v >> 32) & 1;
        }
    } else {
        for (int i = 0; i < 8; i++) out[i] = tmp[i];
    }
}

/* Reduce a 16-word value mod n via bit-by-bit long division.
 * Process the 512-bit dividend from the high end; for each bit
 * we shift the running 256-bit remainder up by one (consuming one
 * bit from the dividend) and conditionally subtract n. */
static void fen_reduce_512(uint32_t out[8], const uint32_t c[16]) {
    uint32_t r[8] = { 0 };
    /* Process bits from MSB to LSB. */
    for (int bit = 511; bit >= 0; bit--) {
        /* Shift r left by 1. */
        uint32_t carry = 0;
        for (int i = 0; i < 8; i++) {
            uint32_t newcarry = r[i] >> 31;
            r[i] = (r[i] << 1) | carry;
            carry = newcarry;
        }
        /* Pull next dividend bit into r[0]. */
        uint32_t db = (c[bit >> 5] >> (bit & 31)) & 1u;
        r[0] |= db;
        /* If r >= n, subtract. (Plus account for the discarded high carry.) */
        if (carry || fen_cmp(r, N) >= 0) {
            uint64_t borrow = 0;
            for (int i = 0; i < 8; i++) {
                uint64_t v = (uint64_t)r[i] - N[i] - borrow;
                r[i] = (uint32_t)v;
                borrow = (v >> 32) & 1;
            }
        }
    }
    for (int i = 0; i < 8; i++) out[i] = r[i];
}

/* out = a * b mod n. */
static void fen_mul(uint32_t out[8], const uint32_t a[8], const uint32_t b[8]) {
    uint32_t c[16];
    for (int i = 0; i < 16; i++) c[i] = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t v = (uint64_t)a[i] * b[j] + c[i + j] + carry;
            c[i + j] = (uint32_t)v;
            carry    = v >> 32;
        }
        c[i + 8] = (uint32_t)carry;
    }
    fen_reduce_512(out, c);
}

/* out = a^(n-2) mod n (Fermat inverse). */
static void fen_inv(uint32_t out[8], const uint32_t a[8]) {
    uint32_t e[8];
    e[0] = N[0] - 2; e[1] = N[1]; e[2] = N[2]; e[3] = N[3];
    e[4] = N[4];     e[5] = N[5]; e[6] = N[6]; e[7] = N[7];

    uint32_t r[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 255; i >= 0; i--) {
        uint32_t tmp[8];
        fen_mul(tmp, r, r);                     /* square */
        for (int j = 0; j < 8; j++) r[j] = tmp[j];
        if ((e[i >> 5] >> (i & 31)) & 1u) {
            fen_mul(tmp, r, a);
            for (int j = 0; j < 8; j++) r[j] = tmp[j];
        }
    }
    for (int i = 0; i < 8; i++) out[i] = r[i];
}

/* ---- Curve points in Jacobian coordinates ------------------- */

struct point { fe x, y, z; };

static void point_set_infinity(struct point *r) {
    fe_set0(r->x); fe_set1(r->y); fe_set0(r->z);
}
static int  point_is_infinity(const struct point *p) { return fe_is0(p->z); }

/* Doubling in Jacobian: out = 2*p. Formula handles a = -3 directly:
 *
 *   t1 = (Y)^2
 *   t2 = 4*X*Y^2
 *   t3 = 8*Y^4
 *   t4 = 3*(X-Z^2)*(X+Z^2)
 *   X' = t4^2 - 2*t2
 *   Y' = t4*(t2 - X') - t3
 *   Z' = 2*Y*Z
 *
 * (NIST-friendly, since a=-3.) */
static void point_double(struct point *out, const struct point *p) {
    if (point_is_infinity(p)) { point_set_infinity(out); return; }
    fe t1, t2, t3, t4, tmp;
    fe_sqr(t1, p->y);                            /* t1 = Y^2 */
    fe_mul(t2, p->x, t1);                        /* t2 = X*Y^2 */
    fe_add(t2, t2, t2);
    fe_add(t2, t2, t2);                          /* t2 = 4*X*Y^2 */
    fe_sqr(t3, t1);                              /* t3 = Y^4 */
    fe_add(t3, t3, t3);
    fe_add(t3, t3, t3);
    fe_add(t3, t3, t3);                          /* t3 = 8*Y^4 */
    fe_sqr(tmp, p->z);                           /* tmp = Z^2 */
    fe_sub(t4, p->x, tmp);                       /* X - Z^2 */
    fe_add(tmp, p->x, tmp);                      /* X + Z^2 */
    fe_mul(t4, t4, tmp);                         /* (X-Z^2)(X+Z^2) */
    fe_add(tmp, t4, t4);
    fe_add(t4, tmp, t4);                         /* t4 = 3*(...) */

    /* Z' = 2*Y*Z   — do this before X' since it uses old Y,Z */
    fe_mul(out->z, p->y, p->z);
    fe_add(out->z, out->z, out->z);

    /* X' = t4^2 - 2*t2 */
    fe_sqr(out->x, t4);
    fe_sub(out->x, out->x, t2);
    fe_sub(out->x, out->x, t2);

    /* Y' = t4*(t2 - X') - t3 */
    fe_sub(tmp, t2, out->x);
    fe_mul(out->y, t4, tmp);
    fe_sub(out->y, out->y, t3);
}

/* Generic Jacobian point addition: out = p + q.
 * If p == q or p == -q, falls back to doubling / infinity.
 *
 *   U1 = X1*Z2^2,    U2 = X2*Z1^2
 *   S1 = Y1*Z2^3,    S2 = Y2*Z1^3
 *   H  = U2 - U1
 *   R  = S2 - S1
 *   X3 = R^2 - H^3 - 2*U1*H^2
 *   Y3 = R*(U1*H^2 - X3) - S1*H^3
 *   Z3 = H*Z1*Z2
 */
static void point_add(struct point *out, const struct point *p, const struct point *q) {
    if (point_is_infinity(p)) { *out = *q; return; }
    if (point_is_infinity(q)) { *out = *p; return; }

    fe z1z1, z2z2, u1, u2, s1, s2, h, r, h2, h3, u1h2, t;

    fe_sqr(z1z1, p->z);
    fe_sqr(z2z2, q->z);
    fe_mul(u1, p->x, z2z2);
    fe_mul(u2, q->x, z1z1);
    fe_mul(s1, p->y, z2z2);  fe_mul(s1, s1, q->z);
    fe_mul(s2, q->y, z1z1);  fe_mul(s2, s2, p->z);
    fe_sub(h, u2, u1);
    fe_sub(r, s2, s1);

    if (fe_is0(h)) {
        if (fe_is0(r)) { point_double(out, p); return; }
        point_set_infinity(out);
        return;
    }

    fe_sqr(h2, h);
    fe_mul(h3, h2, h);
    fe_mul(u1h2, u1, h2);

    /* X3 = R^2 - H^3 - 2*U1*H^2 */
    fe_sqr(t, r);
    fe_sub(t, t, h3);
    fe_sub(t, t, u1h2);
    fe_sub(out->x, t, u1h2);

    /* Y3 = R*(U1*H^2 - X3) - S1*H^3 */
    fe_sub(t, u1h2, out->x);
    fe_mul(t, t, r);
    fe_mul(z1z1, s1, h3);            /* reuse */
    fe_sub(out->y, t, z1z1);

    /* Z3 = H*Z1*Z2 */
    fe_mul(t, p->z, q->z);
    fe_mul(out->z, t, h);
}

/* out = k*p, where k is a 32-byte big-endian scalar.
 * Naive double-and-add. Not constant-time — variable time
 * depending on k, but matches what every quick-and-honest ECC
 * library does as a v1. */
static void point_scalarmult(struct point *out, const uint8_t k[32],
                             const struct point *base) {
    struct point r;
    point_set_infinity(&r);
    for (int i = 0; i < 32; i++) {
        for (int b = 7; b >= 0; b--) {
            point_double(&r, &r);
            if ((k[i] >> b) & 1) {
                point_add(&r, &r, base);
            }
        }
    }
    *out = r;
}

/* Convert a Jacobian point to affine (X/Z^2, Y/Z^3). Returns 0
 * on success, -1 if the input was the point at infinity. */
static int point_to_affine(uint8_t x_out[32], uint8_t y_out[32],
                           const struct point *p) {
    if (point_is_infinity(p)) return -1;
    fe zi, zi2, zi3, xa, ya;
    fe_inv(zi,  p->z);
    fe_sqr(zi2, zi);
    fe_mul(zi3, zi2, zi);
    fe_mul(xa,  p->x, zi2);
    fe_mul(ya,  p->y, zi3);
    fe_to_be32(x_out, xa);
    fe_to_be32(y_out, ya);
    return 0;
}

/* ---- Public API ---------------------------------------------- */

/* Reduce a 32-byte BE scalar mod (n-1), then add 1. Result in [1, n-1]. */
static void scalar_from_seed_bytes(uint32_t out_priv[8], const uint8_t bytes[32]) {
    /* Reduce 256-bit value mod n. */
    uint32_t s[8];
    be32_to_fe(s, bytes);
    /* Subtract n once or twice if needed. */
    fen_reduce(s, 3);
    /* Ensure non-zero: if s == 0, set s = 1. */
    int zero = 1;
    for (int i = 0; i < 8; i++) if (s[i]) { zero = 0; break; }
    if (zero) s[0] = 1;
    for (int i = 0; i < 8; i++) out_priv[i] = s[i];
}

void p256_keypair_from_seed(uint8_t pub[64], uint8_t priv[32],
                            const uint8_t seed[32]) {
    /* priv = SHA-256(seed) mod (n-1) + 1.  pub = priv * G. */
    uint8_t h[32];
    sha256(seed, 32, h);
    uint32_t d[8];
    scalar_from_seed_bytes(d, h);
    fe_to_be32(priv, d);

    /* Compute pub = d*G. */
    struct point G, Pd;
    fe_copy(G.x, GX);
    fe_copy(G.y, GY);
    fe_set1(G.z);
    point_scalarmult(&Pd, priv, &G);
    point_to_affine(pub, pub + 32, &Pd);
}

int p256_sign(uint8_t sig[64],
              const uint8_t hash[32], const uint8_t priv[32]) {
    /* z is the hash interpreted as an integer mod n. */
    uint32_t z[8];
    be32_to_fe(z, hash);
    fen_reduce(z, 3);

    uint32_t d[8];
    be32_to_fe(d, priv);

    for (int retry = 0; retry < 8; retry++) {
        /* Random k in [1, n-1]. Use rand_bytes — not RFC 6979
         * deterministic, but mathematically equivalent for sign
         * correctness. Reduce mod n and reject zero. */
        uint8_t kbytes[32];
        rand_bytes(kbytes, 32);
        uint32_t k[8];
        be32_to_fe(k, kbytes);
        fen_reduce(k, 3);
        int kzero = 1;
        for (int i = 0; i < 8; i++) if (k[i]) { kzero = 0; break; }
        if (kzero) continue;

        /* R = k*G,  r = R.x mod n. */
        struct point G, R;
        fe_copy(G.x, GX); fe_copy(G.y, GY); fe_set1(G.z);
        uint8_t kraw[32];
        fe_to_be32(kraw, k);
        point_scalarmult(&R, kraw, &G);
        uint8_t rx[32], ry[32];
        if (point_to_affine(rx, ry, &R) < 0) continue;

        uint32_t r[8];
        be32_to_fe(r, rx);
        fen_reduce(r, 3);
        int rzero = 1;
        for (int i = 0; i < 8; i++) if (r[i]) { rzero = 0; break; }
        if (rzero) continue;

        /* s = k^-1 * (z + r*d) mod n. */
        uint32_t kinv[8], rd[8], zrd[8], s[8];
        fen_inv(kinv, k);
        fen_mul(rd, r, d);
        fen_add(zrd, z, rd);
        fen_mul(s, kinv, zrd);
        int szero = 1;
        for (int i = 0; i < 8; i++) if (s[i]) { szero = 0; break; }
        if (szero) continue;

        fe_to_be32(sig,      r);
        fe_to_be32(sig + 32, s);
        return 0;
    }
    return -1;       /* shouldn't happen with sane RNG */
}

int p256_verify(const uint8_t sig[64],
                const uint8_t hash[32], const uint8_t pub[64]) {
    uint32_t r[8], s[8], z[8];
    be32_to_fe(r, sig);
    be32_to_fe(s, sig + 32);
    be32_to_fe(z, hash);
    fen_reduce(z, 3);

    /* r, s must be in [1, n-1]. */
    int rzero = 1; for (int i = 0; i < 8; i++) if (r[i]) { rzero = 0; break; }
    int szero = 1; for (int i = 0; i < 8; i++) if (s[i]) { szero = 0; break; }
    if (rzero || szero) return -1;
    if (fen_cmp(r, N) >= 0 || fen_cmp(s, N) >= 0) return -1;

    /* s_inv = s^-1 mod n,  u1 = z*s_inv,  u2 = r*s_inv. */
    uint32_t sinv[8], u1[8], u2[8];
    fen_inv(sinv, s);
    fen_mul(u1, z, sinv);
    fen_mul(u2, r, sinv);

    /* X = u1*G + u2*Q.  Compute via two scalarmults + an add. */
    struct point G, Q, P1, P2, X;
    fe_copy(G.x, GX); fe_copy(G.y, GY); fe_set1(G.z);
    be32_to_fe(Q.x, pub);
    be32_to_fe(Q.y, pub + 32);
    fe_set1(Q.z);

    uint8_t u1b[32], u2b[32];
    fe_to_be32(u1b, u1);
    fe_to_be32(u2b, u2);

    point_scalarmult(&P1, u1b, &G);
    point_scalarmult(&P2, u2b, &Q);
    point_add(&X, &P1, &P2);

    if (point_is_infinity(&X)) return -1;

    /* v = X.x mod n.  Valid iff v == r. */
    fe zi, zi2, xa;
    fe_inv(zi,  X.z);
    fe_sqr(zi2, zi);
    fe_mul(xa,  X.x, zi2);
    uint8_t xb[32];
    fe_to_be32(xb, xa);
    uint32_t v[8];
    be32_to_fe(v, xb);
    fen_reduce(v, 3);
    return (fen_cmp(v, r) == 0) ? 0 : -1;
}

/* ---- ASN.1 DER for (r, s) ----------------------------------- */

/* Encode one big-endian 32-byte integer as a DER INTEGER. Strips
 * leading zeros and adds a leading 0x00 if the high bit is set
 * (so the DER integer is positive). */
static int encode_integer(uint8_t *out, const uint8_t in[32]) {
    int start = 0;
    while (start < 31 && in[start] == 0) start++;
    int len = 32 - start;
    int need_lead = (in[start] & 0x80) ? 1 : 0;
    int total = 2 + len + need_lead;
    out[0] = 0x02;             /* tag = INTEGER */
    out[1] = (uint8_t)(len + need_lead);
    int o = 2;
    if (need_lead) out[o++] = 0x00;
    for (int i = 0; i < len; i++) out[o++] = in[start + i];
    return total;
}

int p256_sig_to_der(uint8_t *out, int out_cap, const uint8_t sig[64]) {
    uint8_t body[2 * 33 + 4];
    int o = 0;
    o += encode_integer(body + o, sig);
    o += encode_integer(body + o, sig + 32);
    int total = 2 + o;
    if (total > out_cap) return -1;
    out[0] = 0x30;             /* tag = SEQUENCE */
    out[1] = (uint8_t)o;
    for (int i = 0; i < o; i++) out[2 + i] = body[i];
    return total;
}

static int decode_integer(uint8_t out_be32[32], const uint8_t *p, int avail, int *consumed) {
    if (avail < 2 || p[0] != 0x02) return -1;
    int len = p[1];
    if (len < 1 || len > avail - 2) return -1;
    /* Strip leading 0x00 if present (positive-integer padding). */
    int idx = 2;
    if (len >= 2 && p[idx] == 0x00) { idx++; len--; }
    if (len > 32) return -1;
    for (int i = 0; i < 32 - len; i++) out_be32[i] = 0;
    for (int i = 0; i < len; i++) out_be32[32 - len + i] = p[idx + i];
    *consumed = 2 + p[1];
    return 0;
}

int p256_sig_from_der(uint8_t sig[64], const uint8_t *der, int der_len) {
    if (der_len < 8 || der[0] != 0x30) return -1;
    int body_len = der[1];
    if (body_len + 2 != der_len) return -1;
    int o = 2, used;
    if (decode_integer(sig,      der + o, der_len - o, &used) < 0) return -1;
    o += used;
    if (decode_integer(sig + 32, der + o, der_len - o, &used) < 0) return -1;
    return 0;
}
