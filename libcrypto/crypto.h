/*
 * AdventOS libcrypto — header for SHA-256, HMAC, HKDF, AES, GCM, X25519.
 *
 * Statically linked into user programs that need TLS (httpsd, httpsget).
 * Not loaded via the libc dyld layer — the API is large, only two
 * programs use it, and the keys/state need to be fork-private which
 * argues for static .data anyway.
 */
#ifndef ADVENTOS_LIBCRYPTO_H
#define ADVENTOS_LIBCRYPTO_H

#include "../user/libuser.h"   /* uint32_t, size_t, etc. */

/* libuser.h only defines uint16/32_t and size_t. Crypto code needs
 * uint8_t and uint64_t too — add them locally rather than touching
 * the libuser ABI. */
typedef unsigned char       uint8_t;
typedef unsigned long long  uint64_t;
typedef long long           int64_t;

/* ---- SHA-256 ----------------------------------------------------- */
struct sha256 {
    uint32_t state[8];
    uint64_t count;       /* bytes hashed so far */
    uint8_t  buf[64];
    uint32_t buf_len;
};
void sha256_init  (struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t n);
void sha256_final (struct sha256 *s, uint8_t out[32]);
/* One-shot. */
void sha256       (const void *data, size_t n, uint8_t out[32]);

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

/* ---- HMAC-SHA-256 ------------------------------------------------- */
void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len,
                 uint8_t out[32]);

/* ---- HKDF (RFC 5869) and HKDF-Expand-Label (TLS 1.3 §7.1) -------- */
void hkdf_extract(const uint8_t *salt, size_t salt_len,
                  const uint8_t *ikm,  size_t ikm_len,
                  uint8_t prk[32]);
void hkdf_expand (const uint8_t *prk,  size_t prk_len,
                  const uint8_t *info, size_t info_len,
                  uint8_t *okm, size_t okm_len);
/* TLS 1.3's HKDF-Expand-Label("label", context, length). label is
 * automatically prefixed with "tls13 " per RFC 8446 §7.1. */
void tls13_hkdf_expand_label(
    const uint8_t prk[32],
    const char *label,             /* nul-terminated */
    const uint8_t *context, size_t context_len,
    uint8_t *out, size_t out_len);
/* Derive-Secret(secret, label, msgs) = HKDF-Expand-Label(secret, label,
 * Hash(msgs), Hash.length). */
void tls13_derive_secret(
    const uint8_t prk[32],
    const char *label,
    const uint8_t *transcript_hash, size_t hash_len,
    uint8_t out[32]);

/* ---- AES-128 block cipher --------------------------------------- */
struct aes128 {
    uint32_t rk[44];      /* 11 round keys, 4 words each */
};
void aes128_set_key (struct aes128 *ctx, const uint8_t key[16]);
void aes128_encrypt (const struct aes128 *ctx, const uint8_t in[16], uint8_t out[16]);

/* ---- AES-128-GCM AEAD ------------------------------------------- */
/* Encrypt: writes ciphertext (same length as plaintext) and a 16-byte
 * tag. nonce must be 12 bytes. aad may be NULL with aad_len = 0. */
void aes_gcm_encrypt(const uint8_t key[16],
                     const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *pt, size_t pt_len,
                     uint8_t *ct, uint8_t tag[16]);
/* Decrypt: returns 0 on success (tag valid), -1 if tag mismatch.
 * Plaintext written even on tag mismatch, so caller must NOT use it. */
int  aes_gcm_decrypt(const uint8_t key[16],
                     const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *ct, size_t ct_len,
                     const uint8_t tag[16],
                     uint8_t *pt);

/* ---- X25519 (RFC 7748) ------------------------------------------- */
/* X25519 scalar mult: out = scalar * point, both 32 bytes. The
 * generator (basepoint) is passed as point = x25519_basepoint to
 * compute the public key from a secret scalar; for shared-secret
 * derivation, point is the peer's public key. */
extern const uint8_t x25519_basepoint[32];
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

/* ---- Random (best-effort PRNG) ----------------------------------- */
/* Seeded from sys_time + initial state on first call. Crypto-strength
 * is "fine for a demo" — not for real secrets. Documented in t26. */
void rand_bytes(uint8_t *out, size_t n);

/* ---- Ed25519 (RFC 8032) ----------------------------------------- */
/* Sign uses the standard "expanded secret key" format: 64 bytes
 * where the low 32 are the seed and the high 32 are the cached
 * public key. Generated from a 32-byte seed via _from_seed(). */
void ed25519_keypair_from_seed(uint8_t pk[32], uint8_t sk[64], const uint8_t seed[32]);
void ed25519_sign(uint8_t sig[64],
                  const uint8_t *msg, size_t msg_len,
                  const uint8_t sk[64]);
int  ed25519_verify(const uint8_t sig[64],
                    const uint8_t *msg, size_t msg_len,
                    const uint8_t pk[32]);

/* ---- ECDSA over NIST P-256 (secp256r1) ------------------------- */
/* Closes the session-39 Schannel gap by giving us a signature
 * scheme every TLS client supports. See libcrypto/p256.h for
 * details on the wire formats. */
void p256_keypair_from_seed(uint8_t pub[64], uint8_t priv[32], const uint8_t seed[32]);
int  p256_sign  (uint8_t sig[64], const uint8_t hash[32], const uint8_t priv[32]);
int  p256_verify(const uint8_t sig[64], const uint8_t hash[32], const uint8_t pub[64]);
int  p256_sig_to_der  (uint8_t *out, int out_cap, const uint8_t sig[64]);
int  p256_sig_from_der(uint8_t sig[64], const uint8_t *der, int der_len);

#endif
