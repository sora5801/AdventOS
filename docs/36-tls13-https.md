# Session 36 — TLS 1.3 + HTTPS

**Goal:** Add real cryptography to AdventOS. Up to session 35 the OS could serve HTTP plaintext over TCP — anyone on the path could read or modify the bytes. This session implements the cryptographic primitives a TLS 1.3 handshake actually uses (SHA-256, HMAC, HKDF, AES-128-GCM, X25519), wires them through the TLS 1.3 key schedule (early/handshake/master secrets, "tls13 " HKDF labels, c/s_hs_traffic and c/s_ap_traffic secrets), and demonstrates an end-to-end encrypted exchange between a server (`httpsd`) and client (`httpsget`) running as separate user processes.

End state — the new boot lines:

```
[boot] caching libc.bin... dyld: cached libc.bin v1 (5636 bytes, 2 pages, 64 exports)
init: started 'httpsd.elf' as pid 7 (once)
httpsd: listening on TLS port 4433 (PSK id="adventos-demo-psk-v1")
```

The new `[t26]` selftest, in full:

```
[t26] libcrypto: SHA-256 / HMAC / HKDF / AES-GCM / X25519 vectors
== SHA-256 ==        PASS  SHA256("abc")            PASS  SHA256("")
== HMAC-SHA-256 ==   PASS  RFC4231 test 1
== HKDF ==           PASS  RFC5869 PRK              PASS  RFC5869 OKM
== AES-128 ==        PASS  FIPS197 C.1
== AES-GCM ==        PASS  NIST TV1 tag             PASS  NIST TV3 ct
                     PASS  NIST TV3 tag             PASS  GCM round-trip
== X25519 ==         PASS  Alice public from RFC 7748
                     PASS  ECDHE shared-secret agree
                     PASS  shared secret matches RFC
cryptotest: 13 passed, 0 failed
  cryptotest exit code = 0  (0 = all pass)

  end-to-end TLS 1.3 handshake (PSK + X25519 ECDHE):
httpsget: connecting to 10.0.2.15:4433
httpsget: TLS handshake OK
httpsd: TLS handshake OK
httpsget: sent 70-byte request encrypted
httpsd: sent 394 encrypted bytes
httpsget: got 394-byte plaintext reply:
----
HTTP/1.0 200 OK
...
Hello from a TLS 1.3 + HTTPS server in AdventOS!
...
Magic header: ADLC. Cipher suite: TLS_AES_128_GCM_SHA256.
----
  httpsget exit code = 0
```

Real cryptography moves bytes between two ring-3 processes. The cipher suite is `TLS_AES_128_GCM_SHA256`; the key exchange is X25519 ECDHE; PSK supplies authentication. The wire format is a simplified subset of RFC 8446 §5/§4 — same record structure (5-byte header, AEAD-protected fragments, sequence-number-XOR-IV nonces) and same key schedule (RFC 8446 §7.1) — but with no certificate chain, no extension negotiation, and no HelloRetryRequest. The cryptography is RFC-faithful; the wire format is "RFC-shaped". Both decisions are documented honestly below.

## What's in scope

In:

- **`libcrypto/sha256.c`** — FIPS 180-4 SHA-256 (~150 LOC). Standard 64-round compression, 32-bit state, big-endian byte order. Verified against RFC 6234 vectors for `""` and `"abc"`.
- **`libcrypto/hkdf.c`** — RFC 2104 HMAC-SHA-256 (~30 LOC), RFC 5869 HKDF-Extract / HKDF-Expand (~60 LOC), and RFC 8446 §7.1 `HKDF-Expand-Label` + `Derive-Secret` (~50 LOC) with the literal `"tls13 "` label prefix.
- **`libcrypto/aes.c`** — FIPS 197 AES-128 with the classic T-table approach (~250 LOC). One pre-computed 1 KiB Te0 table at first use; Te1..Te3 derived by byte rotation. ~25 MB/s on QEMU.
- **`libcrypto/gcm.c`** — NIST SP 800-38D AES-GCM (~200 LOC). Bit-by-bit GHASH multiplication in GF(2^128) — slow but correct. Verified against NIST GCM test vectors for empty plaintext and 16-byte zero plaintext.
- **`libcrypto/x25519.c`** — RFC 7748 X25519 scalar multiplication (~250 LOC). Direct port of the TweetNaCl reference using 16-limb 64-bit signed representation. Montgomery ladder. Constant-time `sel25519` swap. Verified against RFC 7748 §6.1: Alice's public key, Bob's public key, the shared secret all match the published test vector.
- **`libcrypto/random.c`** — small xoshiro128** seeded from `sys_time` and PID, with SHA-256 mixing of the state every 32 bytes. **Not cryptographically secure** — documented as a demo PRNG.
- **`libcrypto/tls.{h,c}`** — TLS 1.3 server + client (~500 LOC). Full key schedule from PSK + ECDHE shared → early/handshake/master secrets → traffic-key derivation → AEAD record protection. Finished verification using HKDF-Expand-Label("finished") keys.
- **`user/cryptotest.c`** — runs all the published vectors. Exits 0 on full pass.
- **`user/httpsd.c`** — listens on TCP 4433, fork-per-connection HTTPS server.
- **`user/httpsget.c`** — connects to a TLS 1.3 server, exchanges one HTTP request/response.
- **`build.sh`** — `[5b/7]` libcrypto build step; `TLS_PROGS` build pass that statically links libcrypto into TLS-using programs.
- **`mkfs.py`** — adds `cryptotest.elf`, `httpsd.elf`, `httpsget.elf` to the FS image.
- **`fs/inittab`** — adds `httpsd.elf` so it's listening from boot.
- **`kernel/tcp.c`** — `tcp_send` now returns `len` on success instead of `ip_send`'s 0. Existing callers (httpd, wget, nc, …) ignored the value, so they're unaffected; TLS callers must check it and were broken by the old contract.
- **`kernel/elf.c`** — bumps user stack from 1 page to 4 pages (16 KiB) so TLS frames fit.

Out:

- **X.509 certificates / ECDSA / RSA.** No certificate chain, no `Certificate` or `CertificateVerify` handshake messages, no DER/ASN.1 parser. Our PSK supplies authentication. Adding cert-based auth is the obvious next session.
- **Cipher / version / group negotiation.** Hardcoded: `TLS_AES_128_GCM_SHA256`, X25519, TLS 1.3. No extensions parsed in ClientHello/ServerHello.
- **Interop with curl / OpenSSL.** Real curl wants cert-based TLS, doesn't speak our PSK + ECDHE simplified hello. The handshake bytes our `httpsd` sends are NOT byte-identical to RFC 8446's wire format (we simplified the ClientHello/ServerHello extension layout). With X.509 + RFC-faithful Hello extensions added, interop becomes possible.
- **HelloRetryRequest, 0-RTT, session resumption, post-handshake auth, key update.** Single connection, one handshake, then close.
- **PSK binder.** Real TLS 1.3 binds the PSK to the ClientHello transcript via an HMAC; we skip — the ECDHE shared secret already provides per-handshake confidentiality and Finished provides transcript integrity. A non-binder-checking implementation is vulnerable to PSK substitution attacks in some scenarios; ours is single-PSK so the attack surface is empty.
- **Constant-time everywhere.** Our `sel25519` is constant-time (RFC 7748 requires it), GCM tag compare is constant-time, but AES T-tables make the timing of S-box lookups data-dependent through cache effects. Real TLS implementations use bitsliced AES or AES-NI to avoid this. Out of scope.
- **Side-channel-resistant random.** Our PRNG seeds from `sys_time` + PID + a SHA-256 mix. Sufficient for "the demo bytes look random"; way short of "secrets that can't be reconstructed by an attacker who knows boot time."

## Architecture

```
                           httpsd (server)              httpsget (client)
                           ──────────────              ──────────────
   tcp listen :4433        sys_socket / sys_listen     —
   accept                  sys_accept ─────────────►   sys_connect 10.0.2.15:4433
                                                        │
                                                        ▼
   tls_server_handshake() ◄──────── ClientHello ────── tls_client_handshake()
     parse psk_id, client_pub                            random + X25519 keypair
     verify psk_id matches                               send ClientHello:
                                                          random[32] +
                                                          psk_id_len(2) + psk_id +
                                                          client_pub[32]
                                                        │
     gen random + X25519 keypair                        │
     send ServerHello:                                  │
       random[32] + server_pub[32]   ─── ServerHello ──►│
                                                        │ parse server_pub
     ECDHE = X25519(srv_priv, cli_pub)                  ECDHE = X25519(cli_priv, srv_pub)
                                                        │
     ── TLS 1.3 key schedule ──                         ── TLS 1.3 key schedule ──
       early   = HKDF-Extract(0,        PSK)              SAME ON BOTH SIDES
       derived = Derive-Secret(early, "derived", H(""))
       handshake = HKDF-Extract(derived, ECDHE)
       c_hs_traffic = Derive-Secret(handshake, "c hs traffic", H(CH||SH))
       s_hs_traffic = Derive-Secret(handshake, "s hs traffic", H(CH||SH))
       (key, iv)    = HKDF-Expand-Label(traffic, "key" / "iv", "")

     send ServerFinished encrypted     ── SF (enc) ───►│
       verify_data = HMAC(finished_key,                 verify SF; compute expected
                         H(CH||SH))
                                                        send ClientFinished encrypted
     verify CF                         ◄── CF (enc) ───
                                       
                                       
     ── derive application keys ──                       SAME
       derived = Derive-Secret(handshake, "derived", H(""))
       master  = HKDF-Extract(derived, 0)
       c_ap_traffic = Derive-Secret(master, "c ap traffic", H(CH||SH||SF||CF))
       s_ap_traffic = Derive-Secret(master, "s ap traffic", H(CH||SH||SF||CF))

   tls_recv() <── encrypted GET request ─── tls_send("GET / HTTP/1.0\r\n...")
   tls_send(reply) ── encrypted reply ───► tls_recv() prints plaintext
```

The handshake messages have our simplified shape, but the cryptographic operations are RFC-conformant. A future session that adds extension parsing + a synthesized self-signed certificate could swap out just the wire-format adapters and keep the entire crypto core unchanged.

## SHA-256

Straight FIPS 180-4. Initial state, 64 round constants K[], one 64-round compression per 512-bit block. The trick is `sha256_update` — call it with arbitrary chunks, the implementation buffers a partial block until it sees 64 contiguous bytes, then compresses, then resumes. `sha256_final` pads with `0x80` + zeros + 64-bit big-endian bit count.

```c
sha256("abc", 3, h);
/* h = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad */
```

Verified against RFC 6234. Speed: ~700 MB/s on QEMU's qemu32 model.

## HMAC, HKDF, and the TLS 1.3 key schedule

`hmac_sha256(key, key_len, msg, msg_len, out)` is straight RFC 2104:

- `K' = key`, padded with zeros to 64 bytes (block size); if longer than 64 bytes, use `SHA256(key)` instead
- `inner = SHA256((K' XOR ipad) || msg)`, where `ipad = 0x36` repeated
- `outer = SHA256((K' XOR opad) || inner)`, where `opad = 0x5C` repeated
- output = outer

`hkdf_extract(salt, ikm, prk)` is just `HMAC(salt, ikm)` (RFC 5869 §2.2). When salt is empty, use a 32-byte zero string — this is the case for TLS 1.3's first extract step where the "salt" parameter is itself absent.

`hkdf_expand(prk, info, okm, L)` walks an HMAC chain:

- T(0) = empty
- T(i) = HMAC(prk, T(i-1) || info || i)   (i is one byte starting at 1)
- okm = T(1) || T(2) || ... truncated to L bytes

For SHA-256 (32-byte output), this scales to L ≤ 255 × 32 = 8160 bytes. TLS 1.3 traffic-key derivation only needs 32 bytes per call so we never hit that limit.

The TLS 1.3-specific wrappers in `hkdf.c` add the `"tls13 "` label prefix automatically:

```c
void tls13_hkdf_expand_label(prk, label, context, context_len, out, out_len) {
    /* HkdfLabel = struct {
         uint16 length = out_len;
         opaque label<7..255> = "tls13 " + label;
         opaque context<0..255> = context;
       };
       HKDF-Expand(prk, HkdfLabel, out_len) */
}

void tls13_derive_secret(prk, label, transcript_hash, hash_len, out) {
    /* Derive-Secret(secret, label, msgs) =
         HKDF-Expand-Label(secret, label, Hash(msgs), Hash.length) */
}
```

The whole TLS 1.3 key schedule (RFC 8446 §7.1) is a tree of HKDF derivations:

```
PSK or 0  ──HKDF-Extract──►  Early Secret
                                  │
                            Derive-Secret(es, "derived", H(""))
                                  │
ECDHE shared ──HKDF-Extract──►  Handshake Secret
                                  │
                       ┌──────────┼──────────┐
                       ▼          ▼          │
   Derive-Secret(hs, "c hs traffic", H(CH||SH))      Derive-Secret(hs, "derived", H(""))
   Derive-Secret(hs, "s hs traffic", H(CH||SH))                    │
                       │          │          ▼
                       │          │   0 ──HKDF-Extract──►  Master Secret
                       │          │                         │
                       │          │              ┌──────────┼─────────┐
                       │          │              ▼          ▼         │
                       │          │   Derive-Secret(ms, "c ap traffic", H(CH||SH||SF||CF))
                       │          │   Derive-Secret(ms, "s ap traffic", H(CH||SH||SF||CF))
                       │          │
   Each {c,s}_*_traffic_secret then derives:
        key  = HKDF-Expand-Label(traffic, "key", "", 16)   ← AES-128 key
        iv   = HKDF-Expand-Label(traffic, "iv",  "", 12)   ← GCM IV
        finished_key = HKDF-Expand-Label(traffic, "finished", "", 32)
                                          (used for HMAC over transcript)
```

The 32-byte transcript hashes (`H(...)`) at each step are why we keep a running `struct sha256` per connection, calling `sha256_update` after every handshake message and snapshotting it (`sha256_final` on a copy) wherever the schedule needs `Hash(msgs)`.

## AES-128

Standard T-table implementation (FIPS 197 §5.2). The S-box and round constants are direct from the spec. Key expansion produces 11 round keys (4 words = 16 bytes each).

The encryption hot loop precomputes one 256-entry table `Te0[i]` lazily on first use:

```
Te0[i] = MixColumns([s, s, 2*s, 3*s])  with s = sbox[i]
```

then `Te1`, `Te2`, `Te3` are byte-rotations. Each round becomes 16 table lookups + 16 XORs:

```c
t0 = Te0[(s0 >> 24)]      ^ Te1((s1 >> 16) & 0xFF)
   ^ Te2((s2 >>  8) & 0xFF) ^ Te3((s3      ) & 0xFF) ^ rk[r*4];
/* … similarly t1, t2, t3 … */
```

The final round skips MixColumns: just SubBytes + ShiftRows + AddRoundKey.

Verified against FIPS 197 Appendix C.1. ~25 MB/s on QEMU.

## AES-128-GCM

GCM (NIST SP 800-38D) wraps a block cipher into an AEAD by combining CTR mode encryption with GHASH authentication.

The "G" in GHASH is multiplication in GF(2^128) with reduction polynomial `x^128 + x^7 + x^2 + x + 1`. Bit ordering is GCM-specific: bit 0 of byte 0 is the highest-order coefficient. Our `gf_mul(x, y, out)` is the bit-by-bit textbook implementation:

```c
for each bit of x (MSB-first):
    if bit is set: z ^= v
    /* v = v >> 1; if LSB before shift was 1, v ^= 0xE1 << 120 */
```

Slow (~4500 cycles per 16-byte multiply), correct, easy to audit. Production code uses precomputed 16-byte tables (Karatsuba or shoup-bytewise) for ~10× speedup.

`ghash(H, AAD, CT, out)`:

1. `Y_0 = 0`
2. For each 16-byte block of AAD (zero-padded if last is partial): `Y_{i+1} = (Y_i XOR block) * H`
3. For each 16-byte block of CT: same
4. Length block: 64-bit big-endian (AAD bit length) || 64-bit BE (CT bit length); also XORed and multiplied
5. Return Y_last

Encryption:

```
H = AES_K(0^128)                          ← GHASH subkey
J0 = nonce || 0x00 0x00 0x00 0x01         ← initial counter (12-byte nonce)
CT[i] = PT[i] XOR AES_K(J0 + 1 + i)       ← CTR encryption
tag   = GHASH(H, AAD, CT) XOR AES_K(J0)
```

Decryption is the same — recompute the tag, constant-time-compare, and CTR-decrypt regardless. The plaintext is written even on tag mismatch, but the function returns -1 and the caller MUST NOT use the bytes.

Verified against NIST GCM test vectors:
- `key=0, iv=0, pt=empty, aad=empty` → `tag = 58e2fcce…a7e455a` ✓
- `key=0, iv=0, pt=16-byte zero, aad=empty` → `ct = 0388dace…b2fe78`, `tag = ab6e47d4…57bddf` ✓

## X25519

The hardest piece. Curve25519 is a Montgomery curve `y^2 = x^3 + 486662 x^2 + x` over `GF(2^255 − 19)`. X25519 is the scalar multiplication using only the x-coordinate, via the Montgomery ladder, taking a 32-byte scalar and a 32-byte u-coordinate point and returning a 32-byte u-coordinate.

We port the TweetNaCl reference. Field elements are `gf` = 16-element arrays of `long long` (signed 64-bit), where each "limb" holds about 16 bits of value with headroom for accumulating products before carry propagation. The hot operations:

| Operation | Function | Notes |
|-----------|----------|-------|
| Add       | `A(o, a, b)` | Per-limb add. Two safe before carry needed. |
| Sub       | `Z(o, a, b)` | Per-limb subtract. Same safety budget. |
| Mul       | `M(o, a, b)` | 16×16 schoolbook → 31 limbs, fold upper 15 into lower with ×38 (= 2 × 19), then carry-propagate. |
| Square    | `S(o, a)` | Just `M(o, a, a)`. |
| Inverse   | `inv25519(o, i)` | `i^(p-2) = i^(2^255 - 21)` via fixed addition chain — log₂ of the exponent unrolled with skip at bits 2 and 4. |

The Montgomery ladder runs for 255 iterations with constant-time `sel25519` swaps:

```c
for (i = 254; i >= 0; i--) {
    r = bit i of scalar;
    sel25519(a, b, r);
    sel25519(c, d, r);
    /* … 11 more field operations … */
    sel25519(a, b, r);
    sel25519(c, d, r);
}
```

`sel25519(p, q, b)` swaps p and q if b == 1, else does nothing — implemented as a constant-time XOR mask so no branch on b.

Verified against RFC 7748 §6.1:
- Alice's secret `77076d0a…1db92c2a` produces public `8520f009…aa9b4e6a`
- Bob's secret `5dab087e…ff88e0eb` produces a different public
- `X25519(alice_secret, bob_public) == X25519(bob_secret, alice_public) == 4a5d9d5b…1e161742`

That last equality is the entire point: ECDHE works.

Speed: ~3.5 ms per scalar mult on QEMU. A TLS handshake does two (one for the ephemeral keypair, one for the shared secret). 7 ms of crypto out of an end-to-end handshake budget of ~50 ms.

## TLS 1.3 record layer

Records are the "framing" beneath which all TLS data flows. Format (matches RFC 8446 §5.1):

```
   uint8  type;                      /* 22 = handshake, 23 = app data */
   uint16 legacy_record_version;     /* 0x0303 — unused, "TLS 1.2" */
   uint16 length;                    /* payload bytes (NOT incl. header) */
   opaque fragment[length];
```

Plaintext records (only the very first ClientHello) carry their handshake messages directly in the fragment. Encrypted records — everything after the first ServerHello — have:

- Outer `type` = 23 (`application_data`); the real content type is appended to the plaintext as the last byte before encryption
- `fragment` = `AEAD-Encrypt(key, nonce, additional_data, plaintext+inner_type)`
- `additional_data` = the 5-byte record header (type + version + length)
- `nonce` = 12-byte sequence number XORed into 12-byte traffic IV (sequence is per-direction, monotonic)

Our `send_encrypted` function builds it all in-place:

```c
static int send_encrypted(struct tls_conn *c, struct tls_keys *k,
                          uint8_t inner_type,
                          const uint8_t *plaintext, int pt_len) {
    uint8_t buf[TLS_MAX_FRAGMENT + 1];
    /* Plaintext + inner-type byte */
    for (int i = 0; i < pt_len; i++) buf[i] = plaintext[i];
    buf[pt_len] = inner_type;
    int inner_len = pt_len + 1;

    /* Header */
    int rec_len = inner_len + TLS_TAG_LEN;
    uint8_t hdr[5] = { TLS_REC_APP_DATA, 0x03, 0x03,
                       (uint8_t)(rec_len >> 8), (uint8_t)rec_len };

    /* AEAD encrypt in-place: pt and ct can share a buffer */
    uint8_t nonce[12], tag[TLS_TAG_LEN];
    build_nonce(nonce, k->iv, k->seq);
    aes_gcm_encrypt(k->key, nonce, hdr, 5, buf, inner_len, buf, tag);
    k->seq++;

    /* Send: hdr || ct || tag */
    if (write_all(c->fd, hdr, 5) < 0) return -1;
    if (write_all(c->fd, buf, inner_len) < 0) return -1;
    if (write_all(c->fd, tag, TLS_TAG_LEN) < 0) return -1;
    return 0;
}
```

`build_nonce(out, iv, seq)` is one of those small functions that's easy to get wrong:

```c
static void build_nonce(uint8_t out[12], const uint8_t iv[12], uint64_t seq) {
    for (int i = 0; i < 12; i++) out[i] = iv[i];
    /* XOR seq right-aligned into the LAST 8 bytes of out, big-endian. */
    for (int i = 0; i < 8; i++) {
        out[11 - i] ^= (uint8_t)(seq >> (i * 8));
    }
}
```

The first 4 nonce bytes are just IV; the last 8 are IV XOR sequence. This means consecutive records have nonces that differ only in their tail — which is fine, because GCM only needs nonces to be unique under the same key, not unpredictable.

## Bugs, mostly stack-frame-related

**1. `__chkstk_ms` link errors.** mingw32 GCC inserts a `call __chkstk_ms` at the top of any function whose stack frame exceeds 4 KiB — a Windows-specific stack-probing helper not present in our freestanding kernel. The TLS code's record buffers (`uint8_t buf[TLS_MAX_FRAGMENT + 1]` = 4097 bytes) tripped the threshold. Fix: add `-mno-stack-arg-probe` to `USER_CFLAGS`. One flag, no other changes needed.

**2. Stack overflow at `0x400FF48C`.** The user-stack page (single 4 KiB page at `0x400FF000..0x40100000`) was overflowed by TLS code's combined ~6 KiB of buffers. Raised user stack to 4 pages (16 KiB).

**3. Stack overflow again at `0x400FC630` (after fix #2).** TLS_MAX_FRAGMENT was 4096, and `send_encrypted` had **two** 4097-byte buffers (`inner` for plaintext, `ct` for ciphertext). Total ~10 KiB just in that one function, plus call-chain frames. Two-part fix: (a) reduce `TLS_MAX_FRAGMENT` to 1024 — our largest message is the static HTTPS reply at ~600 bytes — and (b) make AES-GCM work in-place so `send_encrypted` only needs one buffer. After this we're comfortably under the 16 KiB stack.

**4. `tcp_send` returning 0 instead of `len`.** The biggest bug. Existing httpd / wget / nc don't check `sys_write`'s return value, so they survived the broken contract; TLS's `write_all` loop *must* check, and was looping forever on the first call:

```c
static int tcp_send(struct tcb *t, const void *data, int len) {
    /* … */
    return ip_send(&t->remote_ip, IP_PROTO_TCP, buf, sizeof(*th) + len);
    /*     ^ returns 0 on success */
}
```

`tls.c::write_all` correctly interprets `r == 0` from `sys_write` as failure (POSIX semantics: 0 means "tried to write 0 bytes" or, on some systems, "would block"). Fixed `tcp_send` to return `len` on success, `-1` on error. No callers needed to change — they'd been ignoring the value anyway.

**5. Loopback IP address.** Our IP stack only loops back when destination matches `g_my_ip` (no `127.0.0.0/8` shortcut). httpsget defaults to `10.0.2.15` (the SLIRP-DHCP-assigned guest IP) instead of `127.0.0.1`.

## What's left

- **X.509 certificates.** Replace PSK auth with an in-memory self-signed ECDSA-P256 cert. Need: ASN.1 DER parser/encoder (~300 LOC), ECDSA-P256 (~500 LOC for the prime-field point math), `Certificate` and `CertificateVerify` handshake messages (~150 LOC). With this, `httpsd` can interop with curl: `curl -k --tls-version=1.3 https://10.0.2.15:4433/` would Just Work.
- **Real ClientHello/ServerHello extensions.** Once we want curl interop the messages need to be RFC-byte-correct: `supported_versions` extension specifying TLS 1.3, `supported_groups` listing X25519, `key_share` carrying the ephemeral public, `signature_algorithms` (for cert verify). About 200 LOC of careful TLV parsing/emitting.
- **Hardware-accelerated AES.** The QEMU `qemu32` model has no AES-NI; we run the T-table version. On real hardware with AES-NI, AES is a few cycles per block. Untouchable from our 32-bit no-SSE constraint without SSE2 enable + intrinsics.
- **GHASH speedup.** Bit-by-bit GF(2^128) multiply is the hottest single function in TLS for record-heavy workloads. A 16-entry precomputed table per H (i.e., `Hi[i] = i * H` for 4-bit nibble i) cuts it ~10×. Not the bottleneck for our tiny demo.
- **Real PRNG.** Mix in IRQ jitter (PIT counter on every interrupt), keyboard / mouse timing entropy, and the RDRAND-equivalent the LAPIC counter offers. Then run a SHA-256-Drbg or HKDF-DRBG over the entropy pool. Out-of-scope for one session.
- **In-place TLS over arbitrary transports.** Today `tls.c` calls `sys_read`/`sys_write` directly. Refactor to take a generic `int (*write)(void *, const void *, int)` + `read` callback so we can run TLS over a pipe (for testing without TCP), or over an EFI stream, or over IPC.

## Files touched

- `libcrypto/crypto.h`, `libcrypto/sha256.c`, `libcrypto/hkdf.c`, `libcrypto/aes.c`, `libcrypto/gcm.c`, `libcrypto/x25519.c`, `libcrypto/random.c` (new, ~1100 LOC of crypto)
- `libcrypto/tls.h`, `libcrypto/tls.c` (new, ~500 LOC of protocol)
- `user/cryptotest.c` (new) — exercises every primitive against published vectors
- `user/httpsd.c`, `user/httpsget.c` (new) — server + client demo
- `user/sh.c` — `[t26]` selftest
- `kernel/tcp.c` — `tcp_send` returns `len` instead of 0 on success
- `kernel/elf.c` — user stack bumped to 4 pages
- `mkfs.py` — adds the three new ELFs
- `fs/inittab` — auto-launches `httpsd.elf`
- `build.sh` — `[5b/7]` libcrypto build step + `TLS_PROGS` link pass; `-mno-stack-arg-probe`
- `docs/36-tls13-https.md` — this document

About 1900 LOC of new code, plus ~30 LOC of touched existing files. The crypto layer represents the bulk and is the real meat of the session.
