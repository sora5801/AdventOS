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
| Unix coreutils — ls, cat, cp, mv, rm, mkdir, rmdir, chmod, touch, find, head, tail, grep, sort, uniq, wc, tee, tr, seq, echo, date, ps, kill, pwd, id, man | ✅ |
| Shells — interactive `sh.elf` with pipes/redirection/jobs/history/tab-completion/env vars/mid-line editing | ✅ |
| Modal editor — `vi.elf` (undo, count prefixes, search/replace, motions, modes) | ✅ |
| Man pages — 27 pages under `/man/`, `man <topic>` + `man -k WORD` | ✅ |
| Scripting — `lua` (Lua-syntax subset, int32 numbers, tree-walking interpreter) | ✅ |
| Native compiler — `cc` (C-subset, int-only + string literals, emits ELF32) | ✅ |
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

Current session: **91 — cc Phase 2 part 1: string literals + `puts`**. See [`docs/78-cc-strings.md`](docs/78-cc-strings.md) for the deep dive (string pool appended to PT_LOAD, dedup, `puts`/`print_str` helper functions, escape sequences). `puts("hello, world")` works.

Recent session deep dives:
- [Session 91 — cc Phase 2 part 1: string literals + puts](docs/78-cc-strings.md)
- [Session 90 — A C-subset compiler (`cc`)](docs/77-tinycc.md)
- [Session 89 — Lua: multi-return + generic for](docs/76-lua-multireturn.md)
- [Session 88 — Lua: error handling, closures, GC](docs/75-lua-error-handling-and-gc.md)
- [Session 87 — Lua-syntax interpreter](docs/74-tinylua.md)
- [Session 86 — vi polish (undo, search/replace, count prefixes, ?pat)](docs/73-vi-polish.md)
- [Session 85 — Man pages](docs/72-man-pages.md)
- [Session 84 — Shell mid-line editing](docs/71-shell-mid-line-editing.md)
- [Session 83 — Usable Unix Phase 1: coreutils gap-fill + selftest reliability](docs/70-usable-unix-coreutils.md)

The full session index is in `docs/`. Highlights:
- [Session 80 — SMP=2 deadlock fixes](docs/68-smp2-deadlock-fixes.md) (`-smp 2` finally usable)
- [Session 81 — Structured pipelines (JSONL)](docs/69-structured-pipelines.md) (`\|>` operator)
- [Session 50 — SSH server](docs/50-ssh-server.md)
- [Session 36 — TLS 1.3 + HTTPS](docs/36-tls13-https.md)

## Status & scope

AdventOS is a personal-project OS. It targets QEMU 10.x and the bochs/seabios BIOS that ships with it. Real-hardware boot has worked in the past but isn't continuously tested. The OS is single-architecture (i386), single-FS (AdventFS), single-machine — no clustering, no live migration, no certifications.

**Path A — Usable Unix is complete** as of session 86:
- Phase 1 ✅ — coreutils gap-fill (cp/mv/rm/mkdir/rmdir/chmod/touch/find)
- Phase 2 ✅ — shell mid-line editing (left/right arrows, Ctrl-A/E/W/U/K)
- Phase 3 ✅ — man pages (`man <topic>`, 26 pages)
- Phase 4 ✅ — vi polish (undo, count prefixes, `:s/old/new/`, backward search)

**Path D — Scripting is complete** as of session 89. AdventOS has a usable Lua-syntax interpreter (`lua`) with all the major idioms: pcall/error, capture-by-value closures, mark-sweep GC, multi-return values, generic `for k, v in pairs(t)`, real iterators. See [`docs/74-tinylua.md`](docs/74-tinylua.md) (original design), [`docs/75-lua-error-handling-and-gc.md`](docs/75-lua-error-handling-and-gc.md) (session-88 additions), and [`docs/76-lua-multireturn.md`](docs/76-lua-multireturn.md) (session-89 final piece). Deliberately not in scope: metatables, coroutines, capture-by-reference closures, string patterns, math library.

**Path B — Self-hosting is in progress.** Session 90 (Phase 1) shipped a 1500-line C-subset compiler — int-only, no strings/pointers/globals/preprocessor; see [`docs/77-tinycc.md`](docs/77-tinycc.md). Session 91 (Phase 2 part 1) added string literals + `puts`/`print_str` so `puts("hello, world")` works; see [`docs/78-cc-strings.md`](docs/78-cc-strings.md). Remaining Phase 2 sub-sessions: char/pointers/&/* (92), globals (93), `printf` (94), preprocessor (95), compound ops (96).

Remaining candidate paths:
- **Path B Phase 2+** — char/pointers/strings/globals/preprocessor in `cc`, or eventually a real `tcc` port.
- **Path C — Graphics.** A minimal window manager on the VBE framebuffer.
- **Path E — Drivers.** virtio (modern QEMU's preferred device family), more USB device classes, sound consumer.

## License

(Not yet specified. Treat as all-rights-reserved until a license file lands.)

## Acknowledgements

Built with help from Claude (claude.com/claude-code) acting as the engineering pair. The session deep-dives in `docs/` are the audit trail of that collaboration.
