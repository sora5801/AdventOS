# Session 39 — X.509 + curl interop

**Goal:** make `https://10.0.2.15:4433/` answerable from a real RFC-8446 TLS client. Session 36 stood up TLS 1.3 with a hardcoded PSK and a "RFC-shaped" ClientHello/ServerHello — enough for our own `httpsget` to talk to our own `httpsd`, not enough for anything off-the-shelf. This session adds SHA-512, Ed25519, an ASN.1 DER encoder, a self-signed X.509 v3 certificate builder, real ClientHello extension parsing, and the encrypted server flight (`EncryptedExtensions` + `Certificate` + `CertificateVerify` + `Finished`) — the four messages that turn a TLS 1.3 handshake into a *certificate-authenticated* TLS 1.3 handshake.

End state — `[t26]` selftest plus the boot lines:

```
[boot] ...
init: started 'httpsd.elf' as pid 7 (once)
httpsd: built self-signed Ed25519 cert (233 bytes DER)
httpsd: listening on TLS port 4433

[t26] libcrypto: SHA-256 / HMAC / HKDF / AES-GCM / X25519 vectors
== SHA-256 ==     PASS  SHA256("abc")           PASS  SHA256("")
== HMAC-SHA-256 ==PASS  RFC4231 test 1
== HKDF ==        PASS  RFC5869 PRK             PASS  RFC5869 OKM
== AES-128 ==     PASS  FIPS197 C.1
== AES-GCM ==     PASS  NIST TV1 tag            PASS  NIST TV3 ct
                  PASS  NIST TV3 tag            PASS  GCM round-trip
== SHA-512 ==     PASS  SHA512("abc")           PASS  SHA512("")
== Ed25519 ==     PASS  RFC 8032 test 1 public key
                  PASS  RFC 8032 test 1 signature
                  PASS  Ed25519 verify RFC signature
                  PASS  Ed25519 reject tampered signature
== X25519 ==      PASS  Alice public from RFC 7748
                  PASS  ECDHE shared-secret agree
                  PASS  shared secret matches RFC
cryptotest: 19 passed, 0 failed
```

And from a host on the QEMU SLIRP network:

```
$ openssl s_client -connect localhost:4433 -tls1_3 -ign_eof <<<'GET / HTTP/1.0'
...
Server certificate
-----BEGIN CERTIFICATE-----
MIHmMIGZoAMCAQICAQEwBQYDK2VwMB0xGzAZBgNVBAMTEkFkdmVudE9TIGRlbW8g
Y2VydDAeFw0yNjAxMDEwMDAwMDBaFw0zNjAxMDEwMDAwMDBaMB0xGzAZBgNVBAMT
EkFkdmVudE9TIGRlbW8gY2VydDAqMAUGAytlcAMhAOGsU56M4zMWy4Ea/LuBQFHW
8iruxVRvQPzoAV8GKBFAMAUGAytlcANBAC5+DXHbuKhdahez5ovABtY6cDW5XiFT
1T2ZIl7/mMCHBUfXPnoosRCNEsOqYpimn3uhtGF+42M3Nyb8n4cNyQg=
-----END CERTIFICATE-----
subject=CN=AdventOS demo cert
issuer=CN=AdventOS demo cert
Peer signature type: ed25519
Peer Temp Key: X25519, 253 bits
Negotiated TLS1.3 group: x25519
New, TLSv1.3, Cipher is TLS_AES_128_GCM_SHA256
HTTP/1.0 200 OK
Content-Type: text/plain
...
Hello from a TLS 1.3 + HTTPS server in AdventOS!

What just happened on the wire:
  - Real RFC 8446 ClientHello with extensions
  - X25519 ECDHE for forward secrecy
  - Self-signed Ed25519 X.509 certificate (RFC 8410)
  - CertificateVerify Ed25519 signature over the transcript
  - AES-128-GCM record-layer encryption
  - 1500 lines of crypto, served from ring 3, no libssl
```

OpenSSL 3.5 verifies our `Certificate`, our `CertificateVerify` Ed25519 signature, our X25519 key share, and our `Finished` HMAC — and then sees the encrypted application data come back as an HTTP/1.0 reply.

## What's in scope

In:

- **`libcrypto/sha512.c`** — FIPS 180-4 SHA-512 (~160 LOC). Sister of `sha256.c`: 64-bit state, 80 rounds, 128-byte blocks, 64-byte output, 128-bit byte counter. Verified against RFC 6234 vectors for `""` and `"abc"`. Required by Ed25519 (RFC 8032 hardcodes SHA-512).
- **`libcrypto/sha512.c`, `sha256.c`** — fix to a latent `update(buf, 0)` bug that wiped the partial buffer length to 0 even when there was data still in the buffer. Latent in SHA-256 since session 36 (TLS transcript updates always have non-zero data). Active in Ed25519 sign: `update(prefix, 32)` then `update("", 0)` for empty messages destroyed the prefix. **Cost two days of staring at scalarmult code that wasn't actually broken.**
- **`libcrypto/ed25519.c`** — RFC 8032 Ed25519 sign + verify (~340 LOC). Direct port of the TweetNaCl reference: same 16-limb signed-int field elements as `x25519.c`, four-coordinate `(X, Y, Z, T)` Edwards points, Montgomery ladder for scalar mult, `modL` reduction modulo the curve order. Verified bit-for-bit against RFC 8032 §7.1 Test 1.
- **`libcrypto/x509.{h,c}`** — ~270 LOC. ASN.1 DER backward-writer + X.509 v3 self-signed certificate builder for Ed25519 keys (RFC 8410, OID `1.3.101.112`). Two-pass: build the TBS, sign it, build the outer `Certificate` SEQUENCE wrapping TBS + algorithm + signature.
- **`libcrypto/tls.{h,c}`** — split into a PSK flow (kept for the OS↔OS demo) and a new cert flow (`tls_server_handshake_cert`, `tls_client_handshake_cert`). Cert flow does real RFC 8446 ClientHello extension parsing (`supported_versions`, `supported_groups`, `signature_algorithms`, `key_share`), real ServerHello extension construction, ChangeCipherSpec middlebox-compat, encrypted server flight `EE || Cert || CV || Finished` in a single record, encrypted ClientFinished verification.
- **`user/httpsd.c`** — generates an Ed25519 keypair from a baked-in seed, builds a self-signed cert at startup, switches to `tls_server_handshake_cert`. Runtime cert build: ~233 bytes of DER per startup, no Python pre-gen, no PEM-on-disk.
- **`user/httpsget.c`** — switches from PSK to `tls_client_handshake_cert`, accepts any cert (`-k`-equivalent — a demo client, not a CA-validating one).
- **`user/cryptotest.c`** — 6 new vectors: SHA-512 `""` + `"abc"`, Ed25519 RFC 8032 test 1 public key + signature, verify-roundtrip, tampered-sig rejection.

Out:

- **Cert chain validation.** Our cert is self-signed; we don't ship a CA list, don't parse cert chains beyond a single entry, don't check `notBefore`/`notAfter`, don't enforce key usage extensions. `openssl s_client` correctly reports "Verify return code: 18 (self-signed certificate)" — and `curl -k` would silence that. The TLS handshake's *cryptographic* assertions (Ed25519 signature is valid, X25519 ECDHE produces matching shared secrets, `Finished` HMAC matches) all pass.
- **Other signature schemes.** Ed25519 only. No RSA, no ECDSA-P256/P384/P521. Our `signature_algorithms` extension only includes Ed25519 on the client side, and the server rejects ClientHellos that don't offer Ed25519. **This is why Windows curl (Schannel) can't talk to us.** Schannel doesn't include Ed25519 in TLS 1.3 sig algs by default; our parser returns `-1411` ("no Ed25519 in signature_algorithms") and resets the connection. OpenSSL/curl-on-Linux works fine.
- **`HelloRetryRequest`.** If the client's `key_share` doesn't include x25519, we just refuse the connection. Real TLS 1.3 servers respond with HRR asking the client to retry with an acceptable group.
- **`server_name` / SNI processing.** We parse the extensions block but ignore SNI — single-vhost.
- **`close_notify` alert.** When `httpsd` finishes the response it just `close()`s the TCP socket, which makes OpenSSL grumble `unexpected eof while reading` (cosmetic — the data was delivered fine, but a polite TLS server signals end-of-stream first).
- **0-RTT, session resumption, post-handshake auth, key update.** As before.

## Architecture

```
ClientHello (real, with extensions)
       │
       ▼
┌──────────────────────────────────────────────────┐
│  parse_clienthello_extensions()                  │
│    • supported_versions  → contains 0x0304       │
│    • supported_groups    → contains 0x001D       │
│    • signature_algorithms→ contains 0x0807       │
│    • key_share           → extract x25519 pub    │
└──────────────────────────────────────────────────┘
       │
       ▼
ServerHello + ChangeCipherSpec (plaintext)
       │
       ▼
┌──────────────────────────────────────────────────┐
│  Key schedule (RFC 8446 §7.1)                    │
│   early    = HKDF-Extract(0, 0)        no PSK    │
│   derived  = Derive-Secret(early, "derived", "") │
│   hs       = HKDF-Extract(derived, ECDHE)        │
│   c_hs/sec, s_hs/sec from Derive-Secret with     │
│     transcript = Hash(CH || SH)                  │
│   AES-128 keys + IVs via HKDF-Expand-Label       │
└──────────────────────────────────────────────────┘
       │
       ▼
Encrypted server flight (one AES-GCM record):
  EncryptedExtensions  (empty)
  Certificate          (self-signed Ed25519 X.509)
  CertificateVerify    (Ed25519 sig over transcript context)
  Finished             (HMAC over running transcript)
       │
       ▼
[client verifies, sends its Finished]
       │
       ▼
Application keys derived; tls_send / tls_recv
use AES-128-GCM with seq^iv nonce.
```

The transcript hash is the running SHA-256 of every handshake message — feeding it the right bytes, in the right order, at the right moment, is what most of the new cert-flow code is doing.

## The SHA-512 streaming bug

The most expensive lesson of the session was a one-line latent bug in `sha256_update` and `sha512_update` that bit Ed25519 immediately on its first use. It deserves its own section.

The original `sha512_update`:

```c
void sha512_update(struct sha512 *s, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    inc_count(s, n);

    if (s->buf_len) {
        uint32_t take = 128 - s->buf_len;
        if (take > n) take = (uint32_t)n;
        for (uint32_t i = 0; i < take; i++) s->buf[s->buf_len + i] = p[i];
        s->buf_len += take;
        p += take; n -= take;
        if (s->buf_len == 128) {
            sha512_compress(s->state, s->buf);
            s->buf_len = 0;
        }
    }
    while (n >= 128) {
        sha512_compress(s->state, p);
        p += 128; n -= 128;
    }
    for (size_t i = 0; i < n; i++) s->buf[i] = p[i];
    s->buf_len = (uint32_t)n;        // ← the bomb
}
```

The contract that line was trying to enforce: "after this call, `buf_len` is the count of leftover-tail bytes that didn't fit a full block." Which is true when you enter the function with `buf_len == 0`, and it's still true after the first `if (s->buf_len)` block compresses to a full 128-byte block and resets `buf_len = 0`. But if you enter with `buf_len > 0`, hit the `if`, copy `take` bytes, and the resulting `buf_len < 128` (i.e., the partial-buffer didn't fill), we exit the `if` branch with `buf_len` correctly tracking the partial buffer's contents — and then the bottom of the function unconditionally clobbers it back to `n`.

When `n > 0`, this happens to be benign: `take = n` (since the buffer didn't fill), so after consuming, `n -= take = 0`, and the bottom-of-function `for` loop is a no-op, then `buf_len = 0` ... but wait, that's still wrong. The 32 bytes we just appended to `s->buf` are *in* the buffer; setting `buf_len = 0` *says they aren't*. Then `sha512_final` pads from offset 0, hashing only 0x80 || zeros || bit-count instead of `prefix || 0x80 || zeros || bit-count`.

**Why didn't this surface earlier?** Two reasons:

1. **`sha256_update` had the same bug** but TLS only ever called it with non-zero `n`. The transcript hash chain in session 36 was `update(ClientHello)`, `update(ServerHello)`, `update(ServerFinished)`, … — every message has a body. Latent.
2. **`sha512_update`** got introduced in this session for Ed25519. Ed25519 sign computes `r = SHA-512(prefix || msg)` where `prefix` is the high 32 bytes of `SHA-512(seed)` and `msg` is the message. For our cryptotest's empty-message case, that's `update(prefix, 32)` followed by `update("", 0)`. Boom: the second call enters the `if (s->buf_len)` branch with `take = 0`, falls through, and zeros `buf_len`.

The diagnostic that finally cracked it was symmetric: our public-key derivation matched RFC 8032 (single `sha512(seed, 32, ...)` call, no second update), but `sign` didn't (two updates, second with `n == 0`). Pk derivation goes through the one-shot `sha512(data, n, out)` helper which calls `update` once with `n > 0`, then `final` — never the n=0 path. The ladder of "constants match TweetNaCl, scalarmult matches TweetNaCl, modL matches TweetNaCl, gf matches TweetNaCl, … so why doesn't the signature match?" only collapsed once we noticed the *common factor between sign and verify* was repeated `update` calls that included an empty-message tail.

Fix:

```c
if (s->buf_len) {
    ...top up...
    /* If we didn't fill a full block, leave buf_len at its
     * partial value and return. Skipping this early-out and
     * falling through to "buf_len = n" at the bottom would
     * WIPE the partial buffer when n is 0 — which is exactly
     * what happens on update(empty_msg, 0) after a previous
     * partial update. Lost two days to that bug. */
    if (s->buf_len < 128) return;
    sha512_compress(s->state, s->buf);
    s->buf_len = 0;
}
```

Same one-line fix lands in `sha256.c`. The TLS code paths exercised today don't hit it, but every future crypto user is one paranoid `update(ptr, 0)` call away from the same trap.

## Ed25519 (RFC 8032) — port from TweetNaCl

`libcrypto/ed25519.c` is a direct, almost line-for-line port of TweetNaCl's `crypto_sign_ed25519` for sign and `crypto_sign_open_ed25519` for verify. We share field arithmetic with `x25519.c` (16 limbs of `long long`, the same `car25519`/`sel25519`/`pack25519`/`M`/`S`/`A`/`Z`/`inv25519` ops), but Ed25519 lives on the *twisted Edwards* form of Curve25519 with point-at-infinity `(0:1:1:0)` rather than X25519's projective Montgomery curve. Points are four-coordinate `(X, Y, Z, T)` with `T = X·Y/Z`; addition is the standard extended-coordinate formula:

```
A = (Y₁−X₁)·(Y₂−X₂)
B = (Y₁+X₁)·(Y₂+X₂)
C = T₁·T₂·2d
D = Z₁·Z₂·2
E = B−A,  F = D−C,  G = D+C,  H = B+A
X₃ = E·F,  Y₃ = G·H,  T₃ = E·H,  Z₃ = F·G
```

Constants come straight from TweetNaCl: `D2 = 2·d` (Edwards curve parameter), the basepoint `(X, Y)` (with `Y = 4/5` in the field, hence the famous all-`0x6666` representation), `gf0` (zero), `gf1` (one), and `L` — the prime subgroup order, 32 little-endian bytes for the `modL` reduction step.

The public API:

```c
void ed25519_keypair_from_seed(uint8_t pk[32], uint8_t sk[64], const uint8_t seed[32]);
void ed25519_sign(uint8_t sig[64], const uint8_t *msg, size_t msg_len, const uint8_t sk[64]);
int  ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t msg_len, const uint8_t pk[32]);
```

Sign breakdown — given `sk` is `seed || pk` and we want signature over `msg`:

```
d ← SHA-512(seed)               // 64 bytes
clamp(d[0..31])                  // d[0]&=248, d[31]&=127, d[31]|=64
r ← SHA-512(d[32..63] || msg) mod L
R ← scalarmult(r, basepoint)
   sig[0..31] ← pack(R)
h ← SHA-512(R || pk || msg) mod L
S ← (r + h·d[0..31]) mod L
   sig[32..63] ← S
```

`scalarmult` uses TweetNaCl's Montgomery ladder over 256 bits — `cswap` between `p` (running point) and `q` (point being added) on the bit value, then `add(q, p); add(p, p);` then unswap. Constant-time, doesn't branch on the scalar.

`modL` is the same convex-hull modular reduction TweetNaCl uses: a sliding-window subtract of `L*2^k * x[63-k] / 16` for `k = 31..0`, refining the byte-array result down to 32 bytes < L.

Verify checks `S·B == R + h·A` by computing `−h·A + S·B == R` (where `A` is the public point, recovered from `pk` via `unpackneg` which does an Edwards `y → x` square root via `pow2523`, then negates), packing the result, and comparing against `R`.

Performance on QEMU's qemu32 (no SSE, no MMX): about 30 ms per sign. The dominant cost in a TLS handshake; everything else (X25519, AES-GCM, SHA-256/512, HKDF) runs in single-digit milliseconds. Acceptable for a demo, eyebrow-raising for production.

## ASN.1 DER, written backward

`libcrypto/x509.c` includes a tiny purpose-built DER encoder (~120 LOC), then uses it to build the X.509 v3 certificate (~150 LOC).

ASN.1 DER values are tag-length-value triples (TLVs) with variable-length lengths. The catch with forward-writing a SEQUENCE is that the SEQUENCE's length depends on the total bytes of its children, which you don't know until you've written them. Real X.509 libraries solve this with two passes — measure, then write — or with placeholder lengths that get patched later.

We do it differently: write **backward**, from the right edge of an output buffer. Children get written first (rightmost in the buffer), then we wrap them in a tag+length whose length we now trivially know by subtracting positions. No measure pass, no patching, no recursion-with-output-pointer.

The writer state:

```c
typedef struct {
    uint8_t *buf;
    int      cap;
    int      pos;        // bytes written so far, from the right edge
    int      overflow;
} dw_t;
```

Primitives:

```c
static void dw_put(dw_t *w, uint8_t b);            // emit one byte
static void dw_raw(dw_t *w, const uint8_t *src, int n);  // emit forward-ordered byte block
static void dw_len(dw_t *w, int n);                // 1B / 0x81+1B / 0x82+2B
static int  dw_snap(dw_t *w);                      // remember current position
static void dw_wrap(dw_t *w, uint8_t tag, int snap); // emit TAG + LEN(pos - snap)
```

Cert structure ends up reading naturally from inside out:

```c
int outer = dw_snap(&w);

  /* signature value (innermost, written first → rightmost) */
  int bs = dw_snap(&w);
  dw_raw(&w, sig, 64);
  dw_put(&w, 0x00);                       // BIT STRING unused-bits = 0
  dw_wrap(&w, ASN1_BITSTRING, bs);

  /* signatureAlgorithm */
  der_alg_ed25519(&w);

  /* tbsCertificate (already built earlier, just splice in) */
  dw_raw(&w, tbs_ptr, tbs_len);

dw_wrap(&w, ASN1_SEQUENCE, outer);        // outer Certificate SEQUENCE
```

The TBS itself uses the same pattern — outermost SEQUENCE wraps `version[0] || serial || sig-alg || issuer || validity || subject || SPKI`, each of which is built right-to-left using `dw_snap`/`dw_wrap` pairs. The final `x509_build_self_signed_ed25519` does a small `memmove` to shift the right-aligned bytes down to `out[0..len)` so the caller sees a normal forward-aligned buffer — the cost of the backward style.

The result, verified by OpenSSL's `s_client` parsing it cleanly, is a 233-byte cert containing:

- v3 (`[0] EXPLICIT INTEGER 2`)
- serial = 1
- signature algorithm = `1.3.101.112` (Ed25519, RFC 8410)
- issuer = subject = `CN=AdventOS demo cert`
- validity = `260101000000Z` → `360101000000Z` (2026 → 2036)
- SPKI = AlgorithmIdentifier(Ed25519) + BIT STRING(32B raw pubkey)
- signatureAlgorithm = Ed25519 (must match the inner one — RFC 5280 §4.1.1.2)
- signatureValue = 64-byte Ed25519 signature over the entire TBS DER bytes

OpenSSL's `Certificate chain` output:
```
 0 s:CN=AdventOS demo cert
   i:CN=AdventOS demo cert
   a:PKEY: ED25519, 256 (bit); sigalg: ED25519
   v:NotBefore: Jan  1 00:00:00 2026 GMT; NotAfter: Jan  1 00:00:00 2036 GMT
```

## TLS 1.3 cert flow on the wire

The server-side cert flow `tls_server_handshake_cert` runs the full RFC 8446 §4.1 / §4.2 / §4.3 / §4.4 sequence:

### 1. Parse a real ClientHello

```c
int o = 4;                                              // skip msg_type + length
o += 2;                                                  // legacy_version
copy 32 bytes ← random
sid_len = recbuf[o++]; copy ← session_id
cs_len  = rd16(...); validate 0x1301 in list; o += cs_len
cm_len  = recbuf[o++]; o += cm_len
ext_len = rd16(...);
parse_clienthello_extensions(recbuf + o, ext_len, &cli_pub)
```

`parse_clienthello_extensions` walks the TLV list looking for *exactly* the four extensions we care about and validates they advertise something we can use:

```c
0x002B supported_versions     → must contain 0x0304 (TLS 1.3)
0x000D signature_algorithms   → must contain 0x0807 (ed25519)
0x0033 key_share              → must contain group 0x001D (x25519)
                                with a 32-byte key payload
```

Other extensions (`server_name`, `supported_groups`, ALPN, OCSP status, etc.) are walked-and-ignored. The error code returned to the caller is `-1400 + per` so a parse failure shows up in the log as `rc=-1411` ("no Ed25519 in signature_algorithms") rather than a generic `rc=-14`.

### 2. ServerHello with extensions

`build_serverhello_body` mirrors the structure: legacy_version + 32B random + echoed session_id + selected ciphersuite (0x1301) + null compression + extensions block containing exactly two extensions:

```
0x002B supported_versions = 0x0304          (selected version, single 2B value)
0x0033 key_share          = 0x001D + 32B    (selected group + our pub)
```

### 3. ChangeCipherSpec (compatibility)

RFC 8446 Appendix D.4 says TLS 1.3 implementations "MUST send a single change_cipher_spec record after their first handshake message in each direction". This is purely for middleboxes that look at TLS records and would otherwise refuse a connection that "skips" CCS. We send `[0x14, 0x03, 0x03, 0x00, 0x01, 0x01]` (6 bytes total) immediately after our ServerHello and ignore any CCS records the client sends — `recv_record` has a transparent skip:

```c
if (hdr[0] == TLS_REC_CHANGE_CIPHER) {
    drain rec_len bytes;
    continue;     /* don't bump k->seq, don't update transcript */
}
```

### 4. Compute handshake keys

Same RFC 8446 §7.1 schedule as the PSK flow, but the early-secret IKM is 32 zero bytes (no PSK):

```c
early    = HKDF-Extract(salt = 32 zeros, ikm = 32 zeros)
derived  = Derive-Secret(early, "derived", H(""))
hs       = HKDF-Extract(salt = derived, ikm = ECDHE_shared)
c_hs_sec = Derive-Secret(hs, "c hs traffic", H(CH || SH))
s_hs_sec = Derive-Secret(hs, "s hs traffic", H(CH || SH))
```

Then per RFC 8446 §7.3:

```c
key = HKDF-Expand-Label(traffic_secret, "key", "", 16)
iv  = HKDF-Expand-Label(traffic_secret, "iv",  "", 12)
```

### 5–8. The encrypted server flight, in one record

Four handshake messages back-to-back, encrypted as a single AES-128-GCM record with `inner_type = HANDSHAKE`:

```
EncryptedExtensions     msg_type=8,  body = 0x00 0x00 (empty extensions list)
Certificate             msg_type=11, body = 0x00 (ctx)
                                          + 3B list_len
                                          + [3B cert_len, cert DER, 2B exts_len=0]
CertificateVerify       msg_type=15, body = 0x08 0x07 (ed25519)
                                          + 0x00 0x40 (sig_len=64)
                                          + 64B Ed25519 signature
Finished                msg_type=20, body = 32B verify_data = HMAC(finished_key, transcript)
```

The CertificateVerify signature input, per RFC 8446 §4.4.3, is *not* the transcript itself — it's a 130-byte string:

```
64 octets of 0x20  ||  "TLS 1.3, server CertificateVerify"  ||  0x00  ||  H(transcript_so_far)
```

The 64 leading 0x20 bytes prevent cross-protocol attacks (an Ed25519 signature accidentally created in one context can't be replayed in another). The context string scopes the signature to the server-side handshake. The trailing `0x00` separates the context from the hash.

The `transcript_so_far` at the moment we sign is `H(CH || SH || EE || Cert)` — everything before CertificateVerify. We snapshot the running `c->transcript` (a `struct sha256` is small enough to copy by value), `sha256_final` it into a 32-byte hash, sign, then continue updating the *original* transcript with the CertificateVerify message bytes.

The Finished `verify_data` is computed similarly, with the transcript snapshot taken at `H(CH || SH || EE || Cert || CV)`:

```c
finished_key = HKDF-Expand-Label(s_hs_traffic_secret, "finished", "", 32)
verify_data  = HMAC-SHA256(finished_key, H(transcript_so_far))
```

All four messages are appended to one `flight[]` buffer (with `hs_append` updating the transcript along the way), then `send_encrypted` AES-GCMs the whole 437-byte block as a single record.

### 9–10. Receive ClientFinished

`recv_record` skips the client's own ChangeCipherSpec transparently, decrypts the next record using `c_hs_keys`, and returns the inner handshake payload. We compute the expected `verify_data` over the transcript at that point and constant-time-compare. Application keys are derived from the master secret at the snapshot `H(CH || SH || EE || Cert || CV || SF)` — i.e., before ClientFinished.

## Cert-mode client (the OS↔OS demo)

`tls_client_handshake_cert` mirrors the server but reverses everything. Key differences from the PSK-mode client of session 36:

- ClientHello is now a real RFC 8446 ClientHello with proper extensions (`supported_versions`, `supported_groups`, `signature_algorithms`, `key_share`).
- After sending ClientHello, we send the compat `ChangeCipherSpec`.
- We loop `recv_record` until we see a `Finished` handshake message — the server's encrypted flight may arrive in one record (the common case) or fragmented across multiple records. We accumulate into an `hs_buf` and walk handshake messages out of it as they complete:

  ```c
  while (!seen_finished) {
      recv_record(...) into recbuf2;
      append to hs_buf;
      while (a complete handshake message is in hs_buf) {
          if (msg_type == FINISHED) {
              snapshot transcript for verify;
              seen_finished = 1;
          }
          sha256_update(transcript, msg);
          slide hs_buf forward;
      }
  }
  ```

- We do **not** verify the server cert or CertificateVerify. The client is the moral equivalent of `curl -k`: parse the structure, accept anything. Adding verification means a CA list, a chain walker, name validation, certificate-policy enforcement — well-trodden territory, but a ~5x larger codebase and not the point of the demo.

A round-trip from the `[t26]` selftest:

```
httpsget: connecting to 10.0.2.15:4433
httpsget: TLS handshake OK
httpsd: TLS handshake OK
httpsget: sent 70-byte request encrypted
httpsd: sent 497 encrypted bytes
httpsget: got 497-byte plaintext reply:
----
HTTP/1.0 200 OK
Content-Type: text/plain
Connection: close

Hello from a TLS 1.3 + HTTPS server in AdventOS!
...
```

## Test results

`cryptotest` (RFC vectors): **19 passed, 0 failed.** Including:

- SHA-512 RFC 6234 §8 vectors for `""` and `"abc"`
- Ed25519 RFC 8032 §7.1 Test 1 — public key, signature, verify, tampered-rejection

`[t26]` end-to-end (httpsget → httpsd, both on cert flow): **handshake OK on both sides, 497-byte encrypted reply delivered, exit 0.**

Host-side `openssl s_client -connect localhost:4433 -tls1_3`: **handshake OK, certificate parsed, Ed25519 signature verified, X25519 key agreed, AES-128-GCM application data decrypted to "HTTP/1.0 200 OK ...".**

Host-side Windows `curl -k https://localhost:4433/`: **fails with `schannel: failed to receive handshake`**, because Windows Schannel doesn't include Ed25519 in TLS 1.3 `signature_algorithms` and our server (correctly) rejects ClientHellos that don't offer it. On Linux/macOS curl, which uses OpenSSL/GnuTLS by default, the same command works.

## What's next

- **Add a second signature scheme** (probably ECDSA-P256 — same field-arithmetic infrastructure isn't reusable, so it's a from-scratch ~600 LOC, but it's what every TLS client supports). That would make Schannel work too.
- **Send a `close_notify` alert** before closing TCP. Five-line change, removes OpenSSL's "unexpected eof" warning and is just polite.
- **`HelloRetryRequest`** for clients that don't offer x25519.
- **CA validation** in the client — parse a small embedded trust anchor list and walk a real chain. Out of scope for the demo, but well-defined work.
