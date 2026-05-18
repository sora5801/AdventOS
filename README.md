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
| ATA driver, USB UHCI controller, USB HID keyboard, USB Mass Storage, USB CDC-ACM serial | ✅ |
| AHCI SATA controller (modern hard-disk interface) | ✅ |
| virtio-blk + virtio-net + virtio-rng + virtio-console + virtio-balloon + virtio-9p (host fs passthrough, read+write+rename) | ✅ |
| e1000 / 82540EM gigabit NIC (alongside rtl8139 + virtio-net) | ✅ |
| AC97 audio + `aplay` userspace consumer (PCM/WAV streaming) | ✅ |
| TCP/UDP, DHCP client, DNS resolver + cache, NTP client | ✅ |
| TLS 1.3 (ECDHE-RSA + AES-128-GCM, real-world server interop) | ✅ |
| In-guest httpd, httpsd, sshd, ircd | ✅ |
| In-guest clients: nc, wget, telnet, irc, ssh, httpsget | ✅ |
| Unix coreutils — ls, cat, cp, mv, rm, mkdir, rmdir, chmod, touch, find, head, tail, grep, sort, uniq, wc, tee, tr, seq, echo, date, ps, kill, pwd, id, man | ✅ |
| Shells — interactive `sh.elf` with pipes/redirection/jobs/history/tab-completion/env vars/mid-line editing | ✅ |
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
docs/        Per-session technical deep dives (sessions 1–83+)
mkfs.py      Builds the AdventFS image
build.sh     Orchestrates the whole build
```

## How development works

The project advances in numbered "sessions" — each session is a focused chunk of work that lands as one or more git commits plus a `docs/NN-name.md` deep-dive explaining the design choices and the bugs found. Sessions are not strictly chronological with commit dates; some run a few hours, others span days when a hard bug is being chased.

Current session: **123 — Path E phase 6: AHCI SATA controller**. See [`docs/110-pathE-ahci.md`](docs/110-pathE-ahci.md). New `kernel/ahci.{h,c}` — modern hard-disk interface, what every real PC has shipped with for the last 15 years. Registers each attached SATA disk as a `blkdev` next to ATA, virtio-blk, and USB MSC, so bcache / fs / `SYS_BLOCK_*` syscalls work through it uniformly. Stretch goal too: AdventFS mounts directly off an AHCI disk at `/mnt/sata`.

Recent session deep dives:
- [Session 123 — Path E phase 6: AHCI SATA controller](docs/110-pathE-ahci.md)
- [Session 122 — Path E phase 5: e1000 NIC + 9p atomic rename](docs/109-pathE-e1000-and-rename.md)
- [Session 121 — Path E phase 4: 9p writes + IRQ-driven virtio](docs/108-pathE-9p-write-and-irq.md)
- [Session 120 — Path E phase 3: WSL build + virtio-9p](docs/107-pathE-9p.md)
- [Session 128 — cc language corners (11 features)](docs/115-pathB-language-corners.md)
- [Session 125 — cc optimization passes (reg-alloc, const-fold, peephole, DCE)](docs/112-pathB-optimizations.md)
- [Session 121 — Path B Phase 4 capstone (SBV returns, static/extern, fp typedef)](docs/108-pathB-capstone.md)
- [Session 119 — Path E phase 2: virtio-rng, virtio-console, virtio-balloon](docs/106-pathE-more-virtio.md)
- [Session 118 — Path E: drivers (virtio-blk, virtio-net, CDC-ACM, aplay)](docs/105-pathE-drivers.md)
- [Session 107 — Path C phase 1: userspace framebuffer](docs/94-pathC-fb.md)
- [Session 106 — cc Phase 3 part 10: struct-by-value calls](docs/93-cc-struct-by-value.md)
- [Session 105 — cc Phase 3 part 9: variadic functions](docs/92-cc-variadics.md)
- [Session 104 — cc Phase 3 part 8: typedef](docs/91-cc-typedef.md)
- [Session 103 — cc Phase 3 part 7: enums](docs/90-cc-enums.md)
- [Session 102 — cc Phase 3 part 6: array-of-struct + indexed member access](docs/89-cc-array-of-struct.md)
- [Session 101 — cc Phase 3 part 5: struct value assignment](docs/88-cc-struct-value-assign.md)
- [Session 100 — cc Phase 3 part 4: multi-file compilation](docs/87-cc-multi-file.md)
- [Session 99 — cc Phase 3 part 3: sizeof + scaled pointer arith](docs/86-cc-sizeof-and-scaled-pointers.md)
- [Session 98 — cc Phase 3 part 2: function pointers](docs/85-cc-function-pointers.md)
- [Session 97 — cc Phase 3 part 1: structs](docs/84-cc-structs.md)
- [Session 96 — cc Phase 2 part 6: compound operators](docs/83-cc-compound-ops.md)
- [Session 95 — cc Phase 2 part 5: preprocessor](docs/82-cc-preprocessor.md)
- [Session 94 — cc Phase 2 part 4: printf](docs/81-cc-printf.md)
- [Session 93 — cc Phase 2 part 3: globals](docs/80-cc-globals.md)
- [Session 92 — cc Phase 2 part 2: char + pointers + arrays](docs/79-cc-chars-pointers.md)
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

**Path B — Self-hosting is complete** as of Session 121. Session 90 (Phase 1) shipped a 1500-line C-subset compiler — int-only; see [`docs/77-tinycc.md`](docs/77-tinycc.md). Sessions 91–96 (Phase 2) added string literals + `puts`/`print_str`, char + pointers + arrays + `&` / `*`, global variables, `printf` (compile-time-dispatched intrinsic with `%d`/`%s`/`%c`/`%x`/`%%`), the preprocessor (`#define` / `#undef` / `#include` / `#ifdef` / `#ifndef` / `#else` / `#endif` with classic header-guard support), and compound operators (`+=` / `-=` / `*=` / `/=` / `%=` / `++` / `--` / ternary `?:`). Sessions 97–106 (Phase 3) layered on structs (with `.` / `->` / linked-list-style pointer fields / struct-pointer params), function pointers, `sizeof(TYPE)` + scaled pointer arithmetic, multi-file compilation, struct value assignment via `rep movsd`, array-of-struct + indexed member access, `enum`, `typedef`, real user-defined variadic functions, and struct-by-value function arguments. Session 121 (Phase 4 capstone) ships the last three language items: struct-by-value RETURNS (hidden-first-arg cdecl ABI), `static` / `extern` storage-class keywords, and the `typedef RET (*NAME)(ARGS);` function-pointer typedef syntax — see [`docs/108-pathB-capstone.md`](docs/108-pathB-capstone.md). Session 125 follows up with four optimization passes (smart register-allocator codegen, constant folding, rolling peephole, DCE) that shrink cc's output by ~7.5% — see [`docs/112-pathB-optimizations.md`](docs/112-pathB-optimizations.md).

**Path C — Graphics is started** as of session 107. Userspace can now take ownership of the VBE framebuffer, get it mapped into its address space, and write pixels directly. `gfx.elf` paints a test card; fbcon mutes while a task owns the FB and resumes on release. Follow-ups: software drawing lib (108), mouse driver (109), double-buffer (110), window manager daemon (111).

**Path E — Drivers is the active path.** Phase 1 (session 118) landed virtio-blk (paravirtualized block, slots into the existing `blkdev` table as `vblk0`), virtio-net (paravirtualized NIC, falls back from RTL8139 in `net_init`), USB CDC-ACM (the "USB serial port" class — Arduino/ESP32 dongles work via `-device usb-host` passthrough), and `aplay.elf` (userspace PCM/WAV streamer that feeds the existing AC97 codec via `SYS_AUDIO_PLAY`) — see [`docs/105-pathE-drivers.md`](docs/105-pathE-drivers.md) for the legacy-virtio gotcha where capping qsize silently breaks every request. Phase 2 (session 119) added virtio-rng (entropy with `SYS_GETRANDOM` + `rand`), virtio-console (second serial via `hvc`), and virtio-balloon (cooperative memory pressure via `balloonctl`) — see [`docs/106-pathE-more-virtio.md`](docs/106-pathE-more-virtio.md).

Remaining candidate paths:
- **Path B further optimization** — session 125 shipped reg-alloc, const-fold, peephole, and DCE. More room left: a real Sethi-Ullman register allocator using ECX/EDX, peephole patterns for `mov [mem]; push eax → push [mem]`, common-subexpression elimination, or a real `tcc` port for full-C support.
- **Path C 108+** — drawing library, mouse, window manager (active path).
- **Path E — Drivers extension** — sessions 118–123 covered virtio-blk/net/rng/console/balloon/9p (read+write+rename) + USB CDC-ACM + AC97 consumer + WSL build path + IRQ-driven virtio + e1000 NIC + AHCI. Still candidate: virtio-scsi, USB EHCI (USB 2.0), USB CDC-ECM, full TTY integration of CDC-ACM, AHCI IRQ + NCQ.

## License

(Not yet specified. Treat as all-rights-reserved until a license file lands.)

## Acknowledgements

Built with help from Claude (claude.com/claude-code) acting as the engineering pair. The session deep-dives in `docs/` are the audit trail of that collaboration.
