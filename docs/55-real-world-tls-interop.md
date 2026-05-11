# Session 55 — Real-world TLS interop

**Goal:** close the TLS interop gap session 51 flagged. Make AdventOS's `httpsd` actually talk to real-world clients (OpenSSL `s_client`, curl/Schannel), and tighten the in-OS TLS client so it cryptographically validates the server's CertificateVerify — both sides of the wire.

End state — a clean handshake + body delivery to both real-world clients:

```
$ curl -k https://127.0.0.1:4523/
Hello from a TLS 1.3 + HTTPS server in AdventOS!
What just happened on the wire:
  - Real RFC 8446 ClientHello with extensions
  - X25519 ECDHE for forward secrecy
  - Self-signed ECDSA-P256 X.509 certificate (RFC 5480)
  - CertificateVerify ECDSA-with-SHA256 over the transcript
  ...

$ openssl s_client -connect 127.0.0.1:4523 -tls1_3 -quiet
Verify return code: 18 (self-signed certificate)
HTTP/1.0 200 OK
Content-Type: text/plain
Connection: close

Hello from a TLS 1.3 + HTTPS server in AdventOS!
```

Plus the new `[t37]` selftest, all three assertions PASS:

```
[t37] TLS interop: cert-flow round-trip with CertificateVerify validation
  captured 683 bytes from httpsget  (exit=0)
  PASS  httpsget reports TLS 1.3 handshake OK (sig_alg matched + CertificateVerify validated)
  PASS  HTTP/1.0 200 came through the encrypted record layer
  PASS  response body decrypted into expected greeting
```

And real-world client side — connecting from our `httpsget` to Cloudflare's `https://1.1.1.1/` — still returns its 301 redirect, but now with the server's CertificateVerify signature actually verified against the certificate's public key. Previously the client trusted any signature.

## The two bugs

### 1. Server clobbers `sig_alg` during handshake init

`tls_server_handshake_cert` saves `cert_der`, `cert_der_len`, `server_sk`, zeros the entire `tls_conn`, then restores those three. It does NOT save/restore `sig_alg`.

httpsd sets `t.sig_alg = 0x0403` (ecdsa_secp256r1_sha256) before calling the handshake. The zero-and-restore step erased it to 0. Then the parser:

```c
int want_sig = c->sig_alg ? c->sig_alg : 0x0807;   /* default ed25519 */
```

fell back to **Ed25519** (0x0807). Every OpenSSL client offers Ed25519 too, so the match succeeded, `c->sig_alg = 0x0807`. The CertificateVerify code then branched into the Ed25519 path:

```c
} else {
    /* Ed25519, the session-39 default. */
    uint8_t sig[64];
    ed25519_sign(sig, to_sign, 130, c->server_sk);
    ...
}
```

…calling `ed25519_sign` with `c->server_sk` — but `server_sk` was an **ECDSA-P256** 32-byte scalar, not a 64-byte expanded Ed25519 secret key. The result was 64 bytes of garbage labeled `Signature Algorithm: ed25519 (0x0807)` going out over the wire, attached to a cert whose subjectPublicKeyInfo said ECDSA.

OpenSSL spotted this immediately in `tls12_check_peer_sigalg` — the function checks that the CertificateVerify's sig scheme is compatible with the cert key type, and Ed25519-sig-over-P256-key fails that check. The error was the seemingly-mysterious:

```
error:0A000172:SSL routines:tls12_check_peer_sigalg:wrong signature type
```

The diagnosis required `openssl s_client -trace` output, which showed the actual sig scheme bytes on the wire and made the Ed25519-over-ECDSA mismatch visible. Fix is one line:

```c
const uint8_t *cert_der    = c->cert_der;
int            cert_len    = c->cert_der_len;
const uint8_t *sk          = c->server_sk;
int            saved_sig   = c->sig_alg;   // ← was missing

for (int i = 0; i < (int)sizeof(*c); i++) ((uint8_t *)c)[i] = 0;
c->fd = fd;
c->is_server = 1;
c->cert_der = cert_der;
c->cert_der_len = cert_len;
c->server_sk = sk;
c->sig_alg   = saved_sig;   // ← was missing
```

With the restore in place, the parser sees `want_sig = 0x0403`, matches that scheme in the client's offered list, and the CertificateVerify branches into the ECDSA path with the actual P-256 private key. OpenSSL accepts the signature.

### 2. Client never validated the server's CertificateVerify

Before this session, `tls_client_handshake_cert` extracted *nothing* from the server's Certificate message and *never called* `ed25519_verify` or `p256_verify` on the CertificateVerify body. The handshake "succeeded" purely on the basis of:

- ECDHE produced a shared secret
- Server's Finished verified (which is a MAC over the transcript, computed from the shared secret + handshake traffic secret)

The CertificateVerify was treated as opaque transcript material — its content was hashed into the running transcript but the bytes themselves were never proven to come from someone holding the cert's private key. This is a **complete missing proof-of-possession check**, the exact thing CertificateVerify exists to provide.

Symptoms: the in-OS `httpsget→httpsd` loopback "passed" even with bug #1 sending garbage signatures. Real OpenSSL caught it; our client didn't.

The fix is in two parts:

**(a) A tiny X.509 parser** — just enough DER walking to find the SubjectPublicKeyInfo's BIT STRING in our own emitted cert shapes (Ed25519 via RFC 8410 and ECDSA-P256 via RFC 5480). About 70 lines, runs on the first `Certificate` message processed:

```c
static int x509_extract_pubkey(const uint8_t *cert, int cert_len,
                                int *out_alg,
                                uint8_t *out_pk, int *out_pk_len) {
    /* outer SEQ → tbsCertificate SEQ → [version] → serial → sigAlg →
     * issuer → validity → subject → subjectPublicKeyInfo → algorithm →
     * OID → match against ed25519 / p256 → BIT STRING. */
    ...
}
```

Not a general X.509 parser — those are thousands of lines. Just enough to find the two cert shapes we emit, walk past the optional EXPLICIT-tagged version, and identify the algorithm from its OID bytes (`06 03 2B 65 70` for ed25519, `06 07 2A 86 48 CE 3D 02 01` for ECDSA P-256).

**(b) Run the matching verify routine** on receipt of CertificateVerify:

```c
if (mt == TLS_HS_CERT_VERIFY) {
    int sig_alg = ...;
    if (sig_alg != cert_alg) return -123;   /* alg/key mismatch */

    /* Recompute the 130-byte CV-context exactly as the server signed it. */
    uint8_t th[32];
    struct sha256 ts = transcript_at_cv_compute_point;
    sha256_final(&ts, th);
    uint8_t to_verify[130];
    build_cv_sign_input(to_verify, th);

    if (sig_alg == 0x0807) {
        if (ed25519_verify(sig, to_verify, 130, cert_pk) != 0) return -125;
    } else if (sig_alg == 0x0403) {
        uint8_t rs[64];
        if (p256_sig_from_der(rs, sig, sig_len) != 0) return -126;
        uint8_t hash[32];
        sha256(to_verify, 130, hash);
        if (p256_verify(rs, hash, cert_pk + 1) != 0) return -127;
    }
}
```

The transcript snapshot is taken BEFORE adding the CertificateVerify message itself to the running hash — that's what the server signed (RFC 8446 §4.4.3). For P-256 the wire signature is DER-encoded, so the existing `p256_sig_from_der` unwraps it back to raw `R||S` before `p256_verify`. The `cert_pk + 1` skips the uncompressed-point 0x04 prefix.

After this change, the in-OS httpsget→httpsd loopback now ACTUALLY validates — if bug #1 regresses, `[t37]` fails at the handshake instead of silently accepting garbage.

## Bonus: `close_notify`

Real TLS clients log warnings if the TCP connection closes without a `close_notify` alert first:

```
0A000126:SSL routines::unexpected eof while reading       (openssl)
schannel: server closed abruptly (missing close_notify)   (curl)
```

Cosmetic, but distracting in test output. Added:

```c
int tls_close_notify(struct tls_conn *c) {
    struct tls_keys *k = c->is_server ? &c->s_ap_keys : &c->c_ap_keys;
    uint8_t alert[2] = { 1 /*warning*/, 0 /*close_notify*/ };
    return send_encrypted(c, k, TLS_REC_ALERT, alert, sizeof(alert));
}
```

httpsd calls it right before `sys_close(conn)`. Both clients now log clean closes instead.

## What [t37] actually proves

```c
const char *a[] = { "httpsget.elf", "https://10.0.2.15:4433/", 0 };
sys_exec("httpsget.elf", a);
```

This forks httpsget pointing at our own httpsd through QEMU's SLIRP loopback (`10.0.2.15` is the default IP SLIRP gives the guest). Both ends run inside the VM but exchange real TLS 1.3 cert-flow bytes over real TCP.

The three assertions check the captured stdout of httpsget:

1. **"TLS 1.3 handshake OK"** — only printed after BOTH directions of the handshake succeed, which now requires the client's `ed25519_verify` / `p256_verify` of the server's CertificateVerify to return 0. Bug #1 alone (server sends garbage sig) would fail this; bug #2 alone (client doesn't check) would not — but with both fixed the assertion is meaningful.

2. **"HTTP/1.0 200 came through the encrypted record layer"** — proves the cipher state is intact post-handshake and AEAD records decrypt cleanly.

3. **"response body decrypted into expected greeting"** — proves data integrity end-to-end.

A future regression of either bug would now fail this test, not silently slip through.

## Manual real-world verification

Used the OpenSSH 10.3 / MSYS2 OpenSSL 3.5.6 on the Windows host with QEMU hostfwd:

```bash
qemu-system-i386 -drive format=raw,file=os.img -display none \
    -netdev user,id=net0,hostfwd=tcp::4523-:4433 ...

# Real curl, real Schannel TLS implementation:
curl -k https://127.0.0.1:4523/
Hello from a TLS 1.3 + HTTPS server in AdventOS!
...

# Real openssl s_client:
openssl s_client -connect 127.0.0.1:4523 -tls1_3 -quiet
HTTP/1.0 200 OK
Hello from a TLS 1.3 + HTTPS server in AdventOS!
```

Both deliver the full HTTP response cleanly. No `tls12_check_peer_sigalg: wrong signature type` from OpenSSL, no `unexpected eof` warnings, no Schannel `missing close_notify`. The session 51 doc's note about the gap is now retired.

## Known gaps still

- **No certificate chain validation in our client.** We extract the SubjectPublicKey from the FIRST cert presented and verify CertificateVerify against it — that proves proof-of-possession by whoever holds the cert key. We do NOT walk the chain, validate signatures up to a root CA, check name constraints, expiration, revocation, or hostname binding. Connecting to `https://1.1.1.1/` still happens with `-k`-equivalent posture. Real-world trust would need a CA root store (Mozilla's bundle is ~250 KB) and a name-verification routine, both significant work.
- **No support for sig schemes beyond `ssh-ed25519` and `ecdsa_secp256r1_sha256`.** Real-world public servers also offer `rsa_pss_rsae_*` variants. Adding RSA verification would unlock more endpoints but is the largest remaining algorithm to add (RSA exponentiation, full ASN.1 OID-walking for `rsaEncryption`, RSASSA-PSS signature scheme).
- **No SNI hostname/Subject Alternative Name check.** Same as above — we'd accept a cert claiming to be `example.com` when connecting to `cloudflare.com` as long as the proof-of-possession works.

## Files touched

```
libcrypto/tls.c               +130 -2    sig_alg save/restore + cert validation
                                          x509_extract_pubkey DER walker
                                          tls_close_notify
libcrypto/tls.h               +4         tls_close_notify prototype
user/httpsd.c                 +3         call tls_close_notify before sys_close
user/sh.c                     +65        [t37] selftest
docs/55-real-world-tls-interop.md +new   this file
```
