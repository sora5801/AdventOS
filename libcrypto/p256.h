/*
 * ECDSA over NIST P-256 (secp256r1, prime256v1). Closes the
 * session-39 gap: with Ed25519 only, Windows curl (Schannel)
 * refused to negotiate TLS 1.3 because Schannel's signature_algorithms
 * list doesn't include ed25519. ECDSA-P256 is in every TLS client's
 * supported set, so adding it makes `curl -k https://...` work
 * from Windows too.
 *
 * On-the-wire formats:
 *   Public key   64 bytes — X(32 BE) || Y(32 BE), uncompressed point
 *                (preceded by 0x04 in X.509 SubjectPublicKey)
 *   Private key  32 bytes — scalar in big-endian
 *   Signature    64 bytes — R(32 BE) || S(32 BE) raw,
 *                or DER-encoded (variable length, max 72 bytes)
 *
 * Hash is always SHA-256 — TLS 1.3 sigalg 0x0403 is literally
 * "ecdsa with secp256r1 and SHA-256".
 */
#ifndef ADVENTOS_P256_H
#define ADVENTOS_P256_H

#include "crypto.h"

/* Derive a keypair deterministically from a 32-byte seed.
 *   priv = SHA-256(seed) mod (n-1) + 1     (so priv is in [1, n-1])
 *   pub  = priv * G   (compressed: 64 bytes uncompressed XY only,
 *                      no leading 0x04 here — caller adds that for DER)
 */
void p256_keypair_from_seed(uint8_t pub[64], uint8_t priv[32],
                            const uint8_t seed[32]);

/* ECDSA sign — `hash` is a 32-byte SHA-256 digest (already computed
 * by the caller, since ECDSA-with-SHA256 hashes the message first).
 * `sig` receives R || S, each 32 bytes big-endian.
 * Returns 0 on success. */
int p256_sign(uint8_t sig[64],
              const uint8_t hash[32], const uint8_t priv[32]);

/* ECDSA verify — same hash + sig format as sign().
 * Returns 0 on valid, -1 on invalid. */
int p256_verify(const uint8_t sig[64],
                const uint8_t hash[32], const uint8_t pub[64]);

/* Convert raw 64-byte (R||S) to ASN.1 DER:
 *     SEQUENCE { r INTEGER, s INTEGER }
 * Each INTEGER is variable-length, big-endian, with a leading 0x00
 * if the high bit would otherwise be set (DER positive-integer rule).
 * Returns the DER length (8..72), or -1 if `out_cap` is too small. */
int p256_sig_to_der(uint8_t *out, int out_cap, const uint8_t sig[64]);

/* Parse DER back into raw 64-byte (R||S). Returns 0 / -1. */
int p256_sig_from_der(uint8_t sig[64], const uint8_t *der, int der_len);

#endif
