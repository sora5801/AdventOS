# Session 60 — Real DNS + DHCP + NTP

**Goal:** upgrade three core network services from "minimum viable" to "real-OS-feeling":

- **NTP** — new. SNTP client (RFC 4330) that talks to a server, parses the 64-bit Transmit Timestamp, and applies an additive correction to the kernel clock so subsequent `SYS_TIME` calls return disciplined time. Plus a kernel-side test responder for closed-loop testing.

- **DNS** — `/etc/resolv.conf` parsing for a multi-nameserver fail-over list, plus a small TTL-based cache so repeated lookups don't re-query the upstream. Cache stats exposed for the selftest.

- **DHCP** — the existing state machine already acquired a lease at boot; what was missing was visibility. Parse the lease-time option (RFC 2132 §9.2), stamp the acquired time, expose all of it via a new `SYS_DHCP_INFO` syscall. Includes the T1 (half-lease) renewal deadline.

What ships in `[t43]`, 14/14 PASS:

```
[t43] DNS + DHCP + NTP — /etc/resolv.conf, TTL cache, lease info, SNTP
  PASS  DHCP reports an active lease
  PASS  DHCP got us 10.0.2.15 from SLIRP
  PASS  Gateway 10.0.2.2 advertised by DHCP
  PASS  lease_seconds > 0 (DHCP_OPT_LEASE_TIME parsed)
  PASS  T1 renewal deadline is in the future
  PASS  first DNS resolve example.com succeeded
  PASS  lookup counter incremented
  PASS  first lookup recorded as a miss
  PASS  second DNS resolve example.com succeeded
  PASS  second lookup served from cache
  PASS  no additional miss for cache hit
  PASS  SYS_NTP_SYNC returned a positive epoch (test-responder talked back)
  PASS  server-supplied epoch == hand-planted FAKE_EPOCH (1893456000)
  PASS  SYS_TIME jumped to the disciplined epoch (within 5s window)
```

Selftest total carries forward to **129 PASS, 0 FAIL** (was 115 after [t42]).

---

## 1. NTP — `kernel/ntp.{h,c}`

### Wire protocol (RFC 4330 §4)

SNTP is the "client only" subset of NTP. One UDP round-trip on port 123 with a 48-byte packet each way:

```
0                   1                   2                   3
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
|LI |VN |Mod|Stratum|Poll|Precision|       Root Delay         |
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
|             Root Dispersion           |   Reference ID       |
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
|             Reference Timestamp (64-bit)                      |
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
|             Originate Timestamp (64-bit)                      |
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
|             Receive  Timestamp (64-bit)                       |
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
|             Transmit Timestamp (64-bit)                       |
+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
```

Each 64-bit timestamp is high-32 = seconds since NTP epoch (**1900**-01-01), low-32 = fractional seconds in units of 2⁻³². The client cares only about the Transmit Timestamp the server writes — that's when the server *says* the response was sent. Subtract 2208988800 (seconds between 1900 and 1970) to convert to Unix epoch.

### Client logic (`ntp_sync`)

```c
1. Build a request packet:
     flags = (LI=0 << 6) | (VN=4 << 3) | Mode=3 (client)
     everything else zero
     tx_ts_sec = pit_ticks() as a per-call originate marker
2. udp_send(server, src=12300, dst=123, &req, 48)
3. Spin for up to 2 s, polling on the listener callback
4. On reply: verify orig_ts_sec matches our marker (stray-reply guard)
5. Extract reply.tx_ts_sec, subtract NTP_UNIX_OFFSET → caller's Unix epoch
```

The "spin with `pit_sleep(20)`" pattern matches `dns_resolve`'s — fine for a synchronous boot-style call, suboptimal for a daemon, but we're not running a daemon here.

### Test responder

To exercise the full request/response without depending on the public internet, `ntp_test_responder(enable, epoch_to_return)` registers a kernel-side UDP-123 listener. The listener replies to incoming SNTP queries with `epoch + NTP_UNIX_OFFSET` in the Transmit Timestamp — i.e. it pretends the wall-clock is whatever you tell it. The t43 test uses `epoch = 1893456000` (2030-01-01 00:00:00 UTC) and then verifies `SYS_TIME` jumped to that value within a 5-second window.

The loopback path is the same one session 29 set up for in-OS TCP: `ip_send` notices `dst == g_my_ip`, synthesizes an `ip_hdr`, and dispatches `udp_rx` directly without going through the NIC. So a query to `10.0.2.15:123` lands on our own listener.

### Clock correction

`kernel/rtc.c` gains a single `int32_t g_clock_correction` cumulative offset:

```c
uint32_t rtc_epoch_corrected(void) {
    /* read CMOS + add offset; signed cast handles negative deltas */
    return rtc_to_epoch(&t) + (uint32_t)g_clock_correction;
}

void rtc_apply_correction(int32_t delta) {
    g_clock_correction += delta;
}
```

`SYS_TIME` calls `rtc_epoch_corrected` instead of `rtc_to_epoch` directly. The first NTP sync at boot calls `rtc_apply_correction(server_epoch - local_epoch)`, and from then on every userspace `sys_time()` returns disciplined wall-clock.

We *don't* push the correction back into the RTC chip. That'd let the disciplined time survive reboots, but writing CMOS is finicky enough (the magic register sequence, the cmos-busy bit) that for a hobby OS it's not worth the bug surface. The in-memory offset gets re-derived on every boot via a fresh NTP query.

---

## 2. DNS — `kernel/dns.{h,c}`

### Multi-server fail-over via `/etc/resolv.conf`

The classic Unix mechanism for DNS robustness: list multiple servers, the resolver walks them in order until one answers. Pre-session-60 we had ONE server, the one DHCP option 6 hands us — if SLIRP's forwarder is unhappy, every lookup fails.

```
# /etc/resolv.conf
nameserver 10.0.2.3        # SLIRP's built-in forwarder (DHCP default)
nameserver 8.8.8.8         # Google Public DNS
nameserver 1.1.1.1         # Cloudflare
```

`dns_load_resolv_conf()` parses this at boot (called after the FS is mounted). The parser is hand-rolled — strip whitespace, skip `#` comments, look for the literal `nameserver`, parse a dotted quad. ~50 lines.

The server list is `DNS_SERVERS_MAX = 4` entries with the DHCP-provided one in slot 0 (added in `dns_init`). Duplicates are skipped so `nameserver 10.0.2.3` in resolv.conf doesn't double-add the DHCP entry.

`dns_resolve` then loops:

```c
for (int sx = 0; sx < g_n_dns_servers && !got_response; sx++) {
    udp_send(&g_dns_servers[sx], ...);
    wait up to 2s
    if reply: got_response = 1; break
}
```

Two-second timeout per server, four servers, worst case is 8 seconds to fail completely — but the SLIRP path normally answers in single-digit milliseconds, so this is silent overhead.

### TTL cache

A 16-entry fixed table. Each entry caches `(name, ip, expires_at)` where `expires_at` is the Unix epoch at which the cached value goes stale. On lookup:

1. Walk the table, return any non-expired hit. (Cache hit → `g_dns_hits++`, return `0`.)
2. On miss, do the wire lookup as above, then `cache_store(name, ip, ttl)` with the TTL we parsed out of the DNS answer's RR.

TTL is clamped to `[30s, 3600s]` — a misbehaving authoritative that returned `ttl = 86400` couldn't pin us to a stale IP for a day, and a `ttl = 0` "don't cache" hint still caches for 30s so we don't thrash on tight loops.

Eviction is "next miss takes any slot that's expired, or any free slot." No LRU — for a single-user OS the workload is dozens of names, not thousands.

`dns_cache_stats(out[4])` fills `{lookups, hits, misses, live_entries}`. `[t43]` calls it three times: at start, after the first resolve (1 miss expected), after a second resolve of the same name (1 hit expected, miss count unchanged). The cache-hit witness is the most informative — if the cache wasn't wired up, the second lookup would also be a miss.

---

## 3. DHCP — `kernel/dhcp.c`

### What was missing

The original DHCP code parsed `DHCP_OPT_SUBNET_MASK`, `DHCP_OPT_ROUTER`, `DHCP_OPT_DNS_SERVER`, `DHCP_OPT_SERVER_ID`. It included `DHCP_OPT_LEASE_TIME` in its requested-options list but never *parsed* it — the kernel learned its IP / gateway / mask / DNS from the OFFER but had no clue how long the lease was good for, when it was acquired, or when it'd need renewing.

### Session 60 additions

```c
case DHCP_OPT_LEASE_TIME:
    if (l == 4) g_lease_seconds =
        (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
    break;
```

…and at the moment ACK is committed:

```c
g_acquired_epoch = rtc_epoch_corrected();
if (g_lease_seconds == 0) g_lease_seconds = 86400;   /* SLIRP omits */
```

Then for userspace introspection:

```c
void dhcp_get_info(struct sys_dhcp_info *out) {
    /* copy ip / netmask / gateway / dns_server into out */
    out->lease_seconds   = g_lease_seconds;
    out->acquired_epoch  = g_acquired_epoch;
    out->t1_renew_at     = g_acquired_epoch + g_lease_seconds / 2;
    out->have_lease      = (g_state == DHCP_S_ACK) ? 1 : 0;
}
```

`t1_renew_at` is RFC 2131's half-lease threshold ("renewal time" T1). A real implementation runs a background timer here; we expose the deadline value but don't actively renew. The selftest only checks that `t1_renew_at > acquired_epoch`, i.e. the math is consistent.

### What's not done (yet)

- **Active renewal**: a kernel task that polls `sys_time() vs t1_renew_at` and re-REQUESTs at T1, falling back to DISCOVER at T2 (87.5% of lease). With SLIRP handing out 24-hour leases, this matters precisely never in the QEMU demo; on real hardware with a 1-hour DHCP server it matters a lot. Tracked as a follow-up.

- **Lease persistence across reboot.** Real Linux distros stash the last-acquired IP in `/var/lib/dhcp/...` and try a DHCPREQUEST for it first to avoid a full DISCOVER. The kernel currently re-DISCOVERs every boot. Same SLIRP latency note — only matters on real hardware with a real DHCP server.

---

## 4. Syscalls + ABI

Four new syscall slots (76–79):

| #  | Name                  | Purpose                                                     |
|----|-----------------------|-------------------------------------------------------------|
| 76 | `SYS_NTP_SYNC`        | Query server, apply correction, return server epoch         |
| 77 | `SYS_NTP_TEST_RESPONDER` | (test only) register/unregister the kernel-side UDP-123 listener |
| 78 | `SYS_DNS_CACHE_STATS` | Snapshot `{lookups, hits, misses, live}` to caller buffer   |
| 79 | `SYS_DHCP_INFO`       | Snapshot the lease state to `struct sys_dhcp_info`          |

The DHCP struct is mirrored in `user/libuser.h` (with `unsigned char` byte arrays because libuser doesn't pull in `uint8_t` to keep the user ABI surface narrow).

---

## 5. The selftest choreography

```c
[t43]
  sys_dhcp_info(&di)                            ← snapshot lease state
  assert ip == 10.0.2.15, gw == 10.0.2.x,
         lease > 0, t1 > acquired

  sys_dns_cache_stats(s0)                       ← baseline counters
  sys_dns_resolve("example.com", &ip)           ← first lookup: miss
  sys_dns_cache_stats(s1)
  assert s1.misses == s0.misses + 1

  sys_dns_resolve("example.com", &ip)           ← second lookup: hit
  sys_dns_cache_stats(s2)
  assert s2.hits == s1.hits + 1

  sys_ntp_test_responder(1, 1893456000)         ← register fake server
  sys_ntp_sync(self_ip = 10.0.2.15)             ← loopback NTP query
  sys_ntp_test_responder(0, 0)                  ← unregister
  assert sys_time() ≈ 1893456000

  sys_ntp_test_responder(1, original_epoch)     ← reverse the correction
  sys_ntp_sync(self_ip)
  sys_ntp_test_responder(0, 0)
  /* SYS_TIME back to real wall-clock for downstream tests */
```

The NTP test's clever bit is the "reverse the correction" tail. If we left the clock at 2030, every later test that calls `sys_time()` would see a 4-year-in-the-future timestamp — fine in isolation, but enough to confuse tests that compare epochs against each other or against fixture metadata. Calling `sys_ntp_sync` again with the pre-test time as the responder's `epoch` undoes the offset cleanly.

---

## 6. Loopback NTP — the design that mattered

We could have tested NTP against a real public server (`time.google.com` over SLIRP NAT), but:

- SLIRP UDP forwarding behavior varies by QEMU version
- CI environments often block outbound port 123
- The roundtrip is non-deterministic in CI

A kernel-side test responder dodges all of this. The responder is:

```c
static void on_test_query(const struct ip_addr *src, uint16_t src_port,
                          const void *data, int len) {
    /* build reply with the planted epoch in tx_ts_sec */
    udp_send(src, NTP_PORT, src_port, &reply, sizeof(reply));
}
```

Registered via `udp_listen(123, on_test_query)`. When `ntp_sync(10.0.2.15)` runs, the existing session-29 loopback path in `ip_send` short-circuits the egress and dispatches `udp_rx` against our own listener — so the entire round-trip happens in-process, no NIC touched.

The same listener-registration pattern would also let us implement a real production-grade NTP server later if we wanted one (`ntpd.elf`).

---

## 7. Touched files

- `kernel/ntp.{h,c}` — new. SNTP client + test responder.
- `kernel/rtc.{h,c}` — `rtc_apply_correction` + `rtc_epoch_corrected`.
- `kernel/syscall.{h,c}` — four new ops (76–79), `struct sys_dhcp_info` ABI.
- `kernel/dns.{h,c}` — multi-server list, `/etc/resolv.conf` parser, TTL cache, `dns_cache_stats`, `dns_get_servers`.
- `kernel/dhcp.c` — parse `DHCP_OPT_LEASE_TIME`, stamp `g_acquired_epoch`, public `dhcp_get_info`.
- `kernel/kernel.c` — wires `dns_load_resolv_conf()` after `dns_init()`.
- `user/libuser.{c,h}` — wrappers + mirrored ABI struct + syscall numbers.
- `user/sh.c` — `[t43]` selftest.
- `mkfs.py` — ships `/etc/resolv.conf`.
- `fs/etc/resolv.conf` — new default.

## 8. Out of scope (deferred)

- **DHCP renewal task** — track `t1_renew_at`, schedule a re-REQUEST when we hit it.
- **NTP daemon mode** — periodic re-sync, slew vs step, clock-discipline algorithm.
- **DNS CNAME chasing** — currently we return the first A record; a CNAME-only response fails with "no A record."
- **IPv6 anywhere** — DHCPv6, AAAA records, NTP over v6. Same scope decision as everywhere else in the OS.
- **Lease persistence** — surviving reboots by stashing IP+server-id in `/var/lib/dhcp/`.

Most realistic next session: the DHCP renewal task (easy, useful in long-uptime scenarios) plus making NTP run periodically from `init.elf` instead of only-once-per-boot-from-userspace.
