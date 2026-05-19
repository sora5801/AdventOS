# Session 135 — Path B: tcc fully works inside AdventOS

Session 134 (Phase 2) shipped a 386 KiB `tcc.elf` and a smoke that
reported `tcc -v`, `tcc -h`, and `tcc -E` all worked, with `tcc -c`
listed as a "named follow-up — wedges in the codegen→write path."

This session was supposed to debug that wedge. Instead it discovered
the wedge wasn't real: **tcc -c, the full link path, and running
tcc-emitted binaries all work end-to-end inside AdventOS**. The Phase 2
smoke had a `wait_for` bug that made `-c` look stuck when it had
actually finished.

## What this session actually changed

- Rewrote [`smoke_tcc.py`](../smoke_tcc.py) to use the kernel's
  `[user task pid=N exited code=M]` line as the per-command "task
  finished" marker. The old smoke watched for the shell prompt
  `advent$ ` (with space), which is unreliable across rapid command
  boundaries because the shell echoes the next typed command as soon
  as it has buffered input, sometimes before the prompt repaints.
- Added a 6-step verification flow:
  1. `tcc -v` prints the version banner
  2. `tcc -h` prints usage
  3. `tcc -E /thello.c` preprocesses
  4. `tcc -c /thello.c -o /thello.o` compiles to a 1044-byte i386
     ELF object — byte-identical to host `tcc.exe -c` output
  5. `tcc -static -nostdlib -Wl,-Ttext=0x40000000 /thello2.c -o /thello2.elf`
     fully links a hello-world program with its own `_start` into an
     856-byte executable
  6. Running `/thello2.elf` from the shell — kernel ELF loader accepts
     it (two PT_LOAD segments + GNU_RELRO), `_start` runs,
     `sys_write_fd(1, "tcchi\n", 6)` and `sys_exit(0)` both work,
     `[user task ... exited code=0]`.

- Added [`fs/thello2.c`](../fs/thello2.c): a hello program with a
  hand-written `_start` that talks to AdventOS via raw `int $0x80`.
  Linked binary works without any AdventOS startup helpers.
- Added [`user/fwtest.c`](../user/fwtest.c) +
  [`smoke_fwtest.py`](../smoke_fwtest.py) — the standalone
  regression test for the FILE * write path (`open` →
  `fdopen` → `fwrite` mix → `fclose` → `close`). Useful as a sanity
  check before assuming the issue is in tcc; it was the second
  thing I ran during this session and confirmed the issue was
  *not* in libc.

## How the misdiagnosis happened

The Phase 2 smoke ran tcc multiple times against the same serial
socket with a shared `buf`. Between commands it did `buf = b""`, but
because the QEMU shell echoes the typed command back BEFORE running
it, by the time the smoke's `wait_for(b"advent$ ", buf, timeout=180)`
loop started reading after the second `sendall`, the previous prompt
+ the freshly echoed `advent$ /tcc.elf -c ...` was already arriving.
The echo's leading `advent$ ` matched the marker; the smoke decided
the command had returned immediately. But then nothing else matched
the marker for the next 180 s — because **the buffer that contained
the matching prompt had already been read**, and tcc's actual exit
left only `[user task pid=N exited code=0]\nadvent$` (the new prompt
echo follows the NEXT typed character, which never came).

The fix is to wait for a marker that the kernel emits exactly once per
finished task: `exited code=`. Each child process produces one such
line at termination, regardless of shell state.

## What's actually working

| Operation | Status |
|-----------|--------|
| `tcc -v` (print version) | ✅ |
| `tcc -h` (print usage) | ✅ |
| `tcc -E foo.c` (preprocess) | ✅ |
| `tcc -c foo.c -o foo.o` (compile to ELF object) | ✅ — 1044 B, byte-identical to host tcc |
| `tcc -static -nostdlib /foo.c -o /foo.elf` (full link) | ✅ — 856 B Linux ELF |
| Running tcc-emitted ELF via AdventOS loader | ✅ — multi-PT_LOAD accepted |
| `_start` defined in user source | ✅ — runs cleanly |
| `int $0x80` syscalls from tcc-emitted code | ✅ |

The kernel ELF loader at `kernel/elf.c` is actually more forgiving
than the Phase 2 doc claimed: it iterates ALL `PT_LOAD` segments and
maps each one separately. GNU_RELRO and NULL segments are silently
ignored. So tcc's default Linux ELF layout works without any output-
format adaptation.

The Phase 2 audit was overstated. cc and tcc now coexist as Path B
compilers; the user picks based on what features they need.

## Files touched

- `smoke_tcc.py` — rewritten to use the kernel's "exited code="
  marker. 6-step verification.
- `fs/thello2.c` — new, minimal hello with own `_start`.
- `user/fwtest.c` — new, FILE * write-path regression test.
- `smoke_fwtest.py` — new, runs fwtest inside QEMU.
- `mkfs.py` — ships thello2.c + fwtest.elf in the FS.
- `build.sh` — builds fwtest.elf as a normal user program.
- `docs/121-pathB-tcc-fully-working.md` — this file.
- `README.md` — current-session pointer + entry in Recent deep dives.

## Open work for future sessions (genuine)

- Make tcc emit AdventOS-default single-PT_LOAD output without
  requiring `-static -nostdlib -Wl,-Ttext=0x40000000`. Add a
  `-mode=adventos` mode to tcc that defaults all three.
- Provide a small `start.o`-equivalent for tcc so user programs
  don't need to hand-write `_start`. Linked in by default with a
  `-mode=adventos` flag.
- Make tcc's default include path point at `/tcc/include` inside
  AdventOS so `#include <stdarg.h>` etc. work without `-I` flags.
  Ship a copy of tcc's `include/` tree at `/tcc/include`.
- Make `printf` etc. work in tcc-emitted programs by linking against
  libuser. Currently `_start` has to make raw syscalls.

These are the actual Phase 3 items, not the imaginary ones from the
Phase 2 doc.
