/*
 * AdventOS libcrypto — multi-precision integer arithmetic (session 58).
 *
 * Generic 32-bit-limb bignum, sized to comfortably hold 4096-bit values
 * so RSA-2048 modular exponentiation (whose intermediate products reach
 * 4096 bits before reduction) fits without reallocation. Pure C, no
 * assembly, no FPU — runs anywhere the kernel does.
 *
 * Representation:
 *   uint32_t v[BN_MAX_LIMBS]   limbs, LSB at v[0]
 *   int      n                 number of significant limbs (0 == zero)
 *
 * Invariants the helpers maintain:
 *   - v[n..] is zero on every public-API return (so bn_cmp can stop
 *     at max(a->n, b->n) and unread limbs are predictable)
 *   - n is the smallest count s.t. v[n-1] != 0; equivalently, leading
 *     zero limbs are trimmed
 *
 * What's NOT here:
 *   - constant-time anything. RSA secret-exponent operations leak via
 *     branch + timing. Acceptable for a hobby OS; do not deploy.
 *   - Montgomery / Barrett reduction. Mod is plain schoolbook long
 *     division. Slow but correct.
 *   - allocation. All bignums are stack-allocated fixed-size structs;
 *     callers control where they live.
 */
#ifndef ADVENTOS_LIBCRYPTO_BIGNUM_H
#define ADVENTOS_LIBCRYPTO_BIGNUM_H

#include "crypto.h"

/* 4096-bit max for the value itself, doubled headroom for products,
 * a couple of extra limbs for the long-division working buffer. */
#define BN_MAX_LIMBS  144

typedef struct {
    uint32_t v[BN_MAX_LIMBS];
    int      n;
} bignum;

/* ---- Init + conversion ----------------------------------------- */

void bn_zero        (bignum *b);
void bn_set_u32     (bignum *b, uint32_t value);
void bn_copy        (bignum *r, const bignum *a);

/* Big-endian byte strings — RFC 8017 / X.509 encoding. */
void bn_from_bytes_be(bignum *b, const uint8_t *buf, size_t n);
/* Writes exactly `n` bytes (left-pads with zeros if the value is shorter,
 * truncates the high bytes if longer — that's a bug, caller's job to
 * size right). */
void bn_to_bytes_be (const bignum *b, uint8_t *buf, size_t n);

/* Returns 0 for bn_zero; otherwise the index of the highest set bit + 1
 * (i.e. number of bits needed to represent the value). */
int  bn_bit_length  (const bignum *b);

/* Byte length needed to hold this bignum. */
int  bn_byte_length (const bignum *b);

/* ---- Comparison + queries -------------------------------------- */

int  bn_cmp     (const bignum *a, const bignum *b);   /* -1 / 0 / +1 */
int  bn_is_zero (const bignum *b);
int  bn_is_odd  (const bignum *b);
int  bn_bit_at  (const bignum *b, int bit);

/* ---- Addition / subtraction ------------------------------------ */

void bn_add     (bignum *r, const bignum *a, const bignum *b);
/* r = a - b. Returns 0 on success, -1 if a < b (r is then undefined). */
int  bn_sub     (bignum *r, const bignum *a, const bignum *b);

/* In-place shifts. */
void bn_lshift1 (bignum *b);
void bn_rshift1 (bignum *b);

/* ---- Multiplication + division --------------------------------- */

void bn_mul     (bignum *r, const bignum *a, const bignum *b);

/* r = a mod m. Plain long division (slow). */
void bn_mod     (bignum *r, const bignum *a, const bignum *m);

/* Convenience: r = (a * b) mod m. */
void bn_modmul  (bignum *r, const bignum *a, const bignum *b, const bignum *m);

/* Right-to-left binary modular exponentiation. r = base^exp mod m. */
void bn_modpow  (bignum *r, const bignum *base, const bignum *exp, const bignum *m);

/* ---- Number-theoretic helpers ---------------------------------- */

void bn_gcd     (bignum *r, const bignum *a, const bignum *b);

/* Modular inverse: r = a^-1 mod m. Returns 0 on success, -1 if gcd(a, m)
 * != 1. Extended Euclidean algorithm. */
int  bn_modinv  (bignum *r, const bignum *a, const bignum *m);

/* Miller-Rabin probabilistic primality test. `rounds` controls the
 * confidence: at 64 rounds the false-positive rate is < 2^-128, which
 * is well below the noise floor of "the rest of the crypto stack."
 * Returns 1 if probably prime, 0 if composite. */
int  bn_is_prime(const bignum *n, int rounds);

/* Fill `b` with `bits` random bits, with the MSB and LSB both forced
 * to 1 (so the result is exactly `bits` long and is odd — both
 * necessary preconditions for RSA prime candidates). Uses rand_bytes(). */
void bn_rand_bits(bignum *b, int bits);

#endif
