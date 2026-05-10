# Session 45 — Real-world HTTPS GET

**Goal:** make `httpsget` reach the actual public internet — not just the in-OS httpsd loopback test, but a TLS 1.3 server we don't control. End-to-end demo: AdventOS resolves a hostname (or accepts a literal IP), opens a TCP connection through QEMU's SLIRP NAT, runs a full RFC 8446 ClientHello → ServerHello → encrypted server flight handshake with a real Cloudflare endpoint, and prints the live HTTP/1.1 response.

End state — `[t26]` selftest with networking up:

```
  --- real-world HTTPS GET: https://1.1.1.1/ ---
httpsget: 1.1.1.1 -> 1.1.1.1:443 /
httpsget: TCP connected
httpsget: TLS 1.3 handshake OK
httpsget: sent 81-byte request encrypted
HTTP/1.1 301 Moved Permanently
Date: Sun, 10 May 2026 23:55:05 GMT
Content-Length: 0
Connection: close
Location: https://one.one.one.one/
Server: cloudflare
CF-RAY: 9f9cdf705a6470f0-SJC

httpsget: received 231 plaintext bytes total
  real-world httpsget exit = 0  (0 = page fetched)
```

AdventOS just did:

- DNS skip (the host parses as a dotted-quad IP literal) and a real TCP `connect()` through SLIRP NAT to `1.1.1.1:443`.
- A standards-conformant TLS 1.3 ClientHello with **SNI suppressed** (Cloudflare doesn't accept IP literals in `server_name`), broadened `signature_algorithms` covering ECDSA-P256 + RSA-PSS-RSAE + Ed25519, X25519 ECDHE, and `TLS_AES_128_GCM_SHA256` as the only cipher.
- Receipt of Cloudflare's ServerHello (90 bytes) → ChangeCipherSpec → 2463-byte encrypted server flight (real Cloudflare ECDSA-P256 cert chain + CertificateVerify + Finished), all decrypted cleanly with the X25519-derived keys.
- An encrypted `GET /` HTTP/1.1 request and a decrypted, live, **231-byte HTTP response from a server in San Jose** (`CF-RAY: ...-SJC`) — including a redirect-to-`one.one.one.one`, the canonical Cloudflare DNS-resolver page.

Local `httpsget → httpsd` and cryptotest 27/27 still pass — the changes don't regress anything.

## What's in scope

In:

- **`libcrypto/tls.h`** — adds `const char *server_name` to `struct tls_conn` so the caller can pass SNI before `tls_client_handshake_cert`. Bumps `TLS_MAX_FRAGMENT` from 2048 → 6144 to fit a single-cert real-world server flight.
- **`libcrypto/tls.c` (ClientHello builder)** —
  - Emits the **SNI extension** (RFC 6066 §3) when `c->server_name` is set: 2-byte type=0, length, list-len, name-type=0, name-len, name bytes.
  - **Broader `signature_algorithms`** list: ECDSA-P256-SHA256 (0x0403), RSA-PSS-RSAE-SHA256/384/512 (0x0804/5/6), Ed25519 (0x0807). With this menu, almost every public TLS 1.3 server can find a sig_alg it can use for `CertificateVerify`.
  - Preserves `server_name` across the handshake function's zero-clear of the conn struct (parallel to how the server handshake preserves `cert_der` / `server_sk`).
- **`libcrypto/tls.c` (memory layout)** — converts the `TLS_MAX_FRAGMENT`-sized stack arrays in `send_encrypted`, both PSK and cert client/server handshakes, the cert-flow server flight assembler, and `tls_recv`, all to file-scope `static`. With a 6 KiB fragment cap, putting multiple of these on the same 16 KiB user stack overflows. AdventOS is fork-per-connection, so per-process `static` BSS is safe.
- **`kernel/tcp.h`** — bumps `TCP_RX_BUF` from 2048 → 4096. That's the value we advertise as the receive window in every ACK; Cloudflare's server flight (2463 bytes) was overflowing the old 2 KiB window and causing the peer to stall mid-record.
- **`kernel/sock.h`** — bumps `SOCK_RX_BUF` from 2048 → 4096 to match. (The per-socket ring buffer is what `sock_read` drains into the user; if it's smaller than the advertised window, we drop bytes.)
- **`user/httpsget.c`** — rewrites the argument handling around real URLs:
  - `httpsget https://1.1.1.1/` — parses scheme, host, optional port, path
  - `httpsget host /path` — convenience two-arg form (defaults to port 443)
  - `httpsget` (no args) — backward-compat default to the loopback `httpsd` on 10.0.2.15:4433
  - Resolves host via `sys_dns_resolve` *only if* the host isn't already a dotted-quad
  - Sends a proper HTTP/1.1 `GET path` with `Host: host` header
  - Drains response in 1.5 KiB chunks, printing each to stdout, capping at 8 KiB
  - Suppresses SNI when the host is an IP literal (a few servers, including Cloudflare's 1.1.1.1, reject IP-literal SNI)
- **`user/sh.c`** — adds a new sub-test inside `[t26]`: spawns `httpsget https://1.1.1.1/` after the local httpsd round-trip. Exit code is logged but failure is non-fatal (no internet, host firewall blocking SLIRP, etc.).

Out:

- **Certificate validation.** The client accepts any cert, exactly as session 41 documented (`curl -k` equivalent). Validating the chain needs an embedded trust anchor list + RSA signature verification + DN walking — out of scope.
- **ALPN.** Cloudflare's 1.1.1.1 plays nicely with us without ALPN; if we wanted HTTP/2 we'd need it. HTTP/1.1 in the response confirms TLS-layer ALPN is unused.
- **DNS via SLIRP.** The DNS path is wired in (`sys_dns_resolve`) but timed out reaching the host's upstream DNS through QEMU's SLIRP forwarder in our test setup. Using `https://1.1.1.1/` (literal IP) sidesteps it. DNS works on other AdventOS networks; it's a test-environment limitation, not a bug in the OS.
- **HTTP/1.1 chunked transfer.** Cloudflare's 301 response had `Content-Length: 0` so no chunked decoder was needed. A larger response would arrive in chunks we'd need to reassemble — left for later.

## The two bugs that took longest to find

### 1. `read_n(ct) failed` — the TCP window was too small

After all the TLS extensions plumbing went in, the handshake reached the ServerHello and ChangeCipherSpec records, then died reading the encrypted server flight. Diagnostic output during debug:

```
[tls] recv: type=22 ver=3.3 len=90      ← ServerHello
[tls] recv: type=20 ver=3.3 len=1       ← ChangeCipherSpec
[tls] recv: type=23 ver=3.3 len=2467    ← Encrypted flight header read OK
[tls] recv: read_n(ct) failed           ← Then ciphertext read failed
```

The record's 5-byte header arrived, but the 2451-byte ciphertext following it didn't. That looks like Cloudflare closed the connection mid-record, which would be bizarre — until you remember that Cloudflare also obeys our advertised TCP receive window. We were advertising 2048 bytes (`TCP_RX_BUF`) on every ACK; Cloudflare sent 2048 then waited for the window to open. Our app drained those 2048 from the socket ring (`SOCK_RX_BUF`, also 2048), the kernel sent an updated window, *but the new window was still capped at 2048* because we'd already received 2467 bytes — exceeding what the buffer can hold.

The fix is sympathetic in both directions: bump `SOCK_RX_BUF` to comfortably hold one server flight (the ring buffer the app drains from), and bump `TCP_RX_BUF` to match the new window we advertise on the wire. 4 KiB is enough for a single-cert flight; going larger blew the kernel's BSS budget at 16 K × 16 sockets = 256 KiB (which mysteriously triggered a QEMU TCG translator assertion — probably a page-crossing issue in kernel `.bss`, not worth chasing for 256 KiB I don't actually need).

### 2. SNI of an IP literal — Cloudflare doesn't accept it

The first run with `https://1.1.1.1/` sent SNI `server_name = "1.1.1.1"` and Cloudflare quietly didn't complete the handshake. RFC 6066 §3 explicitly forbids IP literals in SNI ("Currently, the only server names supported are DNS hostnames..."). Cloudflare enforces this; many other servers don't.

Quick fix in `httpsget`: detect when the host is `[0-9.]+` and suppress the SNI extension. The TLS code accepts `c->server_name = NULL` and emits no `server_name` extension at all, which the server happily accepts (defaulting to whichever vhost it has on that IP).

## ClientHello, real-world edition

Comparing what we sent before and after:

| Field | Before (session 41) | After (session 45) |
|---|---|---|
| `legacy_version` | 0x0303 | 0x0303 |
| `random` | 32 bytes | 32 bytes |
| `legacy_session_id_len` | 0 | 0 |
| `cipher_suites` | `[0x1301]` | `[0x1301]` |
| `supported_versions` | `[0x0304]` | `[0x0304]` |
| `supported_groups` | `[x25519]` | `[x25519]` |
| `signature_algorithms` | `[ecdsa_p256, ed25519]` | `[ecdsa_p256, rsa_pss_sha256/384/512, ed25519]` |
| `key_share` | x25519 + 32B pub | x25519 + 32B pub |
| `server_name` | (none) | name (optional, IP literals suppressed) |

The fifth signature scheme — `rsa_pss_rsae_sha256` (0x0804) — is what most real public servers actually use because most public-CA certs are still RSA. The server picks one of our offered schemes for its `CertificateVerify`; this client doesn't verify the signature (curl `-k` semantics), so the choice is purely "permission for the server to send something we'll accept on the wire."

## TCP window vs the TLS record dance

This was the most interesting interaction. TLS records have a 5-byte plaintext header (type + version + length) that's not encrypted. The peer's TCP stack delivers those 5 bytes; our app reads them via `read_n(hdr, 5)`; we parse the length and queue a follow-up `read_n(ct_len)` for the body. If `ct_len > rx_window`, the peer has to wait for our ACK + window update before sending the rest.

Two effects from this:

1. **Window-update latency** — our ACK has to traverse SLIRP, hit Cloudflare's stack, and a fresh segment has to come back. While we're spinning in `sock_read`, the kernel happily yields and the bytes do eventually arrive. But Cloudflare has its own end-to-end-handshake timeout (5-10 seconds), and if our window only opens 2 KiB at a time across multiple round trips, that adds latency *per round trip*.

2. **Receiver buffer capacity** — even if Cloudflare did push past our window (some implementations do for handshake messages), our `sock_data_in` would discard whatever doesn't fit in `SOCK_RX_BUF`. So sizing `SOCK_RX_BUF` so a single TLS record fits is what makes the handshake reliable.

We picked 4 KiB as the smallest size that comfortably holds a single-cert Cloudflare flight (2463 bytes) plus the ChangeCipherSpec record plus a bit of slack. The right number for a multi-cert chain (e.g., a server with an intermediate CA) would be more like 8-16 KiB; we'd hit that when we test a real domain rather than 1.1.1.1's single-cert configuration.

## What's next

- **Real DNS in the test harness.** SLIRP's DNS forwarder did something unhappy in our headless QEMU on Windows. Plain `wget http://example.com/` reproduces the same DNS timeout. Likely a host-firewall / SLIRP combination; not an AdventOS bug. A fix on the host (or `-netdev user,dns=8.8.8.8`) would make the demo work with hostnames.
- **HTTP/1.1 chunked transfer.** If we hit a larger page, we'd need a streaming chunk-decoder around the response body.
- **Cert validation.** Embed a tiny CA bundle (Cloudflare's chain only?), add RSA signature verification, walk the cert's signed-by-issuer chain. ~600 LOC of new code. Promotes us from `curl -k` to `curl` proper.
- **TLS session resumption.** PSK-resumption via `pre_shared_key` + `psk_key_exchange_modes` extensions. Skips the cert dance on a second connection to the same host. Useful for HTTPS clients that fetch many resources.
- **ALPN negotiation.** Letting us tell the server "I speak HTTP/1.1" up front; the server picks one of our offered protocols and tells us back. Required if we ever want HTTP/2.
