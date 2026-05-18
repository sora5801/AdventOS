# Session 131 — Path B: tcc port, Phase 1 foundation

After session 128 closed out the cc language corners (and made it
clear cc has stopped being a useful place to extend the C surface
much further), the next move on Path B is to port **TinyCC** so
AdventOS gets a real C compiler with float/double, `long long`,
full type system, function-like macros, etc. — everything that's
out of cc's reach by design.

The end state is ambitious — `tcc /hello.c -o /hello.elf` running
inside the OS — and genuinely multi-week. This session lands the
**foundation**: vendor tcc, strip it down to a working i386-only
subset, verify it builds on the host, document the libc surface
that Phase 2 has to fill in.

Status:
- ✅ tcc 0.9.28rc vendored at `tcc/`
- ✅ Non-i386 backends stripped (ARM, ARM64, C67, RISC-V, x86_64)
- ✅ Non-ELF output stripped (PE/COFF, Mach-O, TI COFF)
- ✅ Host build wired into build.sh (step `[5f/7]`)
- ✅ Host-built `tcc/tcc.exe` is ~680 KiB, recognizes itself as `tcc
   version 0.9.28rc (i386 Linux)`
- ⏳ Phase 2 — cross-compile + libuser shims + ELF output adaptation
   — deferred to next session(s)

## What landed

### `tcc/` — vendored source tree

`git clone https://repo.or.cz/tinycc.git` followed by aggressive
trimming. Down from 45 to 22 .c/.h files, ~1.8 MB. Removed:

| Removed | Reason |
|---------|--------|
| `arm-*`, `arm64-*`, `c67-*`, `riscv64-*`, `x86_64-*` | non-i386 backends |
| `il-gen.c`, `il-opcodes.h` | .NET IL target |
| `coff.h`, `tcccoff.c` | TI C67 COFF |
| `tccpe.c` | Windows PE/COFF output |
| `tccmacho.c` | Apple Mach-O output |
| `win32/` | Windows port |
| `tests/` | 1.2 MB test corpus |
| `*.texi` | needs makeinfo to render |

What remains is the core compiler (tcc.c / libtcc.c / tccpp.c /
tccgen.c / tccelf.c / tccasm.c / tccdbg.c / tccrun.c / tcctools.c),
the i386 backend (i386-gen.c / i386-link.c / i386-asm.c / *.h),
shared headers (elf.h / dwarf.h / stab.h / tcctok.h), the
`include/` subtree with tcc-shipped C headers, and `lib/` (libtcc1
runtime helpers like `__divdi3` for 64-bit arithmetic on i386).

A new `tcc/README.AdventOS` documents what was removed, the libc
audit, and the Phase 2 plan.

### Host build (`[5f/7]` in build.sh)

`tcc.c` does an internal `#include "libtcc.c"` which transitively
pulls in every other source file when `-DONE_SOURCE` is set. Plus
`-DTCC_TARGET_I386` to select the i386 backend. Plus a one-off
helper step that builds `c2str.exe` from `conftest.c` and uses it
to convert `include/tccdefs.h` into `tccdefs_.h` (a C string
literal embedded in the binary).

```
[5f/7] build host tcc (Phase 1 — i386 ELF cross-compiler)
        tcc.exe = 685623 bytes (i386 cross-compiler)
```

Skipped silently if `tcc/` isn't present, so existing builds aren't
slowed down by an extra 680 KiB binary unless the user opts in.

The host `tcc.exe` won't compile-and-run programs directly
(`-run` is unavailable in cross-compilers), but it CAN parse C and
emit i386-Linux ELF object/executable files — exactly what we'll
use offline for Phase-2 bring-up while the in-AdventOS port matures.

---

## Phase 2 audit — libc surface tcc needs

tcc heavily relies on libc. The functions it actually calls
(grepped from tcc.h / tcc.c / libtcc.c / tccpp.c / tccgen.c /
tccelf.c):

| Function | libuser status | Phase 2 work |
|----------|---------------|--------------|
| `malloc`, `free`              | ✅ present | — |
| `memcpy`, `memset`            | ✅ present | — |
| `strlen`, `strcmp`            | ✅ present | — |
| `atoi`                        | ✅ present | — |
| `printf`                      | ✅ present | — |
| `exit`                        | ✅ present (`sys_exit`) | thin macro |
| `calloc`, `realloc`           | ❌ missing | ~40 lines (use malloc + memset / malloc + memcpy + free) |
| `fopen`, `fclose`             | ❌ missing | ~80 lines (FILE* wrapper around sys_open / sys_close + small fd→FILE table) |
| `fread`, `fwrite`             | ❌ missing | thin wrappers over sys_read / sys_fs_write |
| `fseek`, `ftell`              | ❌ missing | sys_lseek wrapper (add if missing kernel-side) |
| `fputs`, `fputc`              | ❌ missing | sys_write wrapper |
| `fprintf`                     | ❌ missing | reuse the printf core, dispatch to file descriptor |
| `sprintf`, `snprintf`         | ❌ missing | reuse printf core, write to buffer |
| `ferror`, `feof`              | ❌ missing | trivial flag in FILE struct |
| `remove`                      | ❌ missing | `sys_unlink` wrapper (exists) |
| `strcpy`, `strncpy`           | ❌ missing | ~10 lines each |
| `strcat`, `strncat`           | ❌ missing | ~12 lines each |
| `strncmp`                     | ❌ missing | ~12 lines |
| `strchr`, `strrchr`, `strstr` | ❌ missing | ~10-15 lines each |
| `memmove`                     | ❌ missing | ~15 lines (memcpy with overlap handling) |
| `memcmp`                      | ❌ missing | ~8 lines |
| `strdup`                      | ❌ missing | trivial (malloc + strcpy) |
| `strerror`                    | ❌ missing | switch on errno → string literal |
| `strtol`, `strtoll`           | ❌ missing | ~50 lines each (base-10/base-16/octal parsing) |
| `getenv`                      | ❌ missing | always return NULL — AdventOS has no env |
| `system`                      | ❌ missing | always return -1 — no shell exec from libuser |
| `abort`                       | ❌ missing | `sys_exit(1)` |
| `qsort`                       | ❌ missing | ~50 lines (recursive partition) |
| `isdigit`/`isspace`/`isalnum`/`isalpha`/`isupper`/`islower`/`isxdigit`/`tolower`/`toupper` | ❌ missing | trivial — table-based or comparison |
| `errno`                       | ❌ missing | global int |
| `setjmp`, `longjmp`           | ❌ missing | needed only for `-run` mode error recovery; we can drop `-run` or stub |
| `getcwd`, `access`, `sysconf` | ❌ missing | mostly used for include-path search; AdventOS has flat / |
| `stat`, `fstat`               | ❌ missing | `sys_fs_size` wrapper; only file-existence check matters |
| `time(NULL)`                  | ❌ missing | `sys_time` wrapper (exists) — used to seed `__DATE__` |

Rough estimate: **~30 functions to add, ~600-1000 lines of
libuser code**. Mostly mechanical (wrap an existing syscall or
implement standard algorithm). The fiddly ones are `fopen/FILE*`
(needs a small per-task FD table) and the printf-family
(`sprintf`/`snprintf`/`fprintf` reuse the same formatter core but
need different sinks).

## ELF output adaptation

Even with libc in place, tcc-emitted binaries won't immediately
run in AdventOS. tcc emits Linux ELF with a `.dynamic` segment, a
`PT_INTERP` pointing at `/lib/ld-linux.so.2`, and full ELF
relocations. AdventOS's `kernel/elf.c` loader only honors
`PT_LOAD` segments and expects:

- `e_entry = 0x40000000`
- Exactly one `PT_LOAD` covering `.text + .rdata + .data`
- No dynamic linking, no symbol table, no relocations to apply

The adaptation is in `tccelf.c::tcc_output_elf`. Phase 2 will pass
flags equivalent to `-static -Wl,-Ttext=0x40000000` and then patch
out the unused dynamic-link plumbing.

Alternative path (lighter): leave tcc's ELF output alone, but post-
process with the existing `objcopy -O binary` pipeline (already
used for `gcc -m32` → user programs). That's basically the
existing build flow with `tcc -c` swapped in for `gcc -c`.

## Why now

cc is ~80% of practical C in 6000 lines, and the remaining 20%
(floats, real type system, function-like macros) is where every
incremental cc session has been hitting diminishing returns. Each
of those features in cc would be a multi-day session that ends up
re-implementing a corner of standards-conformant C the same way tcc
already does. Vendoring tcc trades 50 KLOC of compiler code in the
tree for a one-time integration cost — after which AdventOS has a
real compiler permanently.

cc stays in place as the always-works backstop. tcc layers on top
when ready.

---

## Files touched

- `tcc/` — new vendored tree (22 .c/.h files + headers + lib + examples)
- `tcc/README.AdventOS` — vendoring notes + Phase 2 plan
- `build.sh` — `[5f/7]` host-tcc build step (opt-in, skipped if tcc/ absent)
- `docs/118-pathB-tcc-foundation.md` — this file
- `README.md` — pointer + current-session update
