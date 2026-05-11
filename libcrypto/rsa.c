/*
 * AdventOS libcrypto — RSA-PKCS#1 v1.5 sign / verify / keygen (session 58).
 *
 * Builds on libcrypto/bignum.c.  See rsa.h for the API shape and
 * scope boundaries; this file is the implementation.
 */

#include "rsa.h"
#include "crypto.h"

/* DER-encoded DigestInfo prefix for SHA-256 (RFC 8017 §9.2 step 2,
 * table 1).  19 bytes of SEQUENCE { algorithm, OCTET STRING } header,
 * with the 32-byte hash slotted in after. */
static const uint8_t SHA256_DIGEST_INFO_PREFIX[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
    0x00, 0x04, 0x20,
};
#define SHA256_DIGEST_INFO_LEN  19
#define SHA256_HASH_LEN         32
#define T_LEN  (SHA256_DIGEST_INFO_LEN + SHA256_HASH_LEN)   /* 51 */

/* RFC 8017 §9.2 — produce the encoded message EM of length k bytes:
 *
 *   EM = 0x00 || 0x01 || PS || 0x00 || T
 *
 * where T is the DigestInfo + hash (51 bytes for SHA-256), and PS is
 * 0xFF repeated to fill the remaining space.  k must be at least
 * T_LEN + 11 (3 fixed bytes + 8 minimum PS).
 *
 * For RSA-2048 (k = 256), PS is 202 bytes long. */
static int em_encode(uint8_t *em, int k, const uint8_t *msg, size_t msg_len) {
    if (k < T_LEN + 11) return -1;

    uint8_t hash[SHA256_HASH_LEN];
    sha256(msg, msg_len, hash);

    int ps_len = k - T_LEN - 3;
    em[0] = 0x00;
    em[1] = 0x01;
    for (int i = 0; i < ps_len; i++) em[2 + i] = 0xFF;
    em[2 + ps_len] = 0x00;
    for (int i = 0; i < SHA256_DIGEST_INFO_LEN; i++) {
        em[3 + ps_len + i] = SHA256_DIGEST_INFO_PREFIX[i];
    }
    for (int i = 0; i < SHA256_HASH_LEN; i++) {
        em[3 + ps_len + SHA256_DIGEST_INFO_LEN + i] = hash[i];
    }
    return 0;
}

/* ---- Sign ------------------------------------------------------- */

/* CRT-accelerated signing per RFC 8017 §5.1.2 (when p/q populated).
 *
 *   m1   = m^dp mod p
 *   m2   = m^dq mod q
 *   h    = qinv * (m1 - m2) mod p
 *   sig  = m2 + h * q
 *
 * Each modpow is on numbers half the size of n, so two of them is
 * roughly half the work of one n-sized modpow — and even that's an
 * underestimate because mod-n's long division is wider too. */
static void rsa_priv_op_crt(bignum *out,
                            const bignum *m,
                            const struct rsa_keypair *kp) {
    bignum m1, m2, diff, h, prod;

    bn_modpow(&m1, m, &kp->dp, &kp->p);
    bn_modpow(&m2, m, &kp->dq, &kp->q);

    /* m1 - m2 — may underflow; if so, add p to bring it back into
     * [0, p) before the qinv multiply. */
    if (bn_cmp(&m1, &m2) >= 0) {
        bn_sub(&diff, &m1, &m2);
    } else {
        /* tmp = m2 - m1, then result = p - tmp */
        bignum tmp;
        bn_sub(&tmp, &m2, &m1);
        bn_sub(&diff, &kp->p, &tmp);
    }

    bn_modmul(&h, &kp->qinv, &diff, &kp->p);

    bn_mul(&prod, &h, &kp->q);
    bn_add(out, &m2, &prod);
}

/* Plain modpow fallback when CRT helpers aren't available. */
static void rsa_priv_op_d(bignum *out,
                          const bignum *m,
                          const struct rsa_keypair *kp) {
    bn_modpow(out, m, &kp->d, &kp->n);
}

int rsa_sign_pkcs1_sha256(uint8_t *sig, int sig_cap, int *sig_len_out,
                          const uint8_t *msg, size_t msg_len,
                          const struct rsa_keypair *kp) {
    int k = bn_byte_length(&kp->n);
    if (k <= 0 || k > sig_cap) return -1;

    uint8_t em[RSA_MAX_BYTES];
    if (em_encode(em, k, msg, msg_len) < 0) return -1;

    bignum m;
    bn_from_bytes_be(&m, em, k);

    bignum s;
    /* Use CRT if both primes are populated; otherwise fall back to
     * the straight d-modpow path. */
    if (kp->p.n > 0 && kp->q.n > 0) {
        rsa_priv_op_crt(&s, &m, kp);
    } else {
        rsa_priv_op_d(&s, &m, kp);
    }

    bn_to_bytes_be(&s, sig, k);
    *sig_len_out = k;
    return 0;
}

/* ---- Verify ----------------------------------------------------- */

int rsa_verify_pkcs1_sha256(const uint8_t *msg, size_t msg_len,
                            const uint8_t *sig, size_t sig_len,
                            const bignum *n, const bignum *e) {
    int k = bn_byte_length(n);
    if (k <= 0 || (int)sig_len != k) return -1;

    /* Decode the signature as an integer s in [0, n).  s ≥ n is
     * malformed per RFC 8017. */
    bignum s;
    bn_from_bytes_be(&s, sig, sig_len);
    if (bn_cmp(&s, n) >= 0) return -1;

    /* m = s^e mod n; emit MSB-first as k-byte EM' */
    bignum m;
    bn_modpow(&m, &s, e, n);

    uint8_t em_prime[RSA_MAX_BYTES];
    bn_to_bytes_be(&m, em_prime, k);

    /* Build the expected EM the same way the signer did. */
    uint8_t em[RSA_MAX_BYTES];
    if (em_encode(em, k, msg, msg_len) < 0) return -1;

    for (int i = 0; i < k; i++) {
        if (em[i] != em_prime[i]) return -1;
    }
    return 0;
}

/* ---- Key generation -------------------------------------------- */

/* Generate a probable prime of exactly `bits` bits (top + bottom bit
 * forced via bn_rand_bits).  Retries until Miller-Rabin gives the
 * green light. */
static void gen_prime(bignum *out, int bits) {
    for (;;) {
        bn_rand_bits(out, bits);
        if (bn_is_prime(out, 8)) return;
    }
}

int rsa_keygen(struct rsa_keypair *kp, int bits) {
    if (bits < 64 || bits > 4096 || (bits & 1)) return -1;
    int half = bits / 2;

    bignum one, p1, q1, phi, tmp;
    bn_set_u32(&one, 1);

    for (;;) {
        gen_prime(&kp->p, half);
        gen_prime(&kp->q, half);
        /* Insist p > q so the CRT inverse exists; swap if needed. */
        if (bn_cmp(&kp->p, &kp->q) == 0) continue;
        if (bn_cmp(&kp->p, &kp->q) < 0) {
            bignum t; bn_copy(&t, &kp->p); bn_copy(&kp->p, &kp->q); bn_copy(&kp->q, &t);
        }

        bn_mul(&kp->n, &kp->p, &kp->q);
        /* The product must actually be `bits` bits, not bits-1 — if both
         * primes happen to be near 2^(half-1), n falls short and
         * downstream code that hard-codes the byte length would mis-
         * frame.  Retry on undersized n. */
        if (bn_bit_length(&kp->n) < bits) continue;

        bn_sub(&p1, &kp->p, &one);
        bn_sub(&q1, &kp->q, &one);
        bn_mul(&phi, &p1, &q1);

        /* e = 65537. Coprime to phi requires phi mod e != 0, which
         * holds whenever e doesn't divide any of (p-1), (q-1); for
         * a random p, q this is essentially always true. */
        bn_set_u32(&kp->e, 65537);
        bn_mod(&tmp, &phi, &kp->e);
        if (bn_is_zero(&tmp)) continue;

        /* d = e^-1 mod phi.  Defined exactly when gcd(e, phi) == 1. */
        if (bn_modinv(&kp->d, &kp->e, &phi) < 0) continue;

        /* CRT helpers */
        bn_mod(&kp->dp, &kp->d, &p1);
        bn_mod(&kp->dq, &kp->d, &q1);
        if (bn_modinv(&kp->qinv, &kp->q, &kp->p) < 0) continue;

        return 0;
    }
}
