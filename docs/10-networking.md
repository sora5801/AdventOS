# Session 10 — Networking: PCI + RTL8139 + ARP/IPv4/ICMP

**Goal:** A real network stack. The OS finds its NIC over PCI, drives it to send and receive Ethernet frames, parses ARP and IPv4 packets that come in, replies to who-has and ICMP-echo requests, and can ping out.

End state of the milestone:

```
advent> ping 10.0.2.2
PONG from 10.0.2.2  seq=1  time=0 ms
```

That's an actual ICMP echo round trip across a real driver, real Ethernet/ARP/IPv4 stack, ending at QEMU's SLIRP gateway. ~750 lines of new code.

## What's in scope (and what isn't)

In:
- PCI bus scan + config-space access
- RTL8139 driver: init, IRQ-driven RX, polled TX with a 4-slot round robin
- Ethernet layer: frame parse/build, ethertype dispatch
- ARP: cache + request/reply
- IPv4: header parse/build, one's-complement checksum, protocol dispatch
- ICMP: echo request and reply
- Shell: `ifconfig`, `arp`, `ping`

Out:
- TCP — its own multi-day project (state machine, sequence numbers, retransmit, congestion control, sockets API)
- UDP (trivial to add but not needed yet)
- DHCP (we hard-code the SLIRP defaults)
- DNS
- Multiple NICs / interfaces
- IPv6
- TX in IRQ context (we drive TX synchronously from the caller)
- Routing tables (just "subnet → directly, else → gateway")

## Disk-to-wire path, top down

Sending an ICMP echo request looks like:

```
shell:  ping 10.0.2.2
   │
   │  parse_ipv4 → struct ip_addr { 10, 0, 2, 2 }
   ▼
icmp_send_echo(target, id, seq)
   │  build {icmp_hdr, 32B payload}, set csum
   ▼
ip_send(target, IP_PROTO_ICMP, payload, len)
   │  pick next-hop (target itself if local; else gateway)
   │  arp_lookup(next-hop) → MAC      [or arp_send_request + retry on miss]
   │  build IP header, set total_len, ttl, csum
   ▼
eth_send(mac, ETH_TYPE_IPV4, frame, len)
   │  build {eth_hdr, ip_hdr, icmp_hdr, payload}
   ▼
net_send_frame(buf, total_len)
   ▼
rtl8139_send(buf, total_len)
   │  pick TX slot, wait for OWN=1 from prior, memcpy into TX buffer,
   │  outl TSD with length → NIC DMAs out
   ▼
RTL8139 → wire → SLIRP → host network
```

Receive is the mirror image, IRQ-driven from the bottom:

```
RTL8139 raises IRQ 11
   ▼
rtl_irq() — drains the RX ring while !BUFE
   │  for each packet:  status, length, then the bytes
   ▼
net_rx_frame()
   ▼
eth_rx() — dispatch on ethertype
   ├── ETH_TYPE_ARP   → arp_rx() — cache update; reply if asking about us
   └── ETH_TYPE_IPV4  → ip_rx()
                          │
                          ▼
                       icmp_rx() — echo back, OR set g_ping_received
```

## PCI

[`kernel/pci.c`](../kernel/pci.c). About 60 lines. Exposes:

```c
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
int      pci_find(uint16_t vendor, uint16_t device, struct pci_device *out);
```

Reads use the I/O port pair (0xCF8 address, 0xCFC data). Address word format is fixed:

```
bit 31     : enable
bits 30-24 : reserved
bits 23-16 : bus
bits 15-11 : device
bits 10-8  : function
bits 7-2   : register dword
bits 1-0   : 0
```

`pci_find` brute-walks bus 0..3, dev 0..31, func 0..7. Vendor ID 0xFFFF means "no device here, skip". For each match it captures BAR0 (the I/O base for the RTL8139), the IRQ line, and writes back the command register with bus master + I/O space + memory enabled — QEMU often leaves these clear.

There are no PCI bridges in our world. A real implementation would recurse through them when it sees a class code 0x0604 device.

## RTL8139

[`kernel/rtl8139.c`](../kernel/rtl8139.c), ~150 lines. The card is intentionally simple compared to anything modern — no descriptor rings to set up, no scatter/gather, no MSI.

**Init sequence:**

1. PCI find (10EC:8139), pull I/O base and IRQ line.
2. Power on (`outb 0x52 = 0`).
3. Reset (`CR.RST = 1`, poll until cleared).
4. Read MAC from registers 0x00..0x05 directly — there's no EEPROM dance needed; the controller exposes the burned-in address.
5. Allocate the RX buffer (`8192 + 16 + 1500 = 9708 bytes`) and four 2 KiB TX buffers from `kmalloc`. They're all in our identity-mapped kernel heap, so we can hand the kernel virtual addresses straight to the NIC for DMA.
6. Tell the NIC where the RX buffer is (`outl RBSTART, phys`).
7. Pre-load `TSAD0..3` with each TX buffer's physical address.
8. Hook IRQ 11 and unmask it on the PIC (more on this in a moment).
9. Set IMR for ROK | TOK only.
10. Set RCR for promiscuous + multicast + broadcast + WRAP=1.
11. Set CR for TE | RE — transmit + receive enabled.

`WRAP=1` is what lets us treat the RX buffer linearly: the NIC will write past `RX_BUF_LEN` if the last packet falls across the boundary, up to one MTU. With the +1500 slack we never lose a packet to wraparound, and we don't have to handle a packet split across the buffer ends.

**Receive (`rtl_irq`):**

The 16-bit ISR has bits for ROK, RER, TOK, TER. We acknowledge by writing the bits *back* (write-1-to-clear semantics). Then while `CR.BUFE == 0` (buffer not empty), pull packets out:

```c
uint8_t *pkt = g_rx_buf + g_rx_ptr;
uint16_t status = pkt[0] | (pkt[1] << 8);
uint16_t length = pkt[2] | (pkt[3] << 8);     // includes 4-byte FCS
if ((status & 1) && length >= 14 && length <= 1518) {
    net_rx_frame(pkt + 4, length - 4);        // strip FCS for upstream
}
g_rx_ptr = (g_rx_ptr + length + 4 + 3) & ~3u; // advance, 4-byte aligned
outw(CAPR, g_rx_ptr - 16);                    // -16 is RTL's quirk
```

`CAPR - 16` is one of those things you can only learn from the data sheet (or someone else's RTL8139 code). The chip wants the read pointer biased back by 16. Don't ask why.

**Transmit (`rtl8139_send`):**

Round-robin across 4 TX descriptors. Each descriptor owns a 2 KiB buffer that lives in the kernel heap. To send:

1. Take the global TX spinlock.
2. Pick `g_tx_cur` slot, advance the index.
3. Spin until `TSD[slot].OWN == 1` (NIC done with previous TX, or initial state).
4. `memcpy` the frame into `g_tx_buf[slot]`.
5. `outl TSD[slot] = length`. Writing length-with-OWN-clear kicks the DMA.

The OWN bit is the only synchronization — it goes 0 (NIC owns) on transmit start and 1 (driver owns) when the NIC is done. The IRQ on TOK is purely informational and we don't act on it.

Frame padding to the 60-byte minimum is also done here. Anything shorter would be silently dropped on the wire.

## The IRQ-2 cascade trap

First boot after wiring everything up: PCI scan worked, NIC initialized, ARP request transmitted (no error), no reply ever came back. `ifconfig` looked perfect:

```
eth0:
    HWaddr  52:54:00:12:34:56
    inet    10.0.2.15
    netmask 255.255.255.0
    gateway 10.0.2.2
arp 10.0.2.2 → arp: timeout
```

The NIC was talking; nothing was talking back. Took a minute to remember that **IRQ 11 lives on the slave 8259**. The slave PIC signals the master via IRQ 2 ("the cascade"). And in [`kernel/kernel.c`](../kernel/kernel.c) right after PIC remap I had:

```c
for (int i = 0; i < 16; i++) pic_set_mask((uint8_t)i);
```

— mask everything, let drivers unmask their own IRQs. The PIT and keyboard and serial all sit on the master, so they were fine. IRQ 11 fires on the slave, the slave forwards to IRQ 2 on the master, the master sees IRQ 2 is masked, and the entire RX path silently drops on the floor.

Fix is one line:

```c
pic_clear_mask(2);   /* IRQ 2 = master-side cascade for slave-PIC IRQs */
```

After that, ARP returned the SLIRP gateway's MAC immediately and ping worked end-to-end.

This is one of those classes of bug where the driver is correct, the higher layers are correct, and the symptom (silent drop) gives you almost no signal. The lesson: every time you add a new IRQ on the slave PIC (8..15), make sure IRQ 2 on the master is unmasked. There should probably be an assertion somewhere that catches this.

## ARP

[`kernel/arp.c`](../kernel/arp.c), ~110 lines. A 16-entry direct-mapped cache (`ip → mac`) protected by a spinlock. Handler does three things:

1. **Always** install/refresh `(sender_ip, sender_mac)` on any received ARP packet, regardless of opcode. Free address learning.
2. If opcode is REQUEST and target_ip matches our IP, build a REPLY and send to the requester.
3. (REPLY messages are absorbed into the cache by step 1 alone — there's no separate "I was waiting for this reply" path.)

To send a request, build the standard packet (opcode REQUEST, target_mac all-zero, target_ip what we want) and broadcast on FF:FF:FF:FF:FF:FF. SLIRP's gateway responds within milliseconds.

Looking up an entry on a miss returns `-1`, which lets `ip_send` fire a request and return `-2` "ARP probe sent, retry shortly". The shell's `ping` retries the lookup at 10 ms intervals for 500 ms before giving up.

## IPv4

[`kernel/ip.c`](../kernel/ip.c), ~80 lines. The header is a packed C struct that mirrors the RFC 791 layout. We never set the DF/MF flags, so `flags_frag` stays zero (no fragmentation; if anything ever required it we'd drop).

The one's-complement checksum is the textbook 16-bit-half-word fold:

```c
uint16_t ip_checksum(const void *data, uint32_t len) {
    const uint16_t *p = data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len)        { sum += *(const uint8_t *)p; }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}
```

Same algorithm is reused for ICMP (covering the whole ICMP packet). UDP and TCP would need it over a pseudo-header that includes IP src/dst — same kernel of code, different inputs.

Routing is one-line "is this IP on my subnet?" via `(dst & netmask) == (my_ip & netmask)`. If yes, ARP for the target directly; if no, ARP for the gateway. SLIRP's response to an off-subnet packet is to forward it to the host stack's network — except ICMP through SLIRP-to-real-internet is unreliable on Windows because of raw-socket restrictions. Pinging 10.0.2.2 (the gateway itself) always works.

## ICMP

[`kernel/icmp.c`](../kernel/icmp.c), ~50 lines. Two cases:

- **Echo Request → Reply.** Copy the request bytes verbatim, change `type` from 8 to 0, recompute the checksum, send back to the source IP. Same id, same seq, same payload.
- **Echo Reply.** Set `g_ping_received = 1`, record the seq and the current PIT tick. The `ping` shell command spins on this flag.

The IRQ-context-to-shell rendezvous is just three `volatile` variables:

```c
volatile int      g_ping_received;
volatile uint16_t g_ping_last_seq;
volatile uint32_t g_ping_last_tick;
```

No condition variable, no wait queue. `ping` busy-waits with `pit_sleep(10)` between checks. Good enough at this scale.

## Shell

`ifconfig`:

```
eth0:
    HWaddr  52:54:00:12:34:56
    inet    10.0.2.15
    netmask 255.255.255.0
    gateway 10.0.2.2
```

`arp` (no arg) — print cache. `arp <ip>` — fire a request, wait up to 1 s, print result.

`ping <ip>` — resolve ARP for the next-hop, send ICMP echo with id=0xBEEF and an incrementing seq, poll `g_ping_received` for up to 1 s, print round-trip in PIT-tick (10 ms) granularity.

```
advent> arp 10.0.2.2
arp: 10.0.2.2  ->  52:55:0a:00:02:02
advent> ping 10.0.2.2
PONG from 10.0.2.2  seq=1  time=0 ms
advent> arp
ARP cache:
  10.0.2.2  ->  52:55:0a:00:02:02
```

The MAC `52:55:0a:00:02:02` is SLIRP's pattern: `52:55` prefix + the IP encoded as the last 4 bytes (`0a.00.02.02` = 10.0.2.2). Useful for sanity-checking the wire path.

`time=0 ms` means the round trip completed inside one PIT tick (< 10 ms). Bumping the PIT to 1 kHz would give us millisecond resolution.

## Files added

| File | Role |
|---|---|
| `kernel/pci.{h,c}` | Bus scan + config-space access |
| `kernel/net.{h,c}` | Common types (`mac_addr`, `ip_addr`), htons/htonl, my_ip/my_mac, init |
| `kernel/rtl8139.{h,c}` | NIC driver: init, IRQ-driven RX, round-robin TX |
| `kernel/eth.{h,c}` | Ethernet frame parse + dispatch |
| `kernel/arp.{h,c}` | 16-entry cache + request/reply |
| `kernel/ip.{h,c}` | IPv4 header + checksum + send |
| `kernel/icmp.{h,c}` | Echo request + reply |
| `kernel/kernel.c` | `net_init()` after `sti`; **IRQ 2 cascade unmask** |
| `kernel/shell.c` | `ifconfig`, `arp`, `ping` |
| `build.sh` | Run line includes `-netdev user -device rtl8139,...` |

## Design decisions

**RTL8139, not e1000 or virtio.** The 8139 is the smallest amount of code that gets you a working NIC in QEMU. e1000 needs descriptor rings and PCI-DMA-BAR mapping. virtio is a full virtqueue protocol. The 8139 is "MAC at this offset, read pointer here, write pointer here, IRQ on receive" — fits in 150 lines.

**SLIRP user-mode networking.** Doesn't need a TAP device or admin privileges. Easy to test on Windows. The downside is ICMP-through-SLIRP-to-the-host-network is unreliable; we test against the SLIRP gateway itself, which works deterministically.

**Hard-coded IP/gateway/netmask.** No DHCP. SLIRP always gives 10.0.2.15 to the guest, 10.0.2.2 as the gateway, with 255.255.255.0 mask. We bake those in. Real DHCP (DISCOVER/OFFER/REQUEST/ACK over UDP) is a session of its own.

**No bottom half / softirq.** Every received packet is processed entirely in the IRQ handler — ARP cache writes, IP dispatch, ICMP reply transmission, all in ring 0 with IF=0. For a real OS this is wrong (long-running RX work would block other IRQs); for our scale it's the simplest correct thing.

**Spinlock-protected ARP cache and TX path.** Same UP-correct spinlock as session 6. The cache is read by the shell and written by the NIC IRQ; the TX path can be entered both from the shell context (ARP probes, ping) and from the IRQ context (ARP replies, ICMP echo replies).

**One-shot polling for replies.** `ping` polls `g_ping_received` rather than waiting on a condition variable. Adding a real signal mechanism would mean another wait queue and another set of edge cases for the reaper to deal with. Not worth it for a single-flight ping.

**Promiscuous mode (`RCR_AAP`).** Slightly questionable but harmless — SLIRP filters to the right MAC anyway, and AAP avoids any mismatch between what the NIC accepts and what the kernel filters. A real driver would use `APM` (physical match only) and trust the MAC filter.

## Pitfalls

1. **IRQ 2 must be unmasked** before any slave-PIC interrupt (8..15) can reach the CPU. The cascade chip is invisible from the kernel's POV but very visible when nothing happens.
2. **CAPR offset is `read_ptr - 16`**, not `read_ptr`. RTL8139 quirk; ignore at your peril and the buffer fills up forever.
3. **Network byte order is big-endian.** Wrap every multi-byte header field in `htons`/`ntohs`. The packed struct definitions describe wire-format bytes, not C ints.
4. **TX OWN bit is `1` when driver-owned**, not when NIC-owned. Initial state after reset is `1` for all four descriptors.
5. **Pad short frames to 60 bytes** before sending. Anything shorter is dropped at the controller.
6. **One's-complement checksum end-around carry** has to fold a couple of times if the payload is large. The `while (sum >> 16) ...` loop handles it; one round is not always enough.
7. **`-netdev user` is layer-3-aware on Windows.** SLIRP synthesizes ARP and ICMP for itself but won't proxy ICMP to the wider internet without admin. Test against the gateway, not 8.8.8.8.
8. **PCI command-register bit 2 (bus master) must be set** for the NIC to do DMA. QEMU usually leaves it clear; `pci_find` ORs it back in.

## What's next session 11 material

UDP is the obvious next thing. Then maybe a tiny TCP good enough for a single connection: SYN/SYN-ACK/ACK, in-order data, FIN, with a fixed window and no congestion control. After that, sockets API in libuser, and netcat-like user programs.
