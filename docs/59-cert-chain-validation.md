# Session 59 — X.509 cert chain validation against a CA root store

**Goal:** close the long-standing "curl -k" gap. Before this session our TLS client (`httpsget` + `libcrypto/tls.c`) parsed the server's leaf cert just far enough to extract the public key for `CertificateVerify`, then accepted the cert blindly. A real-world TLS client must additionally check that the cert was signed by a CA it trusts.

What ships in session 59:

- **Full X.509 v3 parser** — `libcrypto/x509.c` now walks every field RFC 5280 names: `tbsCertificate` byte range, issuer DN, subject DN, validity dates (UTCTime + GeneralizedTime), public key info (Ed25519 / ECDSA-P256 / RSA), signature algorithm OID, signature value.
- **CA root store** — `struct ca_store` in-memory, plus `ca_store_load_from_etc_ssl` which scans `/etc/ssl/*.der` and parses each into a trust anchor.
- **`x509_verify_chain`** — given a leaf cert (and optional intermediates), walk the chain to a trust anchor, byte-comparing issuer/subject DNs and verifying each cert's signature with the appropriate algorithm.
- **TLS integration** — `struct tls_conn` gains a `ca_store *` field. Set it before `tls_client_handshake_cert`; the TLS Certificate-message handler calls `x509_verify_chain` and aborts the handshake with `rc=-115` if validation fails. Set it to NULL and the old "curl -k" behavior is preserved (still useful for `httpsget https://1.1.1.1/` against the real internet).
- **Test fixtures** — `/etc/ssl/test-ca.der` (a P-256 self-signed CA generated offline with openssl) and `/etc/ssl/server.der` (a P-256 leaf signed by that CA, with CN `10.0.2.15`), plus the corresponding 32-byte ECDSA scalar in `/etc/ssl/server.key` (mode 0600). httpsd now loads these by default; the synthesize-self-signed path remains as a fallback for testing and is exposed via `--self-signed`.
- **`[t42]` selftest** — 5/5 PASS. Positive case: connect to the CA-signed httpsd on 4433, chain validation accepts. Negative case: spawn a rogue httpsd on 4434 with `--self-signed`, chain validation rejects (`rc=-115`, "chain validation REJECTED server cert").

End-to-end selftest: **115 PASS, 0 FAIL, no regressions.**

---

## 1. The parser

### What we walk

```
Certificate ::= SEQUENCE {
    tbsCertificate          TBSCertificate,
    signatureAlgorithm      AlgorithmIdentifier,
    signature               BIT STRING
}
TBSCertificate ::= SEQUENCE {
    [0] EXPLICIT version DEFAULT v1,    -- skipped
    serialNumber                INTEGER,
    signature                   AlgorithmIdentifier,  -- redundant; we don't enforce
    issuer                      Name,                 -- captured as DN slice
    validity                    Validity,             -- parsed to epoch
    subject                     Name,                 -- captured as DN slice
    subjectPublicKeyInfo        SubjectPublicKeyInfo, -- extracted by alg
    -- IDs, extensions ...                            -- skipped
}
```

The output `struct x509_cert` (see `libcrypto/x509.h`) is a thin parsed view: most fields are slices that point back into the original DER buffer (`const uint8_t *bytes; int len;`), which keeps the parser zero-copy except for the few things we need in a different shape: the validity dates (epoch seconds) and the RSA modulus + exponent (`bignum` form for `rsa_verify_pkcs1_sha256`).

### Critical detail: the TBS slice

The signature in an X.509 cert covers the `tbsCertificate` **plus its outer SEQUENCE tag + length bytes** — i.e. everything from the byte that starts the tbsCertificate to the byte just before `signatureAlgorithm` begins. The parser snapshots that range:

```c
const uint8_t *tbs_start = p;
if (dr_read_tlv(&p, cert_end, &vl) != ASN1_SEQUENCE) return -1;
const uint8_t *tbs_inner_end = p + vl;
out->tbs     = tbs_start;
out->tbs_len = (int)(tbs_inner_end - tbs_start);
```

If you compute the TBS bytes wrong (e.g. skip the SEQUENCE header), the signature will hash a different byte range than the signer hashed and the verify will fail on every cert in the world. This is the #1 way roll-your-own X.509 parsers break — and the kind of bug that ONLY shows up against real-world CAs because our self-signed builder happens to be symmetric about it.

### Validity dates

X.509 carries dates as either `UTCTime` (13 chars, `"YYMMDDHHMMSSZ"`) for dates before 2050, or `GeneralizedTime` (15 chars, `"YYYYMMDDHHMMSSZ"`) for 2050+. RFC 5280 §4.1.2.5 mandates the boundary. The parser handles both — see `parse_x509_time` — converting through a small `civil_to_epoch` helper that does leap-year accounting from 1970.

Validity checking is gated on the `now` parameter passed to `x509_verify_chain`. Pass `now = 0` to disable date checks entirely — what the t42 client currently does because AdventOS doesn't have an NTP-synced clock yet. Once session 60 wires NTP in, that'll become a real check.

### DN slices

Subject and issuer Distinguished Names are captured as **whole-bytes slices** of the DER, not parsed into AVAs (attribute-value assertions). For chain validation that's enough: the relationship we're verifying is "leaf's issuer DN == root's subject DN", and the canonical form of both is the DER encoding the cert authors signed.

Byte-exact comparison via `dn_eq` would technically miss a "same logical name, different encoding" cert pair (e.g. one with PrintableString, one with UTF8String for the CN). Real CAs don't do that — the standard practice is for issuers to copy the issuer-DN bytes verbatim from the parent cert's subject — so we get away with byte compare. RFC 5280 §4.1.2.4 actually allows looser matching but doesn't require it.

---

## 2. Signature verification

```c
static int verify_cert_signature(const struct x509_cert *leaf,
                                  const struct x509_cert *issuer)
```

Hashes `leaf->tbs` with SHA-256 (we only ship SHA-256-family sig algs), then dispatches on `leaf->sig_alg`:

- `X509_SIG_ED25519` → `ed25519_verify(sig, tbs, tbs_len, issuer_pk)` (Ed25519 is "pure" — it hashes the message internally and is variable-length, so we pass the whole TBS, not just the SHA-256)
- `X509_SIG_ECDSA_SHA256` → `p256_sig_from_der(rs, sig, sig_len)` + `p256_verify(rs, sha256(tbs), issuer_pk + 1)` (strip the 0x04 uncompressed-point prefix the cert SPKI carries)
- `X509_SIG_RSA_SHA256` → `rsa_verify_pkcs1_sha256(tbs, tbs_len, sig, sig_len, &issuer_rsa_n, &issuer_rsa_e)` — straight call into the session-58 RSA implementation

The `if (issuer->pk_type != X509_PK_xxx)` guards reject cert chains that try to verify (e.g.) an ECDSA signature with an RSA pubkey — sounds obvious, but a sloppy verifier that just dispatches on `leaf->sig_alg` and ignores the issuer's actual key type would happily try to feed a 256-byte RSA-PKCS#1 sig to `p256_verify`.

---

## 3. CA store

```c
#define CA_STORE_MAX_CERTS  16

struct ca_store {
    struct x509_cert roots[CA_STORE_MAX_CERTS];
    int              n_roots;
    uint8_t          roots_storage[CA_STORE_MAX_CERTS * X509_MAX_CERT];
    int              storage_used;
};
```

A fixed-size in-memory store, no allocation. `ca_store_add(s, der, der_len)` copies the DER into the store's backing buffer (`roots_storage`), then parses against the in-store copy — that way the parsed `struct x509_cert`'s pointer slices stay valid for the lifetime of the store regardless of where the caller's input buffer came from.

`ca_store_load_from_etc_ssl(s)` walks `/etc/ssl/` via `sys_readdir`, opens each entry whose name ends in `.crt` or `.der`, reads up to `X509_MAX_CERT` bytes, and `ca_store_add`s the contents. Returns the count of roots successfully loaded.

We ship two `.der` files in the test fixture — `test-ca.der` (the actual root) and `server.der` (the leaf). The leaf being in the store too is harmless: it's never matched as an issuer (no other cert in the chain claims it as parent), and forging anything to chain through it requires the server's private key anyway. In a hypothetical multi-leaf deployment we'd put **only** root certs in `/etc/ssl/`.

DER vs PEM: the standard external format is PEM (base64-wrapped DER inside `-----BEGIN CERTIFICATE-----`). We ship raw DER. The win is no base64 decoder; the loss is a tiny bit of `openssl x509 -outform DER -in foo.pem -out foo.der` ceremony at fixture-generation time.

---

## 4. Chain walking

`x509_verify_chain(leaf_der, leaf_len, intermediates, inter_lens, n_inter, store, now)`:

1. Parse `leaf_der` into a `struct x509_cert`. Date-check if `now != 0`.
2. For each intermediate (`current` starts at the leaf):
   - Parse it. Date-check.
   - **DN match**: `current->issuer == intermediate->subject` (byte-exact).
   - **Signature**: `verify_cert_signature(current, intermediate)`.
   - Advance `current` to the intermediate.
3. After all intermediates, look for any root in the store whose **subject DN** matches `current->issuer`. If found, run `verify_cert_signature(current, root)`. On success: return 0.

The chain depth is `1 + n_intermediates + 1`. For the t42 test that's just `leaf → root` (no intermediates), which is the common case in self-managed PKIs and most internal corp environments. Real-world public CAs typically present `leaf → intermediate → root`; the parser handles up to 8 intermediates today.

**What we don't check yet:**

- Key Usage / Extended Key Usage extensions (a cert marked "code signing" used as a TLS server cert)
- Basic Constraints (`cA: TRUE` required on non-leaf nodes)
- Subject Alternative Name vs. the hostname being connected to
- Revocation (OCSP / CRL)
- Name constraints

Most of these matter for real-world security. The most important ones for a follow-up session: SAN/CN hostname matching against the URL we connected to, and Basic Constraints.

---

## 5. TLS plumbing

`struct tls_conn` gains:

```c
const struct ca_store *ca_store;
uint32_t               ca_store_now;     /* 0 = skip date checks */
```

Set them before `tls_client_handshake_cert`. The hook inside `tls.c`:

```c
if (mt == TLS_HS_CERTIFICATE) {
    /* ... existing pubkey-extract path ... */
    if (c->ca_store) {
        if (x509_verify_chain(leaf_der, leaf_len,
                              0, 0, 0,
                              c->ca_store,
                              c->ca_store_now) != 0) {
            return -115;
        }
    }
}
```

`rc = -115` is the unique error code for chain-validation rejection — the value is otherwise unused so callers can fold "this handshake failed because of an untrusted cert" out of the generic `-1..-N` error surface. httpsget specifically prints `"chain validation REJECTED server cert"` on that code, which is the negative-side witness that t42 greps.

---

## 6. The test fixture

Generated once, offline, with openssl. Reproducible:

```bash
# CA root: self-signed P-256
openssl ecparam -name prime256v1 -genkey -noout -out test-ca.key
openssl req -new -x509 -key test-ca.key -out test-ca.crt -days 3650 \
    -subj '/CN=AdventOS Test CA' -sha256

# Leaf: P-256 server cert signed by the CA above
openssl ecparam -name prime256v1 -genkey -noout -out server.key
openssl req -new -key server.key -out server.csr -subj '/CN=10.0.2.15'
openssl x509 -req -in server.csr -CA test-ca.crt -CAkey test-ca.key \
    -CAcreateserial -out server.crt -days 3650 -sha256

# Convert to the on-disk wire format AdventOS reads
openssl x509 -in test-ca.crt -outform DER -out test-ca.der
openssl x509 -in server.crt  -outform DER -out server.der
# Extract just the 32-byte raw private scalar (no PKCS#8 wrapping)
openssl ec -in server.key -text -noout | awk '/priv:/,/pub:/' | \
    grep ':' | grep -v 'priv\|pub' | tr -d ' :\n' | xxd -r -p > server.key
```

The three resulting files land in `fs/etc/ssl/` and ship via mkfs.py (3-tuple variant for the world-readable cert, 4-tuple variant with mode `0o600` for the private key).

mkfs.py also gained support for nested directories so `etc/ssl/` could exist (the entry `('ssl', 'etc')` reads as "make 'ssl' under 'etc'").

---

## 7. The selftest

```
[t42] X.509 cert chain validation against /etc/ssl/ CA store
  PASS  CA store loaded test-ca.der + server.der from /etc/ssl/
  PASS  client reports chain validation ON
  PASS  CA-signed cert ACCEPTED — handshake completed + GET succeeded
  PASS  rogue self-signed cert REJECTED — chain validation tripped
  PASS  handshake aborted + httpsget exited non-zero
```

The negative test is the most informative: an untouched session-37 setup (chain validation OFF) would have happily accepted the rogue cert and the handshake would complete. The fact that we observe `rc = -115` ("chain validation REJECTED") + non-zero exit code proves the new code path is genuinely doing work, not just sitting inert.

The choreography:

```
positive: fork (httpsget https://10.0.2.15:4433/) → pipe-capture
          expect: "loaded 2 CA root(s)", "chain validation ON",
                  "TLS 1.3 handshake OK", exit 0

negative: fork (httpsd --self-signed --port 4434)        ← rogue server
          sleep 150ms                                    ← let it bind
          fork (httpsget https://10.0.2.15:4434/) → pipe-capture
          expect: "chain validation REJECTED server",
                  "TLS handshake failed", exit non-zero
          kill rogue, reap
```

Both halves exercise the same `tls_client_handshake_cert` code path; the only difference is the cert the server presents.

---

## 8. Touched files

- `libcrypto/x509.{h,c}` — full parser (`x509_parse_cert`), `x509_extract_pubkey` (replaces the static one tls.c used to carry), CA store, `x509_verify_chain`.
- `libcrypto/tls.{h,c}` — `struct tls_conn` gains `ca_store` + `ca_store_now`. Client-side Certificate handler calls `x509_verify_chain` when store is non-NULL. Removed the now-unused local DER reader.
- `user/httpsd.c` — loads `/etc/ssl/server.der` + `/etc/ssl/server.key` by default; falls back to synthesized self-signed if either file is missing or `--self-signed` is passed. Accepts `--port N`.
- `user/httpsget.c` — loads CA roots from `/etc/ssl/` on startup; passes the store to the TLS layer. Reports `rc=-115` as `"chain validation REJECTED server cert"`.
- `user/sh.c` — `[t42]` selftest with positive + negative scenarios.
- `mkfs.py` — supports nested dirs via `('child', 'parent')` tuples, supports per-file mode via the 4-tuple data_files variant. Ships `/etc/ssl/{test-ca,server}.der` and `/etc/ssl/server.key`.
- `fs/etc/ssl/test-ca.der`, `fs/etc/ssl/server.der`, `fs/etc/ssl/server.key` — new fixtures.

## 9. Out of scope (deferred)

- **Hostname matching** (SAN / CN against the URL host). A cert that says `CN=10.0.2.15` is happily presented when the client connects to `10.0.2.99` — no current check rejects that. The most impactful follow-up.
- **Basic Constraints + path-length** — protect against an unrelated leaf being abused as an intermediate.
- **OCSP / CRL** — revocation. Big project (requires HTTP-fetch-during-handshake plumbing).
- **PEM (base64-wrapped) support** in `/etc/ssl/` — would let users drop standard `*.pem` files. Cosmetic.
- **Server-side client-cert verification** (mutual TLS). We don't request client certs.

Most realistic next session: hostname / SAN matching, since that's the security-critical piece a real-world TLS client *cannot* skip.
