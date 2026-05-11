# Session 58 — RSA-PKCS#1 v1.5 sign and verify

**Goal:** add real RSA to AdventOS's libcrypto. The shape we ship:

- A **generic multi-precision integer** module — `libcrypto/bignum.{c,h}` — supporting add / sub / mul / long-division mod / modmul / right-to-left binary modpow, plus a binary GCD, extended-Euclidean modular inverse, and Miller-Rabin primality testing. 32-bit limbs, fixed 4096-bit buffers, no allocation.

- **RSA-PKCS#1 v1.5 signature scheme** with SHA-256 — `libcrypto/rsa.{c,h}` — implementing RFC 8017 §8.2 / §9.2: signature primitive `s = m^d mod n` (with CRT acceleration when `p, q, dp, dq, qinv` are populated), verification primitive `m' = s^e mod n`, plus byte-exact EM encoding for PKCS#1 v1.5 padding.

- **Key generation** with Miller-Rabin probable-prime sampling, parameterized on modulus size. RSA-2048 keygen is several minutes in QEMU; the selftest uses 512-bit for the roundtrip path and pre-computed openssl-signed vectors for the 2048-bit verify + byte-exact sign check.

- **`[t41]` selftest** — 7/7 PASS. Cross-verifies against openssl, exploits the determinism of PKCS#1 v1.5 to do a byte-for-byte comparison of our sign output vs openssl's. Full selftest count: **109 PASS, 0 FAIL**.

Sample output:

```
[t41] RSA: PKCS#1 v1.5 sign + verify (libcrypto/bignum + libcrypto/rsa)
  ---- rsatest.elf output ----
rsatest: starting
rsatest: modulus bits = 2048  (sig 256 bytes)
rsatest: msg = "AdventOS RSA test vector" (24 bytes)
rsatest: PASS  verify openssl-signed message
rsatest: PASS  our sig == openssl sig (byte-exact)
rsatest: sign elapsed ~19 sec  (CRT path, 2048-bit modulus)
rsatest: PASS  verify our own sig roundtrip
rsatest: PASS  tampered sig rejected
rsatest: PASS  wrong-message sig rejected
rsatest: generating fresh 512-bit RSA keypair
rsatest: keygen elapsed ~33 sec  (512-bit modulus)
rsatest: PASS  sign with fresh key
rsatest: PASS  verify fresh-key sig
rsatest: done
```

What's **not** in this session:

- **No protocol integration** — neither TLS (RSA cert chains) nor SSH (`ssh-rsa` / `rsa-sha2-256` host keys) is wired up. Both want extra DER + wire-format plumbing on top of the primitive; deferring to a separate session.
- **No RSA-PSS** (PKCS#1 v2.1). Modern, but PKCS#1 v1.5 is what real-world TLS cert chains and `ssh-rsa` host keys use, so this is the better one to start with.
- **No RSA encryption / decryption** (RSAES-OAEP). Orthogonal to signing; not needed for either of our protocols.
- **Not constant time.** The implementation leaks bits of `d` through branches and timing. Acceptable for a hobby OS that's the *only* RSA implementation on the machine and no untrusted code runs alongside. Do not deploy.

---

## 1. The bignum module

### Representation

```c
#define BN_MAX_LIMBS  144           /* room for 4096-bit values + 2× headroom */
typedef struct {
    uint32_t v[BN_MAX_LIMBS];       /* limbs, LSB at v[0] */
    int      n;                     /* significant limbs */
} bignum;
```

Two invariants the helpers preserve:

1. **`v[n..MAX]` is zero on every public-API return.** Lets `bn_cmp` etc. stop at `max(a->n, b->n)` without re-checking.
2. **`n` is trimmed** — the smallest count s.t. `v[n-1] != 0`. Equivalently: leading zero limbs are stripped. So `bn_is_zero` is `b->n == 0`.

The buffer is sized for RSA-2048 modular exponentiation: intermediate products of two 2048-bit numbers reach 4096 bits (128 limbs), and the long-division working `rem` needs a couple of slack limbs. 144 covers both cases.

### What's in the module

| Function | What it does |
|---|---|
| `bn_zero / bn_set_u32 / bn_copy` | Init |
| `bn_from_bytes_be / bn_to_bytes_be` | RFC-style big-endian byte conversion |
| `bn_bit_length / bn_byte_length / bn_bit_at / bn_is_odd / bn_is_zero` | Queries |
| `bn_cmp` | Three-way ordering |
| `bn_add / bn_sub / bn_lshift1 / bn_rshift1` | Basic arithmetic |
| `bn_mul` | Schoolbook `O(n²)` multiplication with 64-bit accumulator |
| `bn_mod` | Long-division remainder — bit-by-bit "shift, subtract if ≥ m" |
| `bn_modmul / bn_modpow` | Right-to-left binary modular exponentiation |
| `bn_gcd` | Stein's algorithm (binary GCD) |
| `bn_modinv` | Extended Euclidean with signed-magnitude tracking |
| `bn_is_prime` | Miller-Rabin with a small-prime trial-division filter |
| `bn_rand_bits` | Random bignum of exactly `bits` bits, forcing both MSB and LSB |

### Multiplication

Plain schoolbook, with a 64-bit accumulator to avoid the carry-propagation headaches of a 32-bit-only version:

```c
for (int i = 0; i < an; i++) {
    uint64_t carry = 0;
    for (int j = 0; j < bn; j++) {
        uint64_t prod = (uint64_t)a->v[i] * b->v[j] + r->v[i + j] + carry;
        r->v[i + j] = (uint32_t)prod;
        carry = prod >> 32;
    }
    r->v[i + bn] = (uint32_t)carry;
}
```

`O(n²)` in 32-bit limbs. For 64-limb (2048-bit) inputs that's ~4,096 limb-multiplies per `bn_mul`. Fast enough to keep RSA-2048 sign under a minute in QEMU on this single-threaded i386.

### `bn_mod`

Long division as a bit-loop. For each bit of `a` from MSB down:

1. `rem <<= 1`
2. OR in the next bit of `a`
3. If `rem >= m`, `rem -= m`

That's `bit_length(a)` iterations, each with a `bn_cmp` + `bn_sub`. For 4096-bit `a` and 2048-bit `m`, ~4,096 × ~64 limb operations = ~260K word ops. This is the inner hot path of `bn_modpow` and ends up dominating RSA sign runtime.

A real implementation would use Montgomery or Barrett reduction here for a 3–4× speedup. Out of scope for this session — the textbook version is honest about what it costs.

### `bn_modpow`

Right-to-left binary square-and-multiply:

```c
acc  = 1
b    = base mod m
for each bit of exp from LSB:
    if bit set: acc = (acc * b) mod m
    b = (b * b) mod m            /* skip the last unused square */
```

For a `k`-bit exponent: `k` squarings + average `k/2` multiplies — `~1.5k` modmuls. RSA verify with `e = 65537` (17 bits) is 26 modmuls, well under a second even in QEMU. Sign with the full 2048-bit `d` is ~3,072 modmuls — about a minute without CRT. With CRT it's two ~1024-bit modpows = ~3,072 modmuls on half-size numbers, where each modmul is ~4× cheaper. Net: ~8× speedup, hence the ~19 s rsatest measured.

### Miller-Rabin

For `n` to be tested:

1. Trial-divide through small primes (3..251). Catches the bulk of composites without spending a modpow.
2. Write `n - 1 = d · 2^s` with `d` odd.
3. For each of `rounds` random witnesses `a ∈ [2, n-2]`:
   - `x = a^d mod n`
   - If `x == 1` or `x == n-1`: witness passes, try next
   - Square `x` up to `s-1` times; if any iteration hits `n-1`, witness passes
   - Otherwise `n` is composite

At 8 rounds (the rsa_keygen default), false-positive rate is `< 4^-8 ≈ 2^-16` per non-prime tested, which compounds across all candidates we reject correctly. Good enough for a self-contained hobby OS. Real RSA implementations want 40+ rounds.

### `bn_modinv` (extended Euclidean)

This is the trickiest piece of bignum because of the signed coefficients. The standard formulation:

```
(g, s, t) = extgcd(a, m)   such that  a·s + m·t = g
if g != 1: no inverse exists
else: inverse = s mod m
```

We work with signed bignums by tracking a `neg` flag alongside each magnitude. The intermediate `s` and `s_new = old_s - q · s` can swing positive or negative, and we collapse the sign back at the end (`if neg: r = m - (s mod m)`).

The inline division-with-remainder loop inside `bn_modinv` (it needs the quotient, which `bn_mod` discards) is *almost* a `bn_div` — a follow-up refactor would extract that into its own helper.

---

## 2. The RSA module

### Encoded message (EM) per RFC 8017 §9.2

```
EM = 0x00 || 0x01 || PS || 0x00 || T
where T = DigestInfo (DER-encoded) || hash
```

For SHA-256, `DigestInfo` is 19 bytes:

```
30 31 30 0d 06 09 60 86 48 01 65 03 04 02 01 05 00 04 20
```

Plus 32 bytes of hash → `T_LEN = 51`. PS is `0xFF` repeated, filling `k - 3 - 51` bytes (where `k = byte length of modulus`). For RSA-2048: PS = 202 bytes.

The `em_encode` helper builds it directly into the caller's buffer; both sign and verify go through it (sign feeds it into the integer modpow, verify rebuilds it from the message and byte-compares against the modpow output).

### Signing (RFC 8017 §5.1.2)

Two paths, dispatched on whether `kp->p.n > 0`:

**Plain `d`-modpow (fallback):**
```c
s = m^d mod n
```
Single modpow on the full 2048-bit `n`.

**CRT-accelerated (preferred):**
```c
m1   = m^dp mod p           /* dp = d mod (p - 1) */
m2   = m^dq mod q           /* dq = d mod (q - 1) */
diff = (m1 - m2) mod p      /* careful: m1 < m2 means add p */
h    = qinv · diff mod p
s    = m2 + h · q
```

Two modpows on **half-size** numbers. With `bn_modmul`'s O(n²) cost in `n`-the-limb-count, doing two modpows on 32-limb (1024-bit) numbers is `2 × 32² = 2,048` limb-mults per modmul, vs `64² = 4,096` for one on 64-limb (2048-bit) numbers. Plus the long division underneath scales the same way. Net ~3–4× speedup; the 19-second sign-time the rsatest reports is for this path.

The recombination step is essentially free.

The `m1 - m2` underflow case is the only subtle bit — naively letting `bn_sub` fail and skipping the recovery would silently mis-sign roughly half the time.

### Verifying (RFC 8017 §8.2.2)

```c
if (sig_len != k)              return -1;  /* size mismatch */
if (s as integer >= n)         return -1;  /* RFC §5.2.2 */
m' = s^e mod n
em' = m' as k bytes big-endian
em  = em_encode(msg, k)
if (em != em')                 return -1;
return 0;
```

We rebuild `em` from scratch using the same `em_encode` the signer used and byte-compare. Since PKCS#1 v1.5 padding is deterministic, equal EMs ↔ equal signatures ↔ valid (modulo the modpow itself).

### Key generation

Standard textbook procedure:

```
generate p:  random `bits/2`-bit candidate; force odd, MSB-set;
             retry until Miller-Rabin passes
same for q  (with p != q)
n   = p · q
phi = (p-1) · (q-1)
e   = 65537                  /* fixed */
if e divides phi: retry      /* gcd(e, phi) must be 1 */
d   = e^-1 mod phi           /* via bn_modinv */
dp  = d mod (p-1)
dq  = d mod (q-1)
qinv = q^-1 mod p
```

Loop until everything succeeds. In practice the only failure path that retries is the rare `bn_modinv` rejection when `gcd(e, phi) != 1`; for random `p, q` this is essentially never hit.

The "swap p and q so p > q" step matters because `qinv = q^-1 mod p` requires `q < p` to be a unique element of `[0, p)`. Mathematically you could compute it either way, but our `bn_modinv` returns a result in `[0, m)`, so the convention has to be locked.

The "retry if `n` is bits-1 bits long" check guards against the rare-but-real case where both primes land near the bottom of their range, producing a modulus one bit short of the spec. Downstream code that hardcodes byte length (RFC-compliant PKCS#1 v1.5) would misframe.

---

## 3. The deterministic byte-exact sign check

The strongest test in [t41] is **Part B: "our sig == openssl sig (byte-exact)"**. Here's why it's stronger than "verify passes":

PKCS#1 v1.5 signing is **deterministic** — given the same `(msg, key)` it produces the same signature bit-for-bit. There's no random nonce (unlike ECDSA, which absolutely diverges from openssl across runs even when correct). So if our `rsa_sign_pkcs1_sha256` and openssl's both run on the same key + message, the output must be **byte-identical**.

If they differ, *something* is wrong:

- EM padding off by one byte → completely different integer goes into the modpow → completely different sig
- CRT recombination wrong → off-by-`q` errors, sometimes verifiable, never byte-exact
- modpow accumulator misordering → numerically wrong sig
- Sub-bug in `bn_sub` underflow during `m1 - m2` → ~50% of sigs differ

By cross-checking against openssl as the oracle, we catch all of these without having to be smart about *which* one would be wrong. The first time we ran the test against an early-draft CRT path, this assertion was the canary that flagged a missing `p - tmp` underflow correction.

Test-vector generation pipeline:

```bash
openssl genrsa -out test.key 2048
echo -n "AdventOS RSA test vector" > msg.bin
openssl dgst -sha256 -sign test.key -out sig.bin msg.bin
openssl rsa -in test.key -text -noout > keydump.txt
python3 tools/dump_rsa.py keydump.txt sig.bin libcrypto/rsa_testvec.h
```

The `dump_rsa.py` helper parses `openssl rsa -text` output (regex-extracts `modulus:` / `prime1:` / etc. blocks of hex), strips leading sign-bytes, and emits a C header with `static const uint8_t` arrays.  Committed under `tools/` so the vector is reproducible — anyone can regenerate it with a different key if they want a different test.

---

## 4. Performance numbers

Measured on QEMU i386, single CPU, no SIMD:

| Operation                | Time      |
|--------------------------|-----------|
| RSA-2048 sign (CRT)      | ~19 s     |
| RSA-2048 verify (e=65537)| < 1 s     |
| 512-bit keygen           | ~33 s     |

The verify path is fast enough to wire into protocols today (TLS cert validation, ssh-rsa host-key check on connect). The sign path is feasible for one-off operations (host-key generation at first boot) but too slow for high-frequency signing on this hardware.

For comparison, the existing Ed25519 sign + verify both run in under a second. **RSA is just slower** at every size — the algorithm has a fundamentally higher constant factor in its hot loop than EdDSA's variable-time scalar mult.

If we wanted to make this practical, the order would be:

1. **Montgomery reduction** instead of long-division mod. Big single win on `bn_mod` cost.
2. **Comba multiplication** for `bn_mul` (cuts down register pressure / spill).
3. **Sliding-window modpow** with precomputed odd powers.

Together these would get RSA-2048 sign down to ~1 second range and verify down to milliseconds. None are essential to *correctness*, which is the deliverable for this session.

---

## 5. Touched files

- `libcrypto/bignum.{h,c}` — new. Multi-precision integer arithmetic.
- `libcrypto/rsa.{h,c}` — new. PKCS#1 v1.5 sign / verify / keygen.
- `libcrypto/rsa_testvec.h` — new. Generated from openssl-signed message via `tools/dump_rsa.py`. Embedded into `rsatest.elf`.
- `tools/dump_rsa.py` — new. Test-vector regenerator.
- `user/rsatest.c` — new. Test program covering verify / sign / negatives / keygen.
- `build.sh` — `rsatest` added to TLS_PROGS so it links against libcrypto.
- `mkfs.py` — `rsatest.elf` added to USER_PROGRAMS.
- `user/sh.c` — `[t41]` selftest forks rsatest.elf and greps for the seven PASS lines.

## 6. Out of scope (deferred)

- **TLS RSA certificate path**: `x509.c` has `x509_build_self_signed_p256`; we'd add `x509_build_self_signed_rsa2048` plus a `tls_handshake_cert` branch that signs `CertificateVerify` with `rsa_pkcs1_sha256` (sig_alg 0x0401). Distinct from the primitive but obvious bolt-on once we want it.

- **SSH `ssh-rsa` / `rsa-sha2-256` host keys**: sshd would add the algorithm to its KEXINIT name-list, K_S would encode `string("ssh-rsa") || string(n_mpint) || string(e_mpint)`, and the signature blob would be `string("rsa-sha2-256") || string(sig:256)`. Persistence story is the same as ed25519's `/etc/ssh_host_key`, just with a 1.6 KB private-key serialization (n, e, d, p, q) instead of a 32-byte seed.

- **PSS** (PKCS#1 v2.1) — modern but not what real-world cert chains use today. Lower-priority than v1.5.

- **Constant-time / side-channel-free** — would require rewriting `bn_modpow` to do every bit of the exponent regardless of whether it's set, and every modmul to take the same number of cycles. Big rewrite; deploy-grade.

Next plausible session: RSA-signed TLS server cert end-to-end, so a vanilla `curl https://...` against AdventOS's httpsd works with the RSA path the rest of the world uses by default.
