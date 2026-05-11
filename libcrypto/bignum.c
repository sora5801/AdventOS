/*
 * AdventOS libcrypto — bignum implementation (session 58).
 *
 * 32-bit-limb arithmetic, schoolbook everywhere. Designed to be obvious
 * rather than fast; RSA-2048 sign with CRT takes a few hundred
 * milliseconds in QEMU which is plenty for a hobby OS. See bignum.h
 * for the high-level shape and what's deliberately missing.
 */

#include "bignum.h"

/* ---------------------------------------------------------------- */
/* Small inlines                                                    */
/* ---------------------------------------------------------------- */

static void bn_trim(bignum *b) {
    while (b->n > 0 && b->v[b->n - 1] == 0) b->n--;
}

static void bn_clear_high(bignum *b) {
    /* Force v[n..MAX] = 0. We rely on this everywhere — bn_cmp,
     * bn_mul, etc. read up to v[max(a->n, b->n)] and would otherwise
     * see leftover data from a previous larger value. */
    for (int i = b->n; i < BN_MAX_LIMBS; i++) b->v[i] = 0;
}

void bn_zero(bignum *b) {
    for (int i = 0; i < BN_MAX_LIMBS; i++) b->v[i] = 0;
    b->n = 0;
}

void bn_set_u32(bignum *b, uint32_t value) {
    bn_zero(b);
    if (value) { b->v[0] = value; b->n = 1; }
}

void bn_copy(bignum *r, const bignum *a) {
    for (int i = 0; i < BN_MAX_LIMBS; i++) r->v[i] = a->v[i];
    r->n = a->n;
}

/* ---------------------------------------------------------------- */
/* Byte conversion (big-endian)                                     */
/* ---------------------------------------------------------------- */

void bn_from_bytes_be(bignum *b, const uint8_t *buf, size_t n) {
    bn_zero(b);
    /* Walk the input MSB-first, shifting and OR'ing each byte. */
    for (size_t i = 0; i < n; i++) {
        /* shift left by 8 */
        uint32_t carry = 0;
        for (int j = 0; j < BN_MAX_LIMBS; j++) {
            uint64_t cur = ((uint64_t)b->v[j] << 8) | carry;
            b->v[j] = (uint32_t)cur;
            carry = (uint32_t)(cur >> 32);
        }
        b->v[0] |= buf[i];
    }
    b->n = BN_MAX_LIMBS;
    bn_trim(b);
}

void bn_to_bytes_be(const bignum *b, uint8_t *buf, size_t n) {
    /* Fill buf MSB-first with the low n bytes of the value. */
    bignum t;
    bn_copy(&t, b);
    for (int i = (int)n - 1; i >= 0; i--) {
        buf[i] = (uint8_t)(t.v[0] & 0xFF);
        /* right-shift by 8 */
        uint32_t carry = 0;
        for (int j = BN_MAX_LIMBS - 1; j >= 0; j--) {
            uint32_t lo = t.v[j];
            t.v[j] = (lo >> 8) | (carry << 24);
            carry = lo & 0xFF;
        }
    }
}

int bn_bit_length(const bignum *b) {
    if (b->n == 0) return 0;
    uint32_t top = b->v[b->n - 1];
    int bits = 32;
    while (bits > 0 && !(top & (1u << (bits - 1)))) bits--;
    return (b->n - 1) * 32 + bits;
}

int bn_byte_length(const bignum *b) {
    return (bn_bit_length(b) + 7) / 8;
}

/* ---------------------------------------------------------------- */
/* Comparison + bit queries                                         */
/* ---------------------------------------------------------------- */

int bn_cmp(const bignum *a, const bignum *b) {
    if (a->n != b->n) return a->n > b->n ? 1 : -1;
    for (int i = a->n - 1; i >= 0; i--) {
        if (a->v[i] != b->v[i]) return a->v[i] > b->v[i] ? 1 : -1;
    }
    return 0;
}

int bn_is_zero(const bignum *b) { return b->n == 0; }

int bn_is_odd(const bignum *b) {
    return b->n > 0 && (b->v[0] & 1u);
}

int bn_bit_at(const bignum *b, int bit) {
    int limb = bit >> 5;
    int shift = bit & 31;
    if (limb >= b->n) return 0;
    return (int)((b->v[limb] >> shift) & 1u);
}

/* ---------------------------------------------------------------- */
/* Add / subtract                                                   */
/* ---------------------------------------------------------------- */

void bn_add(bignum *r, const bignum *a, const bignum *b) {
    int n = a->n > b->n ? a->n : b->n;
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = (uint64_t)a->v[i] + b->v[i] + carry;
        r->v[i] = (uint32_t)s;
        carry = s >> 32;
    }
    if (carry) {
        r->v[n++] = (uint32_t)carry;
    }
    r->n = n;
    for (int i = n; i < BN_MAX_LIMBS; i++) r->v[i] = 0;
    bn_trim(r);
}

int bn_sub(bignum *r, const bignum *a, const bignum *b) {
    /* Caller's contract: a >= b. We still do the borrow check and
     * return -1 if it walks off the top so debug pings are loud. */
    if (bn_cmp(a, b) < 0) return -1;
    int n = a->n;
    int64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        int64_t diff = (int64_t)a->v[i] - (int64_t)b->v[i] - borrow;
        if (diff < 0) {
            diff += (int64_t)1 << 32;
            borrow = 1;
        } else {
            borrow = 0;
        }
        r->v[i] = (uint32_t)diff;
    }
    r->n = n;
    for (int i = n; i < BN_MAX_LIMBS; i++) r->v[i] = 0;
    bn_trim(r);
    return 0;
}

void bn_lshift1(bignum *b) {
    uint32_t carry = 0;
    for (int i = 0; i < BN_MAX_LIMBS; i++) {
        uint32_t lo = b->v[i];
        b->v[i] = (lo << 1) | carry;
        carry = lo >> 31;
    }
    if (b->v[BN_MAX_LIMBS - 1] != 0) {
        /* High limb fell off — undefined behavior. Reset.
         * Caller should have known the result fits. */
        b->n = BN_MAX_LIMBS;
    } else {
        /* Expand n if the new top bit pushed us into a new limb. */
        if (b->n < BN_MAX_LIMBS && b->v[b->n] != 0) b->n++;
    }
    bn_trim(b);
}

void bn_rshift1(bignum *b) {
    uint32_t carry = 0;
    for (int i = BN_MAX_LIMBS - 1; i >= 0; i--) {
        uint32_t lo = b->v[i];
        b->v[i] = (lo >> 1) | (carry << 31);
        carry = lo & 1u;
    }
    bn_trim(b);
}

/* ---------------------------------------------------------------- */
/* Multiplication                                                   */
/* ---------------------------------------------------------------- */

/* Schoolbook: r[i+j] += a[i] * b[j].  r must not alias a or b
 * (caller's job to use a scratch). The classic O(n^2) version with
 * a 64-bit accumulator per inner step. */
void bn_mul(bignum *r, const bignum *a, const bignum *b) {
    int an = a->n, bn = b->n;
    int rn = an + bn;
    if (rn > BN_MAX_LIMBS) rn = BN_MAX_LIMBS;
    /* Zero r up front — easier than tracking which limbs were touched. */
    for (int i = 0; i < BN_MAX_LIMBS; i++) r->v[i] = 0;

    for (int i = 0; i < an; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < bn && (i + j) < BN_MAX_LIMBS; j++) {
            uint64_t prod = (uint64_t)a->v[i] * b->v[j] + r->v[i + j] + carry;
            r->v[i + j] = (uint32_t)prod;
            carry = prod >> 32;
        }
        if ((i + bn) < BN_MAX_LIMBS) {
            uint64_t s = (uint64_t)r->v[i + bn] + carry;
            r->v[i + bn] = (uint32_t)s;
            /* If `s` overflowed (carry-from-carry), we'd need to
             * propagate further. With BN_MAX_LIMBS >> what RSA uses,
             * this is impossible for legitimate inputs; we don't
             * bother handling it. */
        }
    }
    r->n = rn;
    bn_trim(r);
}

/* ---------------------------------------------------------------- */
/* Long division: r = a mod m                                       */
/* ---------------------------------------------------------------- */

/* Bit-by-bit "shift left, subtract if >= m" loop. The textbook
 * algorithm; O(bits(a) * limbs(m)). For 2048-bit modpow this is the
 * inner hot path. */
void bn_mod(bignum *r, const bignum *a, const bignum *m) {
    if (bn_cmp(a, m) < 0) { bn_copy(r, a); return; }

    bignum rem;
    bn_zero(&rem);

    int abits = bn_bit_length(a);
    for (int i = abits - 1; i >= 0; i--) {
        /* rem <<= 1 */
        bn_lshift1(&rem);
        /* OR in the next bit of a */
        if (bn_bit_at(a, i)) {
            rem.v[0] |= 1u;
            if (rem.n == 0) rem.n = 1;
        }
        /* if rem >= m, rem -= m */
        if (bn_cmp(&rem, m) >= 0) {
            bn_sub(&rem, &rem, m);
        }
    }
    bn_copy(r, &rem);
    bn_clear_high(r);
}

void bn_modmul(bignum *r, const bignum *a, const bignum *b, const bignum *m) {
    bignum prod;
    bn_mul(&prod, a, b);
    bn_mod(r, &prod, m);
}

/* Right-to-left binary modpow. For each bit of exp from LSB up:
 *   if bit set:  acc = acc * base mod m
 *                base = base^2 mod m
 *
 * For an n-bit exponent this is n squarings plus avg n/2 multiplies
 * — ~1.5n modmuls. RSA verify with e=65537 is just 17 of these
 * (instant). Sign with a 2048-bit d is ~3000 — the slow path. */
void bn_modpow(bignum *r, const bignum *base, const bignum *exp, const bignum *m) {
    bignum acc, b, tmp;
    bn_set_u32(&acc, 1);
    bn_mod(&b, base, m);
    int ebits = bn_bit_length(exp);
    for (int i = 0; i < ebits; i++) {
        if (bn_bit_at(exp, i)) {
            bn_modmul(&tmp, &acc, &b, m);
            bn_copy(&acc, &tmp);
        }
        if (i + 1 < ebits) {       /* skip the last unused square */
            bn_modmul(&tmp, &b, &b, m);
            bn_copy(&b, &tmp);
        }
    }
    bn_copy(r, &acc);
}

/* ---------------------------------------------------------------- */
/* Number-theoretic helpers                                         */
/* ---------------------------------------------------------------- */

/* Binary GCD — handles odd/even cases by shifting rather than dividing,
 * so it's fast without needing bn_div. */
void bn_gcd(bignum *r, const bignum *a, const bignum *b) {
    bignum x, y;
    bn_copy(&x, a);
    bn_copy(&y, b);

    if (bn_is_zero(&x)) { bn_copy(r, &y); return; }
    if (bn_is_zero(&y)) { bn_copy(r, &x); return; }

    /* Extract common factors of 2. */
    int shift = 0;
    while (!bn_is_odd(&x) && !bn_is_odd(&y)) {
        bn_rshift1(&x);
        bn_rshift1(&y);
        shift++;
    }
    /* Now at least one is odd. Drop pure-2 factors from the other. */
    while (!bn_is_odd(&x)) bn_rshift1(&x);
    while (!bn_is_odd(&y)) bn_rshift1(&y);

    /* Both odd here — Stein's reduction. */
    while (bn_cmp(&x, &y) != 0) {
        if (bn_cmp(&x, &y) > 0) {
            bn_sub(&x, &x, &y);
            while (!bn_is_odd(&x)) bn_rshift1(&x);
        } else {
            bn_sub(&y, &y, &x);
            while (!bn_is_odd(&y)) bn_rshift1(&y);
        }
    }
    bn_copy(r, &x);
    /* Multiply back the common factors of 2. */
    for (int i = 0; i < shift; i++) bn_lshift1(r);
}

/* Extended Euclidean — computes (g, s, t) such that a*s + m*t = g.
 * If g == 1, s mod m is the modular inverse of a mod m.
 *
 * We work with signed bignums by tracking a `neg` flag alongside the
 * magnitude — at the end we add m if s is negative to normalize into
 * [0, m). */
static void bn_signed_add(bignum *r, int *r_neg,
                          const bignum *a, int a_neg,
                          const bignum *b, int b_neg) {
    if (a_neg == b_neg) {
        bn_add(r, a, b);
        *r_neg = a_neg;
        return;
    }
    /* Opposite signs — subtract smaller from larger. */
    int c = bn_cmp(a, b);
    if (c == 0) { bn_zero(r); *r_neg = 0; return; }
    if (c > 0) {
        bn_sub(r, a, b);
        *r_neg = a_neg;
    } else {
        bn_sub(r, b, a);
        *r_neg = b_neg;
    }
}

int bn_modinv(bignum *r, const bignum *a, const bignum *m) {
    /* old_r, r_ work down to (gcd, 0). old_s, s_ track the coefficients
     * — old_s ends as a's inverse modulo m, if it exists. */
    bignum old_r, rr;
    bignum old_s, s_;
    int old_s_neg = 0, s_neg = 0;
    bn_copy(&old_r, a);
    bn_copy(&rr,    m);
    bn_set_u32(&old_s, 1);
    bn_set_u32(&s_,    0);

    bignum q, prod, tmp, new_r, new_s;
    int new_s_neg;

    while (!bn_is_zero(&rr)) {
        /* q = old_r / rr  (we need both q and remainder) */
        bignum quot, rem;
        bn_zero(&quot);
        bn_zero(&rem);
        int abits = bn_bit_length(&old_r);
        for (int i = abits - 1; i >= 0; i--) {
            bn_lshift1(&rem);
            if (bn_bit_at(&old_r, i)) { rem.v[0] |= 1u; if (rem.n == 0) rem.n = 1; }
            if (bn_cmp(&rem, &rr) >= 0) {
                bn_sub(&rem, &rem, &rr);
                /* set bit i of quot */
                int qlimb = i >> 5;
                int qshift = i & 31;
                quot.v[qlimb] |= 1u << qshift;
                if (quot.n <= qlimb) quot.n = qlimb + 1;
            }
        }
        bn_copy(&q, &quot);

        /* new_r = old_r - q * rr */
        bn_mul(&prod, &q, &rr);
        bn_sub(&new_r, &old_r, &prod);

        /* new_s = old_s - q * s_  (with sign tracking) */
        bn_mul(&prod, &q, &s_);
        /* (q * s_) has sign s_neg. We're computing old_s - (q*s_). */
        bn_signed_add(&tmp, &new_s_neg,
                      &prod, !s_neg,        /* negate */
                      &old_s, old_s_neg);
        bn_copy(&new_s, &tmp);

        bn_copy(&old_r, &rr);
        bn_copy(&rr,    &new_r);
        bn_copy(&old_s, &s_);
        bn_copy(&s_,    &new_s);
        old_s_neg = s_neg;
        s_neg     = new_s_neg;
    }

    /* old_r is gcd(a, m). If it's not 1 there's no modular inverse. */
    bignum one;
    bn_set_u32(&one, 1);
    if (bn_cmp(&old_r, &one) != 0) return -1;

    /* Normalize old_s into [0, m). */
    if (old_s_neg) {
        bignum tmp2;
        bn_mod(&tmp2, &old_s, m);
        bn_sub(r, m, &tmp2);
    } else {
        bn_mod(r, &old_s, m);
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Miller-Rabin primality test                                      */
/* ---------------------------------------------------------------- */

/* Skip obvious composites by trial-dividing through small primes.
 * Most random candidates fall here without ever reaching the modpow. */
static int small_prime_filter(const bignum *n) {
    static const uint32_t small[] = {
         3,  5,  7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53,
        59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113,
        127, 131, 137, 139, 149, 151, 157, 163, 167, 173, 179, 181,
        191, 193, 197, 199, 211, 223, 227, 229, 233, 239, 241, 251,
    };
    bignum r, small_bn;
    for (int i = 0; i < (int)(sizeof(small) / sizeof(small[0])); i++) {
        bn_set_u32(&small_bn, small[i]);
        if (bn_cmp(n, &small_bn) == 0) return 1;     /* n == p, prime */
        bn_mod(&r, n, &small_bn);
        if (bn_is_zero(&r)) return 0;                /* p divides n */
    }
    return -1;                                       /* inconclusive */
}

int bn_is_prime(const bignum *n, int rounds) {
    bignum two, three;
    bn_set_u32(&two, 2);
    bn_set_u32(&three, 3);

    if (bn_cmp(n, &two)   < 0) return 0;
    if (bn_cmp(n, &two)  == 0) return 1;
    if (bn_cmp(n, &three)== 0) return 1;
    if (!bn_is_odd(n))         return 0;

    int filt = small_prime_filter(n);
    if (filt >= 0) return filt;

    /* Write n-1 = d * 2^s with d odd. */
    bignum one, n1, d;
    bn_set_u32(&one, 1);
    bn_sub(&n1, n, &one);
    bn_copy(&d, &n1);
    int s = 0;
    while (!bn_is_odd(&d)) { bn_rshift1(&d); s++; }

    for (int round = 0; round < rounds; round++) {
        /* a in [2, n-2] */
        bignum a;
        bn_rand_bits(&a, bn_bit_length(n) - 1);
        if (bn_is_zero(&a)) bn_set_u32(&a, 2);
        if (bn_cmp(&a, &two) < 0) bn_set_u32(&a, 2);
        if (bn_cmp(&a, &n1)  >= 0) bn_set_u32(&a, 2);

        /* x = a^d mod n */
        bignum x;
        bn_modpow(&x, &a, &d, n);

        if (bn_cmp(&x, &one) == 0 || bn_cmp(&x, &n1) == 0) continue;

        int composite = 1;
        for (int j = 0; j < s - 1; j++) {
            bignum tmp;
            bn_modmul(&tmp, &x, &x, n);
            bn_copy(&x, &tmp);
            if (bn_cmp(&x, &n1) == 0) { composite = 0; break; }
        }
        if (composite) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* Random                                                           */
/* ---------------------------------------------------------------- */

void bn_rand_bits(bignum *b, int bits) {
    int bytes = (bits + 7) / 8;
    uint8_t buf[BN_MAX_LIMBS * 4];
    rand_bytes(buf, bytes);
    bn_from_bytes_be(b, buf, bytes);

    /* Trim to exactly `bits` by masking off the high bits of the
     * top byte. */
    int top_bit = bits - 1;
    int top_limb = top_bit >> 5;
    int top_shift = top_bit & 31;
    /* zero everything above top_bit */
    if (top_limb < BN_MAX_LIMBS) {
        uint32_t mask = (top_shift == 31) ? 0xFFFFFFFFu
                                          : ((1u << (top_shift + 1)) - 1);
        b->v[top_limb] &= mask;
        for (int i = top_limb + 1; i < BN_MAX_LIMBS; i++) b->v[i] = 0;
    }
    /* Force MSB so we get exactly `bits` bits. */
    if (top_limb < BN_MAX_LIMBS) {
        b->v[top_limb] |= (1u << top_shift);
    }
    /* Force LSB so the result is odd (useful for prime candidates). */
    if (b->n == 0 && bits > 0) b->n = 1;
    b->v[0] |= 1u;
    b->n = top_limb + 1;
    bn_trim(b);
}
