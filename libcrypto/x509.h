/*
 * Tiny X.509 v3 — emitter + parser + chain validator (session 59).
 *
 * Original scope: emit self-signed certs for Ed25519 / ECDSA-P256
 * server keys so curl would accept them in TLS 1.3 (sessions 39, 43).
 *
 * Session 59 expansion: PARSE certs (a peer's serving Certificate
 * message), and verify a chain {leaf, ..., root} against a small
 * in-memory store of trusted CA certs. That closes the
 * "curl -k semantics" gap our old client had — we used to extract
 * the cert's pubkey, verify CertificateVerify against it, and
 * call that "done". A real client must ALSO verify that the
 * cert itself was signed by a CA we trust.
 *
 * What's deliberately limited:
 *   - No general-purpose ASN.1 parser. The cert structure walker
 *     handles the exact shape RFC 5280 describes, but exotic
 *     extensions / non-CN subject DNs / IDN encoding etc. are
 *     out of scope.
 *   - Three signature algos: ecdsa-with-SHA256, ed25519, sha256-
 *     with-RSA. Matches what libcrypto can verify.
 *   - Chain depth is small (CA_STORE_MAX_CERTS + intermediates).
 */
#ifndef ADVENTOS_X509_H
#define ADVENTOS_X509_H

#include "crypto.h"
#include "rsa.h"

/* Generous: a real Ed25519 self-signed cert is ~300 bytes; with
 * a slightly longer CN it's still well under 512. RSA certs with
 * a 2048-bit modulus run ~1100-1400 bytes; bump cap for those. */
#define X509_MAX_CERT  1600

/* ---- Emitter (sessions 39 + 43) -------------------------------- */

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

/* ---- Parser (session 59) ---------------------------------------- */

/* Public-key shape extracted from SubjectPublicKeyInfo. */
#define X509_PK_ED25519   1
#define X509_PK_P256      2
#define X509_PK_RSA       3

/* Signature algorithm — the one used to sign THIS cert (i.e. by the
 * issuer's private key). */
#define X509_SIG_ED25519       1     /* OID 1.3.101.112 */
#define X509_SIG_ECDSA_SHA256  2     /* OID 1.2.840.10045.4.3.2 */
#define X509_SIG_RSA_SHA256    3     /* OID 1.2.840.113549.1.1.11 */

/* DN slice — points back into the source DER buffer, NOT a copy.
 * The cert struct must out-live the source DER for these to be
 * valid. Byte-exact comparison via the bytes is enough for the
 * "issuer of leaf == subject of root" check; we don't normalize. */
struct x509_dn {
    const uint8_t *bytes;
    int            len;
};

/* Parsed cert. Most fields point back into the source DER.  The
 * RSA `n` / `e` fields are the only "owned" data because they need
 * the bignum form for verify; everything else is a slice. */
struct x509_cert {
    /* Whole-cert byte range, for chain-presented logging. */
    const uint8_t *der;
    int            der_len;

    /* TBSCertificate bytes — the signed body. The verifier hashes
     * this exact byte range to check the signature. RFC 5280 §4.1.2.
     * Includes the outer SEQUENCE tag + length, since that's what
     * the encoder hashed. */
    const uint8_t *tbs;
    int            tbs_len;

    /* Issuer / subject DN as raw DER (whole `Name` SEQUENCE bytes). */
    struct x509_dn issuer;
    struct x509_dn subject;

    /* Validity, parsed to Unix epoch seconds. UTCTime + GeneralizedTime
     * both supported. */
    uint32_t       not_before;
    uint32_t       not_after;

    /* Public key — see X509_PK_* constants for type discrimination. */
    int            pk_type;
    /* Ed25519: 32 bytes. P256: 65 bytes (0x04 || X || Y). RSA: see
     * rsa_n + rsa_e below. */
    uint8_t        pk_bytes[1024];
    int            pk_len;
    /* RSA-specific: pre-parsed modulus + exponent (used during chain
     * verify). Only populated when pk_type == X509_PK_RSA. */
    bignum         rsa_n;
    bignum         rsa_e;

    /* Signature algorithm + value. */
    int            sig_alg;
    const uint8_t *sig;
    int            sig_len;
};

/* Parse a DER-encoded cert.  `der` must remain live for the
 * lifetime of `*out` (which contains pointers into it).  Returns
 * 0 on success, -1 on any malformation we don't tolerate (most
 * commonly: wrong tag, length overflow, unsupported algorithm). */
int x509_parse_cert(const uint8_t *der, int der_len,
                    struct x509_cert *out);

/* Convenience for the existing TLS code: extract just the public
 * key + its TLS sig_alg code (0x0807 for ed25519, 0x0403 for p256,
 * 0x0401 for rsa-sha256). Same parser internally as x509_parse_cert. */
int x509_extract_pubkey(const uint8_t *cert, int cert_len,
                        int *out_tls_alg,
                        uint8_t *out_pk, int *out_pk_len);

/* ---- Chain validation (session 59) ----------------------------- */

#define CA_STORE_MAX_CERTS  16

struct ca_store {
    /* Parsed roots.  Each entry's cert.der points into the static
     * `roots_storage` member below, which holds the raw DER. */
    struct x509_cert roots[CA_STORE_MAX_CERTS];
    int              n_roots;
    /* Backing storage for the root DERs. Owns the bytes the roots[]
     * entries point at. */
    uint8_t          roots_storage[CA_STORE_MAX_CERTS * X509_MAX_CERT];
    int              storage_used;
};

/* Initialize an empty store.  Required before ca_store_add. */
void ca_store_init(struct ca_store *s);

/* Add a CA root cert (DER-encoded).  Returns 0 on success, -1 if
 * the store is full or the cert doesn't parse. */
int ca_store_add(struct ca_store *s, const uint8_t *der, int der_len);

/* Load CA roots from `/etc/ssl/<filename>` for every file in the
 * /etc/ssl directory whose name ends in ".der" or ".crt".  Returns
 * the count of roots loaded.  Files are DER (not PEM) — the TLS
 * ecosystem's most common format anyway, and avoids dragging in
 * a base64 decoder. */
int ca_store_load_from_etc_ssl(struct ca_store *s);

/* Verify a leaf cert + (optional) intermediates against the store.
 *
 *   leaf_der        : the server's leaf cert
 *   leaf_der_len
 *   intermediates   : array of intermediate cert DERs (may be NULL)
 *   n_intermediates : count (0 if direct leaf-to-root)
 *   store           : trust anchors
 *   now             : current Unix time (for validity-date checks);
 *                     pass 0 to disable date validation entirely
 *                     (useful in the early-boot path where RTC is
 *                     not yet synced).
 *
 * Returns 0 if the chain validates, -1 otherwise.  Reasons certs
 * are rejected: bad signature, expired, issuer not in store, DN
 * mismatch, unsupported algorithm.
 *
 * The chain walked is leaf → (intermediates in order) → root, where
 * the root must match by Subject DN against any root in the store. */
int x509_verify_chain(const uint8_t *leaf_der, int leaf_der_len,
                      const uint8_t *const *intermediates,
                      const int *intermediate_lens,
                      int n_intermediates,
                      const struct ca_store *store,
                      uint32_t now);

#endif
