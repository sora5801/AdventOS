# AdventOS v1.0.0 — first stable release

> **How to use this file:**
> Go to https://github.com/sora5801/AdventOS/releases/new, pick the
> `v1.0.0` tag from the dropdown, set the release title to
> `AdventOS v1.0.0 — first stable release`, and paste everything
> below the `---` into the description box.

---

A 32-bit i386 hobby OS, written from scratch in C across 171 sessions.
Boots on bare hardware and inside QEMU.  All five development paths
reached completion at v1.0.0.

## What's in v1.0.0

### Kernel + Unix surface

i386 protected-mode kernel, paging, PMM, free-list kmalloc,
preemptive priority-based scheduler, SMP (BSP + 1 AP, BKL +
per-resource locks), full POSIX-ish signals
(TERM/KILL/STOP/CONT/USR1+2/PIPE/CHLD), fork/exec/wait/pipe/dup2,
sandbox masks + per-task resource limits (RSS/CPU/wall/FDs),
AdventFS on-disk filesystem with directories + perms, block cache,
VFS abstraction, /proc filesystem, mmap on fd + lazy paging,
multi-user login (`/etc/passwd`-style).

### Networking + crypto

TCP/UDP, DHCP client, DNS resolver + cache, NTP client, TLS 1.3
(ECDHE-RSA + AES-128-GCM, real-world server interop), X.509 cert
chain validation against a CA root store, RSA-PKCS#1 v1.5 sign +
verify, ECDSA-P256.  In-guest servers: httpd, httpsd, sshd, ircd.
In-guest clients: nc, wget, telnet, irc, ssh, httpsget.

### Storage + drivers

ATA, AHCI SATA (IRQ-driven, NCQ with 32 in-flight slots), NVMe
(admin + I/O queue pairs, PRP-based DMA, READ/WRITE),
virtio-blk / virtio-scsi / virtio-net / virtio-rng /
virtio-console / virtio-balloon / virtio-9p (host-fs passthrough
with read/write/rename), e1000 / 82540EM gigabit NIC, USB UHCI +
EHCI 2.0 (480 Mbps full transfer path), USB HID keyboard, USB
Mass Storage (BOT + SCSI), USB CDC-ACM serial, USB CDC-ECM
Ethernet, AC97 audio + `aplay` userspace PCM/WAV streamer.

### Userland

- **Shell** — `sh.elf`, bash-compatible: pipes,
  `;`/`&&`/`||`/`>>`/`<`, glob, brace expansion `{a,b}` /
  `{1..N}`, `~` tilde, `$VAR`/`$?`/`$#`/`$@`/`$0..$9`,
  parameter forms `${var:-x}` / `${#var}` / `${var%suf}` /
  `${var/old/new}`, arithmetic `$((..))` / `((..))`, control
  flow (`if` / `for` / `while`), functions, builtins (`[`,
  `test`, `read`, `shift`, `break`, `continue`, `return`),
  `!!` / `!N` history recall, Ctrl-R reverse search,
  command/var/file tab completion, dynamic
  `advent<cwd>$ ` prompt.
- **Modal editor** — `vi.elf` (undo, count prefixes,
  search/replace, motions, modes).
- **Man pages** — 27 pages under `/man/`, `man <topic>` +
  `man -k WORD`.
- **Scripting** — `lua` (Lua-syntax subset, int32 numbers,
  tree-walking interpreter with closures, mark-sweep GC,
  multi-return).
- **Native compilers** — `cc` (C-subset from scratch:
  int / char / pointers / arrays / strings / globals /
  printf / preprocessor / compound ops / structs /
  function pointers / sizeof / scaled ptr arith /
  multi-file / enum / typedef / real variadics; emits ELF32)
  + `tcc` (vendored real TinyCC, runs inside the guest).
- **Coreutils** — ls, cat, cp, mv, rm, mkdir, rmdir, chmod,
  touch, find, head, tail, grep, sort, uniq, wc, tee, tr,
  seq, echo, date, ps, kill, pwd, id, man.
- **Debugger** — `dbg.elf`, ptrace-based.

### Window manager + apps

- `wmd` — software compositor with title bars, taskbar,
  workspaces (virtual desktops), Alt-Tab, snap-to-edge with
  ghost preview, USB tablet pointer, drop shadows, context
  menus, notifications.
- `wmterm` — terminal emulator: PTY-backed, 256-color ANSI,
  scrollback (PgUp/PgDn + mouse wheel), text selection +
  clipboard, italic/underline/strikethrough, mid-line caret,
  keyboard-driven selection (Shift+arrow), middle-click paste.
- `wmedit` — text editor (undo/redo/Ctrl-F search).
- `wmcalc` — calculator (decimal + memory keys).
- `wmpaint` — paint app (Ctrl-S to save PPM).
- `wmview` — image viewer with pan.
- `wmfiles` — file manager.
- `wmsysinfo` — system dashboard.
- `wmps` — process viewer.
- `wmclock` — clock with PST timezone.

### Agent layer

- `agentd` — JSON-RPC 2.0 + MCP server on `127.0.0.1:7000`,
  exposing the OS surface to external agents (or in-guest
  tools).
- Structured pipelines — `|>` operator with JSONL between
  stages, plus `--advjson` mode on builtins.
- Selftest harness — 8 in-guest selftests
  (sandbox / limits / kv / jobs / subscribe / cron /
  pipeline / smp-hammer), plus 67 host-driven smoke tests.

## Stability methodology

Stability is measured, not assumed.  Three audit passes (sessions
169, 170, 171) hardened the Path C smoke suite and built a
multi-run flake-audit harness with retry coverage.  Final state:
13 smokes in `smoke_flake_audit.py`, all green on 3-iteration
runs with up to 3 retries per iteration.

Methodology lives in:
- [docs/155 — Path C stability + flake hunt](docs/155-pathC-stability-flake-hunt.md)
- [docs/157 — broader flake audit](docs/157-broader-flake-audit.md)
- [docs/158 — v1.0 readiness assessment](docs/158-v1.0-readiness.md)

## Where to start reading

- [README](README.md) — quickstart, repo layout, run commands
- [docs/INDEX.md](docs/INDEX.md) — all 171 session docs grouped by
  topic + path
- [docs/agent-api.md](docs/agent-api.md) — agent JSON-RPC reference
- [docs/agent-cookbook.md](docs/agent-cookbook.md) — agent patterns

## Out of scope for v1.0

- **Other architectures** — i386 only.  No x86_64, ARM, RISC-V.
- **Kernel self-hosting** — neither `cc` nor `tcc` is wired to
  build `kernel/` from inside the OS.  Both can build small
  userland programs end-to-end.
- **xHCI / USB 3.0**, **virtio-gpu hardware accel**, **EHCI
  periodic schedule** (for true iso transfers).  Standalone
  follow-ups; none gate v1.0.
- **Real Lua features** — metatables, coroutines,
  capture-by-reference closures, string patterns, math lib.
  Path D shipped the subset that's useful for small scripts.

## License

[MIT](LICENSE).

## Acknowledgements

Built with help from Claude (claude.com/claude-code) acting as the
engineering pair.  The session deep-dives in `docs/` are the audit
trail of that collaboration.
