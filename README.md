# AdventOS

A 32-bit i386 operating system written from scratch in C. Boots on bare hardware (and inside QEMU), runs preemptive multitasking with SMP, talks TCP/UDP/DHCP/DNS/NTP/TLS 1.2, hosts in-guest HTTP/HTTPS/SSH/IRC servers, supports USB HID + Mass Storage, and ships a Unix-like userland with fork/exec, pipes, jobs, signals, sandbox/limits, a JSON-RPC daemon, and an in-guest ptrace debugger.

```
    _       _                  _    ___  ____
   / \   __| |_   _____ _ __ | |_ / _ \/ ___|
  / _ \ / _` \ \ / / _ \ '_ \| __| | | \___ \
 / ___ \ (_| |\ V /  __/ | | | |_| |_| |___) |
/_/   \_\__,_| \_/ \___|_| |_|\__|\___/|____/
```

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
| ATA driver, USB UHCI controller, USB HID keyboard, USB Mass Storage | ✅ |
| TCP/UDP, DHCP client, DNS resolver + cache, NTP client | ✅ |
| TLS 1.3 (ECDHE-RSA + AES-128-GCM, real-world server interop) | ✅ |
| In-guest httpd, httpsd, sshd, ircd | ✅ |
| In-guest clients: nc, wget, telnet, irc, ssh, httpsget | ✅ |
| Unix coreutils — ls, cat, cp, mv, rm, mkdir, rmdir, chmod, touch, find, head, tail, grep, sort, uniq, wc, tee, tr, seq, echo, date, ps, kill, pwd, id | ✅ |
| Shells — interactive `sh.elf` with pipes/redirection/jobs/history/tab-completion/env vars | ✅ |
| Modal editor — `vi.elf` (limited but real) | ✅ |
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
docs/        Per-session technical deep dives (sessions 1–83+)
mkfs.py      Builds the AdventFS image
build.sh     Orchestrates the whole build
```

## How development works

The project advances in numbered "sessions" — each session is a focused chunk of work that lands as one or more git commits plus a `docs/NN-name.md` deep-dive explaining the design choices and the bugs found. Sessions are not strictly chronological with commit dates; some run a few hours, others span days when a hard bug is being chased.

Current session: **83 — Usable Unix**. See [`docs/70-usable-unix-coreutils.md`](docs/70-usable-unix-coreutils.md) for the latest deep dive (selftest reliability fixes + coreutils gap-fill).

The full session index is in `docs/`. Highlights:
- [Session 80 — SMP=2 deadlock fixes](docs/68-smp2-deadlock-fixes.md) (`-smp 2` finally usable)
- [Session 81 — Structured pipelines (JSONL)](docs/69-structured-pipelines.md) (`\|>` operator)
- [Session 50 — SSH server](docs/50-ssh-server.md)
- [Session 36 — TLS 1.3 + HTTPS](docs/36-tls13-https.md)

## Status & scope

AdventOS is a personal-project OS. It targets QEMU 10.x and the bochs/seabios BIOS that ships with it. Real-hardware boot has worked in the past but isn't continuously tested. The OS is single-architecture (i386), single-FS (AdventFS), single-machine — no clustering, no live migration, no certifications.

The current phase ("Path A — Usable Unix") is closing the gaps that make daily shell use feel broken: missing coreutils, weak line-editing, no man pages, vi limitations. After that, candidate next phases are self-hosting a C compiler (port `tcc`), a window manager on the VBE framebuffer, more drivers (virtio, AC97 consumer), and a scripting language (probably Lua).

## License

(Not yet specified. Treat as all-rights-reserved until a license file lands.)

## Acknowledgements

Built with help from Claude (claude.com/claude-code) acting as the engineering pair. The session deep-dives in `docs/` are the audit trail of that collaboration.
