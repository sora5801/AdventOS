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

#endif
