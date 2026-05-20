# AdventOS — v1.0.0

A 32-bit i386 operating system written from scratch in C. Boots on bare hardware (and inside QEMU), runs preemptive multitasking with SMP, talks TCP/UDP/DHCP/DNS/NTP/TLS 1.3, hosts in-guest HTTP/HTTPS/SSH/IRC servers, supports USB HID + Mass Storage, and ships a Unix-like userland with fork/exec, pipes, jobs, signals, sandbox/limits, a JSON-RPC daemon, and an in-guest ptrace debugger.

```
    _       _                  _    ___  ____
   / \   __| |_   _____ _ __ | |_ / _ \/ ___|
  / _ \ / _` \ \ / / _ \ '_ \| __| | | \___ \
 / ___ \ (_| |\ V /  __/ | | | |_| |_| |___) |
/_/   \_\__,_| \_/ \___|_| |_|\__|\___/|____/
```

**v1.0.0** — 171 sessions of work.  Reading order, subsystem
breakdown, and per-feature session logs are in
[`docs/INDEX.md`](docs/INDEX.md).  v1.0 readiness rubric is in
[`docs/158-v1.0-readiness.md`](docs/158-v1.0-readiness.md).

## What works today

| Layer | Status |
|---|---|
| Bootloader (real-mode → protected-mode) | ✅ |
| i386 kernel, paging, PMM, kmalloc | ✅ |
| Preemptive scheduler with priorities | ✅ |
| SMP (1–2 CPUs, BSP + 1 AP) with BKL + per-resource locks | ✅ |
| Signals (POSIX-ish: TERM/KILL/STOP/CONT/USR1+2/PIPE/CHLD) | ✅ |
| fork/exec/wait/pipe/dup2 | ✅ |
| Sandbox masks + per-task resource limits (RSS/CPU/wall/FDs) | ✅ |
| AdventFS (custom on-disk FS) — files, directories, perms | ✅ |
| Block cache, virtual FS layer, /proc | ✅ |
| ATA driver, USB UHCI + EHCI 2.0 (480 Mbps full transfer path), USB HID keyboard, USB Mass Storage, USB CDC-ACM serial, USB CDC-ECM Ethernet | ✅ |
| AHCI SATA controller — IRQ-driven, NCQ (32 in-flight slots) | ✅ |
| NVMe — modern PCIe-attached SSD interface (admin + I/O queue pairs, IDENTIFY, READ / WRITE) | ✅ |
| virtio-blk + virtio-scsi + virtio-net + virtio-rng + virtio-console + virtio-balloon + virtio-9p (host fs passthrough, read+write+rename) | ✅ |
| e1000 / 82540EM gigabit NIC (alongside rtl8139 + virtio-net + USB CDC-ECM) | ✅ |
| AC97 audio + `aplay` userspace consumer (PCM/WAV streaming) | ✅ |
| TCP/UDP, DHCP client, DNS resolver + cache, NTP client | ✅ |
| TLS 1.3 (ECDHE-RSA + AES-128-GCM, real-world server interop) | ✅ |
| In-guest httpd, httpsd, sshd, ircd | ✅ |
| In-guest clients: nc, wget, telnet, irc, ssh, httpsget | ✅ |
| Unix coreutils — ls, cat, cp, mv, rm, mkdir, rmdir, chmod, touch, find, head, tail, grep, sort, uniq, wc, tee, tr, seq, echo, date, ps, kill, pwd, id, man | ✅ |
| Shells — `sh.elf` bash-compatible: pipes, `;`/`&&`/`||`/`>>`/`<`, glob, brace expansion `{a,b}`/`{1..N}`, `~` tilde, `$VAR`/`$?`/`$#`/`$@`/`$0..$9`, parameter forms `${var:-x}`/`${#var}`/`${var%suf}`/`${var/old/new}`, arithmetic `$((..))`/`((..))`, control flow (`if`/`for`/`while`), functions, builtins `[`/`test`/`read`/`shift`/`break`/`continue`/`return`, `!!`/`!N` history recall, Ctrl-R reverse search, command/var/file tab completion, dynamic `advent<cwd>$ ` prompt | ✅ |
| Modal editor — `vi.elf` (undo, count prefixes, search/replace, motions, modes) | ✅ |
| Man pages — 27 pages under `/man/`, `man <topic>` + `man -k WORD` | ✅ |
| Scripting — `lua` (Lua-syntax subset, int32 numbers, tree-walking interpreter) | ✅ |
| Native compiler — `cc` (C-subset: int, char, pointers, arrays, strings, globals, `printf`, preprocessor, compound ops, structs (incl. arrays-of, value-assign, by-value calls), function pointers, `sizeof`, scaled ptr arith, multi-file, enum, typedef, real variadics; emits ELF32) | ✅ |
| ptrace-based debugger — `dbg.elf` | ✅ |
| Multi-user with `/etc/passwd`-style login | ✅ |
| JSON-RPC daemon (`agentd`) exposing the OS surface over loopback | ✅ |
| Structured-pipeline operator `\|>` with JSONL between stages | ✅ |
| Selftest harness for kernel + userland regression | ✅ |

## Quick start

Prerequisites: a recent QEMU (10.x+), an i386 cross-toolchain (`mingw-w64` on MSYS2 works), and bash. Tested on Windows 11 / MSYS2 UCRT64. Linux should work with minor `build.sh` tweaks.

```bash
# Build the disk image (bootloader + kernel + AdventFS).
bash build.sh

# Minimal run — graphical QEMU window, USB keyboard, single CPU.
qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32 \
    -smp 1 \
    -device piix3-usb-uhci,id=usb0 \
    -device usb-kbd,bus=usb0.0

# Full run — SMP=2, networking, USB storage.
qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32 \
    -smp 2 \
    -netdev user,id=net0,hostfwd=tcp::8080-:80,hostfwd=tcp::7000-:7000,hostfwd=tcp::2222-:2222 \
    -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
    -device piix3-usb-uhci,id=usb0 \
    -device usb-kbd,bus=usb0.0 \
    -drive id=usbfs,file=usbfs.img,format=raw,if=none \
    -device usb-storage,drive=usbfs,bus=usb0.0
```

Hostfwd maps in the full-run command:
- `localhost:8080` → in-guest `httpd`
- `localhost:7000` → in-guest `agentd` JSON-RPC
- `localhost:2222` → in-guest `sshd`

You can also `curl http://localhost:8080/` from the host once the OS is up.

## Repo layout

```
boot/        16-bit real-mode bootloader (assembly)
kernel/      i386 protected-mode kernel (C + a little asm)
user/        Userland programs (sh, init, coreutils, daemons, selftests)
libc/        Dynamically-linked libc (sessions 9+; `libc.bin` cached at boot)
libuser/     Static helpers linked into every user binary
libcrypto/   From-scratch crypto: SHA-256, AES, P-256 ECDH, RSA, HMAC, X.509
libjson/     Streaming JSON parser/emitter for the agent-RPC + structured pipelines
include/     Shared kernel+userland headers (io, types, etc.)
fs/          Files included at mkfs time (passwd, ssl certs, agent.tools.json…)
docs/        Per-session technical deep dives (171 docs; index at docs/INDEX.md)
mkfs.py      Builds the AdventFS image
build.sh     Orchestrates the whole build
```

## How development works

The project advances in numbered "sessions" — each session is a focused chunk of work that lands as one or more git commits plus a `docs/NN-name.md` deep-dive explaining the design choices and the bugs found. Sessions are not strictly chronological with commit dates; some run a few hours, others span days when a hard bug is being chased.

171 sessions in, all five paths (A/B/C/D/E) are complete; see
[`docs/INDEX.md`](docs/INDEX.md) for the full grouped session log.
Highlight sessions to start with:

- [Session 1 — Bootloader + 32-bit kernel](docs/01-bootloader-and-kernel.md) — how the system boots
- [Session 14 — fork, exec, wait](docs/14-fork-exec-wait.md) — the Unix surface lands
- [Session 36 — TLS 1.3 + HTTPS](docs/36-tls13-https.md) — real-world crypto
- [Session 50 — SSH server](docs/50-ssh-server.md) — ssh in, get a shell
- [Session 64 — Agent JSON-RPC tooling](docs/64-agent-rpc.md) — agentd surface
- [Session 80 — SMP=2 deadlock fixes](docs/68-smp2-deadlock-fixes.md) — `-smp 2` finally usable
- [Session 90 — A C-subset compiler from scratch](docs/77-tinycc.md) — `cc`
- [Session 111 — Window manager daemon](docs/98-pathC-wm.md) — wmd compositor lands
- [Session 134 — wmterm terminal emulator](docs/120-pathC-wmterm.md) — terminals
- [Session 169 — Stability + flake hunt](docs/155-pathC-stability-flake-hunt.md) — how the test discipline works
- [Session 172 — v1.0 readiness assessment](docs/158-v1.0-readiness.md) — the rubric this release passed

## Status & scope

AdventOS is a personal-project OS.  It targets QEMU 10.x and the
bochs/seabios BIOS that ships with it.  Real-hardware boot has worked
in the past but isn't continuously tested.  The OS is single-
architecture (i386), single-FS (AdventFS), single-machine — no
clustering, no live migration, no certifications.

All five development paths reached completion at v1.0.0:

- **Path A — Usable Unix** (sessions 83–86, 136–140): bash-compat
  shell, mid-line editing, control flow, functions, arithmetic,
  parameter expansion, tab completion, history recall, brace
  expansion.  Real `.sh` scripts work.
- **Path B — Self-hosting compiler** (sessions 90–106, 121, 125,
  133–137): `cc` (1500-line C-subset compiler from scratch) +
  `tcc` (vendored real TinyCC).  Both emit ELF32 that runs inside
  AdventOS.  cc has reg-allocator, const-fold, peephole, DCE.
- **Path C — Graphics + WM** (sessions 107–169): VBE framebuffer →
  libgfx → wmd compositor → 10+ apps (wmterm, wmedit, wmcalc,
  wmpaint, wmview, wmfiles, wmsysinfo, wmps, wmclock, …) →
  scrollback, selection + clipboard, 256-color terminal, snap
  preview, workspaces, USB tablet.
- **Path D — Scripting** (sessions 87–89): Lua-syntax interpreter
  (`lua`).  ~1100 lines, pcall/error, closures, mark-sweep GC,
  multi-return, generic `for k,v in pairs(t)`.
- **Path E — Drivers** (sessions 118–127): virtio family
  (blk/scsi/net/rng/console/balloon/9p), e1000 NIC, AHCI SATA
  with NCQ, NVMe (PRP DMA), USB UHCI + EHCI, USB class drivers
  (HID, MSC, CDC-ACM, CDC-ECM), AC97 audio.

The driver tier covers: **storage** (ATA / AHCI-NCQ / virtio-blk /
virtio-scsi / NVMe / USB MSC over either HC), **net** (rtl8139 /
virtio-net / e1000 / USB CDC-ECM), **USB** (UHCI + EHCI both fully
integrated; HID + MSC + CDC-ACM + CDC-ECM class drivers),
**filesystem passthrough** (virtio-9p read/write/rename), **misc**
(virtio-rng / virtio-console / virtio-balloon / AC97 audio).

Per-feature reading order is in [`docs/INDEX.md`](docs/INDEX.md).
The v1.0 readiness rubric is in
[`docs/158-v1.0-readiness.md`](docs/158-v1.0-readiness.md).

### Out of scope for v1.0

- **x86_64, ARM, RISC-V** — i386 only, by design.
- **Self-hosting the kernel** — neither `cc` nor `tcc` is wired to
  build `kernel/` from inside the OS.  Both can build small
  programs end-to-end.
- **xHCI / USB 3.0**, **virtio-gpu hardware accel**, **EHCI
  periodic schedule** (for true iso transfers).  Standalone
  follow-ups; none gate v1.0.
- **Real Lua features:** metatables, coroutines, capture-by-
  reference closures, string patterns, math lib.  Path D shipped
  the subset that's useful for small scripts.

## License

[MIT](LICENSE).  Copy it, fork it, ship it, learn from it.

## Acknowledgements

Built with help from Claude (claude.com/claude-code) acting as the engineering pair. The session deep-dives in `docs/` are the audit trail of that collaboration.
