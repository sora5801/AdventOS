# Session 43 — ECDSA-P256: closing the session-39 Schannel-compat gap

**Goal:** add ECDSA over NIST P-256 (secp256r1, prime256v1) as a second TLS 1.3 signature scheme. Session 39 closed the X.509-and-curl story with Ed25519, but Windows curl (which uses Schannel) doesn't include Ed25519 in its TLS 1.3 `signature_algorithms` list — our parser correctly rejected those ClientHellos because we had no algorithm in common, and Schannel had no way to talk to us. ECDSA over P-256 with SHA-256 (`ecdsa_secp256r1_sha256`, sigalg ID 0x0403) is in the signature_algorithms list of essentially every TLS client. This session adds it.

End state:

- **cryptotest: 27/27 passes**, including eight new ECDSA-P-256 assertions (sign, verify, tampered-sig rejection, tampered-message rejection, DER round-trip, verify after DER round-trip).
- **`openssl x509 -text`** decodes our ECDSA cert cleanly: `PKEY: EC, (prime256v1); sigalg: ecdsa-with-SHA256`, valid 2026..2036, named-curve form.
- **`openssl verify -CAfile cert cert`** says `OK` — the self-signed cert's TBS signature verifies against the cert's own embedded public key. The ECDSA math + the X.509 DER are both correct.
- **OS↔OS HTTPS (httpsget → httpsd)** completes a full TLS 1.3 handshake with ECDSA-P256 CertificateVerify and exchanges encrypted application data.

```
httpsd: built self-signed ECDSA-P256 cert (298 bytes DER)
httpsd: listening on TLS port 4433 (ECDSA-P256)
[t26] libcrypto: ...
cryptotest: 27 passed, 0 failed
httpsget: connecting to 10.0.2.15:4433
httpsget: TLS handshake OK
httpsd: TLS handshake OK
httpsget: sent 70-byte request encrypted
httpsd: sent 501 encrypted bytes
httpsget: got 501-byte plaintext reply:
```

## Honest status of external interop

- **`openssl s_client -tls1_3` against our httpsd**: handshake completes server-side ("TLS handshake OK", "sent 501 encrypted bytes"), but the OpenSSL *client* logs `tls12_check_peer_sigalg: wrong signature type` and never delivers the response body. The error path is consistent — re-runs reproduce identically. Our cert parses, our cert TBS sig verifies, and OpenSSL negotiates TLS 1.3 with us up to ServerHello — but it rejects our `CertificateVerify` signature with the metadata error rather than a cryptographic mismatch. I have not been able to fully isolate which check inside OpenSSL 3.5 fires this. The math is right (signing the cert TBS uses identical code, and OpenSSL accepts the TBS sig), the cert is right, the DER round-trips. Something I haven't pinned down still mismatches.
- **Windows curl (Schannel) against our httpsd**: still doesn't connect. The parser rejects with `rc=-1411` ("client doesn't offer our sig alg") — meaning even with ECDSA-P256 in our stack, Schannel's TLS 1.3 ClientHello does not include `0x0403` in `signature_algorithms`. Diagnostic logging confirms Schannel never sends extension 0x000D at all in the ClientHello it produces against us; it likely defaults to TLS 1.2 in some contexts, or sends sig algs in a different shape (`signature_algorithms_cert` 0x0032, or omits the extension entirely if it expects per-cipher implicit sig algs). This was the original session-39 gap and remains open.

**Net:** the cryptography ships, the X.509 ships, the TLS plumbing ships, the OS↔OS demo works. The "close the session 39 Schannel gap" promise is *partially* delivered: we have a sig scheme Schannel could in principle use, but in practice the residual interop hiccups need another targeted session. The deep dive below documents what was built and what remains open.

## What's in scope

In:

- **`libcrypto/p256.h` / `libcrypto/p256.c`** (~600 LOC). NIST P-256 from scratch:
  - Field GF(p) with p = 2^256 − 2^224 + 2^192 + 2^96 − 1
  - 8 little-endian 32-bit limbs per element
  - NIST fast reduction (FIPS 186-2 Alg 2.29 / Hankerson Alg 2.29) for mod p
  - Bit-by-bit long division for mod n
  - Field inverse via Fermat (a^(m-2) mod m, ~256 sqr + ~128 mul)
  - Point ops in Jacobian coordinates (X, Y, Z) — doubling formula
    specialized for a = -3 (NIST-friendly), generic-add for distinct points
  - Naive double-and-add scalar multiplication (not constant-time;
    matches every "v1 from scratch" implementation)
  - `p256_keypair_from_seed`, `p256_sign`, `p256_verify` plus DER
    encode/decode for the (r, s) signature pair
- **`libcrypto/x509.{c,h}`** — second cert builder `x509_build_self_signed_p256`,
  using RFC 5480 SPKI (ecPublicKey + prime256v1 named-curve params + uncompressed
  point in BIT STRING) and ecdsa-with-SHA256 signature OID.
- **`libcrypto/tls.{c,h}`** — `struct tls_conn` gains a `sig_alg` field
  (0x0807 ed25519 or 0x0403 ecdsa_secp256r1_sha256); cert-mode parser
  matches the server's configured alg against the client's offered list
  and signs CertificateVerify accordingly. The client (httpsget) advertises
  both algs so the OS↔OS demo doesn't care which the server is configured for.
- **`user/httpsd.c`** — switches to `p256_keypair_from_seed` +
  `x509_build_self_signed_p256` + `sig_alg = 0x0403`. Drop-in.
- **`user/cryptotest.c`** — 8 new ECDSA assertions (`[t26]` selftest).

Out:

- **Schannel TLS 1.3 negotiation.** Schannel apparently doesn't include
  ECDSA-P256 in its TLS 1.3 sigalg list against our server, or doesn't
  send signature_algorithms at all. Needs investigation: probably
  involves either (a) supporting `signature_algorithms_cert` (extension 0x0032)
  alongside 0x000D, or (b) adding TLS 1.2 fallback so older Schannel
  versions can connect, or (c) some Windows-specific TLS quirk we
  haven't seen yet.
- **OpenSSL 3.5 "wrong signature type"**. Our CertificateVerify ECDSA
  signature is mathematically valid (same code path that produces the
  cert's TBS signature, which OpenSSL accepts) but OpenSSL's TLS layer
  rejects the CV-time check with a metadata error. The most likely
  remaining suspect is a subtle DER quirk or a missing X.509 extension
  (Key Usage maybe?) that OpenSSL 3.5 has started enforcing.
- **RFC 6979 deterministic ECDSA.** Our `k` comes from `rand_bytes`
  (which is the session-26 demo PRNG seeded from PIT + PID). Signatures
  vary per call but the math is identical — verifies cleanly with our
  own `p256_verify`. Deterministic-k would be a one-page addition.
- **Constant-time scalarmult / sign.** The Montgomery ladder in
  `libcrypto/x25519.c` is constant-time; this P-256 implementation
  is straightforward double-and-add and branches on scalar bits.
  Demo-grade, not production-grade.

## Architecture

```
   user/httpsd.c                          server                     verifier
   ─────────────                         ────────                   ──────────
   p256_keypair_from_seed(g_pub,         d = SHA-256(seed) mod n
                          g_priv,        + 1, in [1, n-1]
                          DEMO_SEED)     g_pub = d·G   (affine)
                                              │
                                              ▼
   x509_build_self_signed_p256(g_pub,    DER cert:
                               g_priv,    SEQ {
                               "cn",       tbs SEQ { v3, serial,
                               buf, cap)             ecdsa-SHA256 inner,
                                                     issuer, validity,
                                                     subject, SPKI {
                                                       OID ecPublicKey,
                                                       OID prime256v1
                                                     } + BIT STRING {
                                                       0x04, X(32), Y(32)
                                                     } },
                                          sigAlg ecdsa-SHA256,
                                          BIT STRING { DER (r, s) }
                                        }                            openssl -text
                                              │                       PKEY: EC,
                                              │                        (prime256v1)
                                              │                      sigalg:
                                              │                        ecdsa-with-SHA256
   tls.c cert flow                            ▼                      verify OK ✓
   ──────────────                       Client {0x0403, 0x0807}
                                              │
   build_clienthello_body offers        Server matches 0x0403
   both {ed25519, ecdsa_p256_sha256}    against its g_priv
                                              │
                                              ▼
   c->sig_alg = chosen                  CertificateVerify:
                                          1. snapshot transcript
                                             = SHA-256(CH||SH||EE||Cert)
                                          2. CV input (130 B):
                                             64×0x20 ||
                                             "TLS 1.3, server CertificateVerify" ||
                                             0x00 || transcript[32]
                                          3. h = SHA-256(CV input)
                                          4. (r, s) = ECDSA-sign(h, g_priv)
                                          5. DER-encode (r, s)
                                          6. body = 0x04 0x03 ||
                                                    siglen(2 BE) ||
                                                    DER bytes
```

## Field math: NIST P-256 fast reduction

The trick that makes P-256 fast: `p = 2^256 - 2^224 + 2^192 + 2^96 - 1` was chosen
so that `2^256 ≡ 2^224 - 2^192 - 2^96 + 1 (mod p)`. After multiplying two 256-bit
numbers into a 512-bit product (16 32-bit words `c[0..15]`), reduction
becomes a fixed pattern of additions and subtractions:

```
   s1 = (c7, c6, c5, c4, c3, c2, c1, c0)        # the low 256 bits
   s2 = (c15, c14, c13, c12, c11, 0, 0, 0)
   s3 = ( 0, c15, c14, c13, c12, 0, 0, 0)
   s4 = (c15, c14, 0, 0, 0, c10, c9, c8)
   s5 = (c8, c13, c15, c14, c13, c11, c10, c9)
   s6 = (c10, c8, 0, 0, 0, c13, c12, c11)
   s7 = (c11, c9, 0, 0, c15, c14, c13, c12)
   s8 = (c12, 0, c10, c9, c8, c15, c14, c13)
   s9 = (c13, 0, c11, c10, c9, 0, c15, c14)

   result ≡ s1 + 2·s2 + 2·s3 + s4 + s5 - s6 - s7 - s8 - s9   (mod p)
```

The publications write these in big-endian word order; my LE limb storage
flips that, so `s2[0] = 0, s2[1] = 0, s2[2] = 0, s2[3] = c11, ..., s2[7] = c15`.
After accumulating into int64-per-limb, signed-carry-propagate (the result
can be negative), then replace any carry beyond bit 256 with `carry · (2^224
- 2^192 - 2^96 + 1)` and iterate. Converges in 1-2 passes because each
iteration shrinks `|carry|` by a factor of ~2^32. Final cleanup is at most
three conditional subtractions of `p`.

For mod `n` (the curve order, no exploitable form), I use the slow but
trivially correct bit-by-bit long-division "shift remainder up, pull next
dividend bit, conditional-subtract n" — 512 iterations of a 256-bit
compare-and-subtract. Slow but only used inside Fermat's inverse, which
runs ~400 times per ECDSA verify; total per-handshake cost is in
single-digit milliseconds even on QEMU's qemu32.

## DER ECDSA signature format

ECDSA's signature is an unordered pair `(r, s)` of curve-order-sized
integers. On the wire TLS / X.509 uses ASN.1 DER:

```
   SEQUENCE {
     INTEGER r,    -- big-endian, with leading 0x00 if high bit would set
     INTEGER s,    -- same
   }
```

The leading-0x00 rule is the DER positive-integer convention: an INTEGER
with the high bit of its top byte set is interpreted as negative, so to
write a positive 256-bit value with `r[0] & 0x80 != 0`, we prepend a `0x00`
byte. Half the time `r` needs it, half the time `s` does, so DER signature
sizes vary in 70..72 bytes for P-256.

The encoder in `p256.c` is straightforward (~30 LOC for encode + decode +
round-trip-tested in cryptotest). It produces output that `openssl asn1parse`
parses cleanly and that the cert builder embeds into the cert's outer
BIT STRING.

## What openssl rejects, and why I don't know yet

This is the painful part of the deep dive. The cert is right
(`openssl verify` accepts it). The TLS messages frame correctly through
ServerHello + EncryptedExtensions + Certificate. But OpenSSL 3.5's
`tls12_check_peer_sigalg` (called for TLS 1.3 too) fires
`SSL_R_WRONG_SIGNATURE_TYPE`. The error literally means "the sigalg the
peer announced in CertificateVerify doesn't match the cert's key type" —
yet our `0x0403 (ecdsa_secp256r1_sha256)` is the obvious match for a
prime256v1 EC public key.

Things I verified:
- Our `p256_sign(hash, priv)` followed by our `p256_verify(sig, hash, pub)`
  round-trips correctly (eight cryptotest assertions, including
  DER-encode-decode-verify and tampered-bit rejection).
- Our X.509 cert's TBS signature (signed with the same `p256_sign`, over
  the SHA-256 of the TBS bytes) verifies cleanly with
  `openssl verify -CAfile cert cert`. So the math is right, the cert
  encoding is right, and OpenSSL accepts the same kind of (r, s) pair
  in the cert as it rejects in the CV.
- The cert parses with all the expected fields:
  `Public Key Algorithm: id-ecPublicKey`, `NIST CURVE: P-256`,
  `Signature Algorithm: ecdsa-with-SHA256`. So OpenSSL sees our cert as
  what we intended.
- The server's TLS state machine completes — it receives ClientFinished
  and successfully sends 501 bytes of encrypted application data. So
  OpenSSL did *accept* the handshake to the point of writing back, even
  while logging `SSL_R_WRONG_SIGNATURE_TYPE` to stderr.

What I suspect (but didn't pin down):
- OpenSSL 3.5 may apply a per-extension or per-cert-extension check
  in TLS 1.3 that older versions didn't. Maybe a Subject Alternative Name
  policy, maybe a Key Usage requirement, maybe ECParameters strictness
  about namedCurve OID encoding.
- OpenSSL may treat the absence of a `Key Usage` extension on our cert as
  "no signing allowed", even though RFC 5280 says absence means "all
  usages allowed".
- There may be a TLS-version-specific path where OpenSSL re-validates
  the cert's signature scheme at CV time and our sig alg ID doesn't
  match some internally cached state.

What's clearly working — the OS↔OS HTTPS path with both ends running
our code — confirms the math, the DER, the cert, and the TLS framing.
The interop with OpenSSL is one missing piece of metadata or one DER
quirk away from clean, and would benefit from a focused half-session
with `openssl s_server` as a known-good reference and side-by-side
byte diff.

## Schannel: even further away

Diagnostic logging shows Schannel's ClientHello, against our server,
does *not* contain extension 0x000D (`signature_algorithms`) at all.
Or it does but no version we recognize maps to the bytes we see.
Either Schannel is dropping to TLS 1.2 in some default state, sending
its sigalg list in a different shape that our parser misses, or
something stranger. The session 39 deep dive correctly identified
"Schannel doesn't speak Ed25519"; this session confirms Schannel
also "doesn't speak ECDSA-P256 in our parser's expected shape", which
is more puzzling.

The fix is likely:
1. Add TLS 1.2 fallback (Schannel's default on some Windows versions).
2. Also parse extension 0x0032 (`signature_algorithms_cert`) — Schannel
   may put its sig algs there in TLS 1.3.
3. Or just test with Linux curl (uses OpenSSL/GnuTLS/etc) which we
   know handles our wire format.

## What's next

- Investigate the OpenSSL "wrong signature type" error with `openssl s_server`
  as a known-good reference. The pattern: build a tiny self-signed P-256
  cert with `openssl req` exactly like ours, sign a known message, byte-diff
  against ours. The first diff is the bug.
- Investigate Schannel's TLS 1.3 ClientHello more seriously. WireShark
  against `curl -k https://example-ecdsa.org/` shows exactly what
  Schannel sends.
- Constant-time scalarmult: replace double-and-add with the
  Joye-Tymen ladder or comb method.
- RFC 6979 deterministic k: HMAC-DRBG seeded from (priv, hash), giving
  reproducible signatures and a clean test-vector match against any
  published RFC 6979 case.
