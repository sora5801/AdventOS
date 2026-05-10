/*
 * Tiny X.509 v3 certificate builder for Ed25519 keys (RFC 8410).
 *
 * Goal: produce a self-signed certificate that curl will accept
 * (with -k for chain validation, -k is unrelated to signature
 * checking — curl still verifies the CertificateVerify on the
 * wire is a valid signature for the public key in the cert, and
 * that the cert's Ed25519 self-signature is well-formed DER).
 *
 * Scope: enough of ASN.1 DER to emit the few structures we need.
 * No general-purpose ASN.1 parser, no validity-time checking on
 * input, no EC curves besides Ed25519. ~150 lines vs the multi-
 * thousand-line beasts in OpenSSL.
 */
#ifndef ADVENTOS_X509_H
#define ADVENTOS_X509_H

#include "crypto.h"

/* Generous: a real Ed25519 self-signed cert is ~300 bytes; with
 * a slightly longer CN it's still well under 512. */
#define X509_MAX_CERT  512

/* Build a self-signed Ed25519 X.509 v3 cert.
 *
 *   pk      : 32-byte Ed25519 public key
 *   sk      : 64-byte Ed25519 expanded secret key (seed || pk)
 *   cn      : nul-terminated common name string (used for both
 *             subject and issuer — the cert is self-signed)
 *   out     : caller buffer; on success contains DER-encoded cert
 *             starting at out[0]
 *   out_cap : capacity of out (X509_MAX_CERT or more)
 *
 * Returns length of the DER cert in out[0..len), or -1 if the
 * buffer was too small or sign failed. */
int x509_build_self_signed_ed25519(
    const uint8_t pk[32],
    const uint8_t sk[64],
    const char *cn,
    uint8_t *out, int out_cap);

/* Build a self-signed ECDSA-P256 X.509 v3 cert (session 43, RFC 5480).
 *
 *   pub : 64-byte uncompressed public key, X(32 BE) || Y(32 BE)
 *         (no leading 0x04 — this function adds it for the
 *         X.509 BIT STRING)
 *   priv: 32-byte ECDSA private scalar
 *   cn  : subject + issuer common name
 *   out : caller buffer
 *
 * Algorithm OIDs:
 *   public key   1.2.840.10045.2.1   (ecPublicKey)
 *   curve params 1.2.840.10045.3.1.7 (prime256v1 / secp256r1)
 *   signature    1.2.840.10045.4.3.2 (ecdsa-with-SHA256)
 */
int x509_build_self_signed_p256(
    const uint8_t pub[64],
    const uint8_t priv[32],
    const char *cn,
    uint8_t *out, int out_cap);

#endif
