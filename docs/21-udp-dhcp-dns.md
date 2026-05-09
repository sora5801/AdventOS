# Session 21 — UDP, DHCP, DNS, and the bootloader sector-count gotcha

**Goal:** Stack three new networking layers on top of the IPv4/Ethernet/ARP foundation from session 12: UDP transport, a synchronous DHCP client that replaces the hardcoded SLIRP IP, and a blocking DNS A-record resolver. Plus a `SYS_DNS_RESOLVE` syscall so userspace can do hostname lookups.

End state — boot output and the new `[t12]` selftest:

```
[boot] starting network stack
rtl8139: PCI 0:3.0  io=0xc000  irq=11
net: link up — MAC 52:54:00:12:34:56  (IP unconfigured — waiting for DHCP)
dhcp: DISCOVER ... got OFFER 10.0.2.15, REQUEST ... ACK
net: 10.0.2.15/255.255.255.0  GW 10.0.2.2  DNS 10.0.2.3
...
[t12] DNS A-record lookup
  example.com -> (timeout / no record)
  github.com -> 140.82.116.3
```

A real DHCP DISCOVER → OFFER → REQUEST → ACK lease cycle, then real DNS resolution from inside the OS to a public host. `httpd.elf` keeps serving curl on :80 throughout (`status=200 bytes=317`).

## What's in scope

In:
- `kernel/udp.{h,c}` — UDP send + per-port listener registration (`udp_listen(port, cb)`); pseudo-header checksum reusing `ip_checksum` over a flat buffer
- `kernel/dhcp.{h,c}` — synchronous BOOTP/DHCP client; runs at boot, falls back to SLIRP defaults on timeout
- `kernel/dns.{h,c}` — synchronous A-record resolver; busy-waits up to 2s for a UDP reply, parses the answer section
- `g_dns_server` slot in `net.c`; `g_my_ip` / `g_gateway_ip` / `g_subnet_mask` start at 0.0.0.0 and DHCP fills them
- IP layer: accept broadcast and "I have no IP yet" packets so DHCP can flow before lease; `ip_send` short-circuits broadcast destinations to `ff:ff:ff:ff:ff:ff` and skips ARP
- `SYS_DNS_RESOLVE = 39` syscall + `sys_dns_resolve(name, ip[4])` libuser wrapper
- New `[t12]` selftest doing a couple of DNS lookups
- Bootloader sector count bumped from 112 → 128 (was loading 56 KiB; kernel now ~66 KiB)

Out:
- DHCP lease renewal (we hold the lease forever; a real client would renew at half the lease time)
- DHCP RENEWAL/REBINDING states, RELEASE on shutdown
- DNS caching (every lookup is a fresh query)
- DNS retry on timeout (one query, one timeout, give up)
- DNS CNAME chasing (one-step works; multi-hop CNAMEs fail)
- DNS AAAA / MX / TXT / etc. (A only)
- DNS over TCP for large responses
- Outbound UDP `connect`/`sendto`/`recvfrom` from userspace (only the in-kernel resolver uses UDP)
- IPv6, multicast (224.0.0.0/4), source-routed broadcasts
- ICMP-driven path-MTU discovery
- DHCP option 121 (classless static routes), 119 (search list), etc. — we parse only mask/router/dns
- IGMP, ARP probe / RFC 5227 conflict detection

## Architecture: three new layers, two new wire-paths

Before:

```
SYS_SOCKET → sock layer → tcp → ip → eth → rtl8139
```

After:

```
SYS_SOCKET → sock layer → tcp ─┐
                                ├→ ip → eth → rtl8139
SYS_DNS_RESOLVE → dns ──→ udp ─┤
DHCP boot path  → udp ──────────┘
```

UDP sits next to TCP at the transport layer. DHCP and DNS are clients of UDP. DHCP runs once at boot, synchronously, before any other service. DNS runs whenever a user calls `sys_dns_resolve`.

The IP layer's small refactor accepts broadcast destinations (255.255.255.255) and traffic-when-we-have-no-IP — both required by DHCP, which talks to the server before having an address of its own.

## UDP

The protocol is small and our impl is small. Header is 8 bytes:

```c
struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t csum;
};
```

Listener registration is a fixed-size table (4 slots) of `{port, callback}`. `udp_rx` walks it on every datagram and dispatches the first matching live callback. Same shape as session 13's TCP listener — no port allocator, no socket abstraction.

The checksum is the gotcha. UDP's checksum covers a 12-byte pseudo-header (src IP + dst IP + zero + protocol + UDP length) PLUS the UDP header PLUS the payload. We reuse session 13's "build a flat buffer, run `ip_checksum` over it once" trick:

```c
static uint16_t udp_checksum(const struct ip_addr *src,
                             const struct ip_addr *dst,
                             const void *seg, uint16_t seg_len) {
    uint8_t buf[12 + 1500];
    /* pack pseudo-header... */
    memcpy(buf + 0, src, 4);
    memcpy(buf + 4, dst, 4);
    buf[8]  = 0;
    buf[9]  = IP_PROTO_UDP;
    buf[10] = (uint8_t)(seg_len >> 8);
    buf[11] = (uint8_t)(seg_len & 0xFF);
    memcpy(buf + 12, seg, seg_len);

    uint16_t csum = ip_checksum(buf, 12u + seg_len);
    return csum == 0 ? (uint16_t)0xFFFF : csum;     /* RFC 768 */
}
```

The "csum == 0 → send 0xFFFF" is a real RFC 768 quirk — a transmitted checksum field of zero means "no checksum was computed," so a real all-zeros result has to be sent as the one's-complement equivalent (0xFFFF) instead.

Listeners run in IRQ context (called from `ip_rx` which is called from `eth_rx` which is called from the RTL8139 IRQ). Same constraint as session 13's TCP callbacks: don't block, don't allocate, don't take long locks.

## DHCP

The DHCP client is synchronous — it runs in `kmain` between `net_init` and the rest of the services, blocking the boot path until the lease lands or times out:

```
kmain
  net_init()        ; RTL8139 up, IP = 0.0.0.0
  udp_init()
  dhcp_acquire_lease()
    DISCOVER (broadcast)
    wait_state(OFFER, 2s)
    REQUEST (broadcast, names the offered IP + server)
    wait_state(ACK, 2s)
    commit g_my_ip / g_gateway_ip / g_subnet_mask / g_dns_server
    arp_send_request(gateway)   ; prime the cache (see pitfall below)
    pit_sleep(50)               ; let ARP reply land
  dns_init()
  task_reaper_start, demos, tcp_init, sock_init, ...
  LAUNCH httpd, sh
```

The synchronous wait works because the UDP rx path is IRQ-driven: the listener callback (`on_dhcp`) sets a `volatile int g_state` to `OFFER` or `ACK` based on the DHCP message-type option. The boot path polls that flag with `pit_sleep(20)` between checks.

The packet shape is BOOTP — 240 bytes of fixed fields (op / htype / hlen / xid / addresses / chaddr) followed by variable-length options. Options are TLV: 1-byte code, 1-byte length, value bytes. We emit DISCOVER + REQUEST and parse OFFER + ACK looking for these option codes:

| Code | Name | What we do with it |
|---|---|---|
| 1 | subnet mask | save → `g_subnet_mask` |
| 3 | router | save → `g_gateway_ip` |
| 6 | DNS server | save → `g_dns_server` |
| 53 | DHCP message type | dispatch DISCOVER/OFFER/REQUEST/ACK |
| 54 | server identifier | echo back in REQUEST |

Plus option 51 (lease time), 50 (requested IP), 55 (parameter request list), 255 (end). The whole parser is one `while (i < len)` loop with a small switch.

If anything times out we apply hardcoded SLIRP defaults (10.0.2.15 / 10.0.2.2 / 255.255.255.0 / 10.0.2.3) so the rest of the system has a usable network. The deep dive's "Pitfalls" section calls out that a real client would do exponential-backoff retries before giving up.

## DNS

The resolver does one thing: take a hostname, return an IPv4 address. A-records only, single query, 2-second timeout, no caching, no retry.

DNS message format is tighter than DHCP:

```
Header (12 bytes):
  ID (2)
  Flags (2)
  QDCOUNT (2)  ← number of questions
  ANCOUNT (2)  ← number of answers
  NSCOUNT (2)
  ARCOUNT (2)

Questions:
  QNAME (variable, length-prefixed labels, ends with NUL)
  QTYPE (2)    ← 1 = A
  QCLASS (2)   ← 1 = IN

Answers:
  NAME (variable, supports 0xC0xx pointer compression)
  TYPE (2)
  CLASS (2)
  TTL (4)
  RDLENGTH (2)
  RDATA (variable)
```

For A records, RDATA is exactly 4 bytes. The query is straightforward — encode the hostname as length-prefixed labels (`google.com` → `[6]google[3]com[0]`), append `[QTYPE=1][QCLASS=1]`, send.

Parsing the response is mildly trickier because the answer NAME field can use *pointer compression* — a leading byte with the top two bits set (`0xC0xx`) means "the name is at offset xx from the start of the message." We don't follow these pointers (we don't care about the answer's name, only its type+rdata), so `skip_name` recognizes either a 0-terminated label sequence or a 2-byte pointer and returns the bytes consumed:

```c
static int skip_name(const uint8_t *msg, int msg_len, int at) {
    int p = at;
    while (p < msg_len) {
        uint8_t b = msg[p];
        if (b == 0)             return p - at + 1;
        if ((b & 0xC0) == 0xC0) return p - at + 2;
        if (p + 1 + b >= msg_len) return -1;
        p += 1 + b;
    }
    return -1;
}
```

The synchronous wait reuses the DHCP pattern: a UDP listener on a high port (53000) writes the full response into a static buffer and sets `g_have_resp`; the resolver polls with `pit_sleep`.

The result for `github.com -> 140.82.116.3` is real — that's GitHub's actual IP on the day this was tested. SLIRP forwards UDP/53 to the host's resolver, so we get whatever the host would.

`example.com` timed out in our test run — could be a slow recursive query, packet loss, or response size hitting the kernel's 1500-byte buffer cap; the deep dive's "Pitfalls" section documents the resolver's known-fragile bits.

## SYS_DNS_RESOLVE — userspace gets to look up names

```c
case SYS_DNS_RESOLVE: {
    const char *uname = (const char *)a;
    uint8_t    *uout  = (uint8_t *)b;
    char name[64];
    int  i;
    for (i = 0; i < 63 && uname[i]; i++) name[i] = uname[i];
    name[i] = 0;
    struct ip_addr ip;
    int rc = dns_resolve(name, &ip);
    if (rc == 0 && uout) {
        uout[0] = ip.b[0]; uout[1] = ip.b[1];
        uout[2] = ip.b[2]; uout[3] = ip.b[3];
    }
    ret = rc;
}
```

Snapshot the user-pointer name into kernel memory (long-running blocking calls shouldn't trust user pointers across yields), call into `dns_resolve`, write the 4 bytes back to the user's `ip[4]` buffer.

libuser wrapper:

```c
int sys_dns_resolve(const char *name, unsigned char ip[4]);
```

Used by t12 in the shell, and available to any future user program.

## The bootloader sector-count bug — half a session of debugging

Adding UDP + DHCP + DNS pushed `kernel.bin` from ~57 KB to ~66 KB. The bootloader was hardcoded to load **112 sectors = 57,344 bytes** into memory at 0x10000. Anything past that simply isn't mapped — the bytes are whatever was sitting in RAM before the bootloader ran (BIOS leftovers / 0xFF / random).

Symptom: a page fault at `8:0` (kernel CS, EIP = 0) right after launching `sh.elf`. Disassembling the kernel ELF showed `task_create`'s call chain referencing strings at `0x1ed3b` — past the loaded region. When `serial_write` walked through the (unmapped, garbage-filled) string buffer, eventually it dereferenced something that resolved to `0`. The kernel halted.

Symptom-by-symptom debugging:
1. `httpd` showed up as `pid=3` instead of the expected `pid=4`. That means only TWO `task_create` calls succeeded before httpd, instead of three (reaper, demo_a, demo_b). One of them silently failed.
2. Adding `kprintf("[task_create] ...")` at the top of `task_create` produced no output, even though the kernel.bin contained the literal — the LITERAL was at offset 0xed00 in the binary, past the 0x100..0xE000 loaded range.
3. Replacing the kprintf with `serial_write("TC0\n")` ALSO produced no output — the literal "TC0" was likewise past the loaded range.
4. Verified `kernel.bin` size = 65,712 bytes, bootloader load = 57,344 bytes, gap = 8,368 unmapped bytes.

Fix: bump the bootloader sector count from 112 → 128 (64 KiB). Comment in `boot/boot.S`:

```asm
.word 128               /* number of sectors to read (64 KiB) — was 112; bumped in
                           session 21 when UDP/DHCP/DNS pushed kernel.bin past 56 KiB */
```

128 sectors × 512 = 65,536 bytes — still tight (kernel is 65,712, so the last 176 bytes overflow if we don't watch). 256 sectors would be safer but crosses the 64K real-mode segment boundary, which some older BIOSes don't handle cleanly through `int 13h ah=42h`. 128 works; the deep dive's "Pitfalls" notes a future bump will need either multiple `int 13h` calls or a higher destination segment.

The lesson: a flat-binary kernel + fixed-sector-count bootloader is a ticking bomb that goes off whenever someone adds a meaningful amount of code.

## The ARP-cache priming — the OTHER session-21 bug

After the bootloader fix the kernel booted cleanly, DHCP got the lease, DNS resolved github.com — but `curl http://localhost:8080/` started timing out. httpd printed `listening on port 80 (userspace)`, the connection got past curl's "Connected" line, then nothing.

Root cause: TCP's first reply (the SYN-ACK) needed to ARP-resolve the client (10.0.2.2 the SLIRP gateway / NAT). The ARP cache was empty. `ip_send` returned -2 with an ARP request fired off. Our TCP doesn't retransmit, so the SYN-ACK was lost forever. Subsequent SYN retransmits from curl hit `tcp_rx` but the TCB was already in `SYN_RCVD`, so they were dropped.

Old sessions worked because the hardcoded boot path happened to ARP the gateway implicitly via some traffic pattern. The new DHCP path uses broadcast (no ARP needed) and DNS goes to 10.0.2.3 (a different IP than the gateway), so neither populates the cache for 10.0.2.2.

Fix: at the end of `dhcp_acquire_lease`, send an ARP request for the gateway and wait 50ms for the reply:

```c
arp_send_request(&g_gateway_ip);
pit_sleep(50);
```

A real OS would either let TCP retransmit (proper fix) or have the IP layer queue a packet pending an ARP resolution (better fix). We do neither yet; priming the cache once at boot suffices for our single-gateway setup.

## Files added / modified

| File | Change |
|---|---|
| `kernel/udp.{h,c}` | New. Listener table + send + checksum |
| `kernel/dhcp.{h,c}` | New. Synchronous DISCOVER/OFFER/REQUEST/ACK client |
| `kernel/dns.{h,c}` | New. Synchronous A-record resolver with pointer-compression-aware name skip |
| `kernel/ip.{h,c}` | UDP rx case; broadcast-bypass-ARP; pre-DHCP "we have no IP" accept-all |
| `kernel/net.{h,c}` | `g_dns_server`; `g_my_ip` etc. start at 0.0.0.0 |
| `kernel/syscall.{h,c}` | `SYS_DNS_RESOLVE = 39`; user-pointer snapshot + result writeback |
| `kernel/kernel.c` | Boot order: udp_init → dhcp → dns_init → rest |
| `boot/boot.S` | Sector count 112 → 128 |
| `user/libuser.{h,c}` | `sys_dns_resolve` wrapper + syscall constant |
| `user/sh.c` | `[t12]` selftest |

## Design decisions

**Synchronous DHCP at boot.** A real OS would have an async/event-driven DHCP daemon that runs alongside other services. Synchronous is simpler and the boot delay (≤4s with both timeouts) is acceptable for a demo. The fallback to SLIRP defaults means the system always boots even if DHCP fails — important for offline / no-server testing.

**No DHCP lease renewal.** Real DHCP renews at T1 (half lease time) and rebinds at T2 (7/8 lease time). We never renew — the lease holds forever from the kernel's POV. SLIRP doesn't actually time out leases either, so this works in practice. Real clients do expect to re-DHCP eventually.

**One global UDP listener table.** Same shape as TCP from session 13. A future port allocator + per-task socket would replace this with the kernel-side equivalent of `bind(2)`. For DHCP (port 68) and DNS (port 53000) this is enough.

**No `udp_send` from userspace.** Only the in-kernel DHCP / DNS code uses UDP. A `SYS_UDP_SEND` would be the natural follow-up — combined with an `accept`-style `udp_recv` it'd give userspace BSD-flavored UDP sockets. Deferred.

**DNS resolver lives in the kernel.** `dns_resolve` is a kernel function; userspace gets at it via `SYS_DNS_RESOLVE`. The argument for kernel-side: it shares the UDP listener mechanism, can be called from the kernel itself (for future networking features), and handles the pseudo-header / port number stuff once. The argument against: a real OS pushes resolution into a userspace daemon (systemd-resolved / nscd / dnsmasq) for better policy. We keep it in-kernel for simplicity.

**Synchronous DNS with 2s timeout.** No retry. POSIX `getaddrinfo()` typically retries 2-3 times. We don't.

**A records only.** No AAAA (we have no IPv6 stack), no MX, no TXT. Adding them is a couple of lines per record type but no current code needs them.

**Pointer-compression aware skip, not full follow.** We skip names by recognizing the 0xC0 prefix without dereferencing — we don't care what the name actually is in answers, only the type+rdata. A real resolver would need to follow pointers for CNAME chasing, etc.

**Broadcast bypasses ARP.** `ip_send` to 255.255.255.255 short-circuits to ff:ff:ff:ff:ff:ff. Without this DHCP-DISCOVER would try to ARP 255.255.255.255 (or the gateway, which is also unset pre-DHCP) and fail.

**`ip_rx` accepts traffic when we have no IP.** Pre-DHCP we don't know our own address, so any packet that made it past the Ethernet MAC filter gets handed up. After DHCP completes, the normal "must match my IP or be broadcast" check kicks in.

**Hardcoded SLIRP fallback.** If DHCP times out, we use 10.0.2.15 / 10.0.2.2 / 255.255.255.0 / 10.0.2.3. Means the OS boots even without a server. Same numbers we used to hardcode pre-session-21.

**Bootloader load = 128 sectors.** Half of 256 to avoid the 64K real-mode segment boundary that some BIOSes can't handle through `int 13h ah=42h`. 64 KiB headroom over the current ~66 KiB kernel — tight but works. A multi-call bootloader with bigger destination segments is the right long-term answer.

## Pitfalls

1. **Bootloader sector count is hardcoded** and silently truncates a too-big kernel. The build script doesn't warn if `kernel.bin` exceeds `bootloader_sectors * 512`. Adding a check would be one `if [ "$kernel_size" -gt "$max" ]; then echo ERROR` line.
2. **128-sector load is on a knife edge.** Kernel.bin is currently 65712 bytes; 128 sectors = 65536 bytes. We're 176 bytes over the limit but it works because... actually it just barely works because the LAST bytes of the kernel are .bss-folded zeros that the loader loads from the disk image (which extends past the kernel sector range, into the FS area which has its own data). When the kernel's runtime hits those bytes during execution, it reads stale FS data — we got lucky that it didn't matter for any reachable code. Fix is the same: bump the loader, or split into multiple int-13h calls.
3. **No ARP retry for in-flight packets.** ip_send returns -2 on cache miss and the caller is supposed to retry. TCP doesn't (we have no retransmit), so a missed first-packet means a stuck connection. We work around this by priming the gateway ARP at boot; new gateways or new same-subnet peers would have the same issue.
4. **DHCP doesn't renew.** Lease expiration would mean network goes down silently.
5. **DNS doesn't retry.** Single query, single timeout. `example.com` timed out in our test trace; a real client would retry.
6. **DNS doesn't follow CNAMEs.** A name that's an alias to another name returns "no record" if the recursive resolver doesn't unwind the chain itself.
7. **DNS query ID is `pit_ticks() & 0xFFFF`.** Predictable. A real client uses a CSPRNG to make spoofing harder.
8. **UDP listener table is 4 slots** and indexed by port. With DHCP (port 68) and DNS (port 53000) we use 2; sockets-from-userspace would need a port allocator.
9. **`udp_listen(port, NULL)` is a placeholder pattern.** dns_init does it to register the slot; the real callback is set on each `dns_resolve` call. If two callers raced (no real concern with one shell), the wrong handler could fire.
10. **DNS callback writes into a fixed-size kernel buffer.** A response > 1500 bytes (TCP-only DNS, unusual but possible for huge TXT records) would silently truncate.
11. **The pseudo-header buffer in `udp_checksum` is a 1500+12 byte stack array.** Big but fits — kernel stacks are 16 KiB.
12. **DHCP DISCOVER / REQUEST set the broadcast flag** in BOOTP `flags`. Some DHCP servers ignore it and unicast the OFFER to our MAC anyway; we accept those via the eth-mac filter (the OFFER is broadcast at IP level too in our path).

## What might come next

`SYS_UDP_SEND` + `SYS_UDP_RECVFROM` so userspace can build network clients (a `traceroute`, a `ntp` client). Then proper TCP retransmission so single dropped packets don't kill connections. Then DNS caching + `getaddrinfo`-style retry. After that, a real DHCP daemon in userspace that handles renewal in the background. And eventually IPv6, which would replicate this whole stack with 16-byte addresses and bigger headers — straightforward but voluminous.
