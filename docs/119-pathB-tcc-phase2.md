# Session 132 — Path B: tcc port, Phase 2 (running inside AdventOS)

Session 131 vendored TinyCC at `tcc/` and got a host-only build wired
up. This session does the much-harder middle part of the port: take
that same source and produce a `tcc.elf` that loads inside AdventOS
and runs.

The headline outcome is a 386 KiB `tcc.elf` that boots, prints its
version banner, prints its full help text, and preprocesses C source
files (`tcc -E /thello.c` emits the standard `# N "file"` line markers
plus the post-#include text). The remaining piece — emitting an
AdventOS-loadable object/executable from `-c` / link output — wedges
the task somewhere in the codegen→write path and is the named
follow-up for the next session.

## What landed

### libuser trampolines for the existing libc.bin surface (commit 1)

libc.bin already implemented ~35 functions but libuser only forwarded
~17 of them. The first commit adds straight pass-through trampolines
for the rest: `strcpy`, `strncpy`, `strcat`, `strrchr`, `strstr`,
`memmove`, `memchr` (string); `atol`, `strtol`, `abs`, `calloc`,
`realloc` (stdlib); `isalpha`, `isdigit`, `isspace`, `isalnum`,
`isupper`, `islower`, `toupper`, `tolower` (ctype); plus varargs
shims for `sprintf` / `snprintf` (forwarding to the existing
`vsprintf_` / `vsnprintf_` cores) and `vsprintf`/`vsnprintf` for
forwarding from another varargs function. No new libc.bin exports
yet — the trampolines just plumb through existing ones.

### FILE * + stdio layer in libc.bin (commit 2)

The biggest new chunk. `libc/file.c` implements the full POSIX-ish
stdio surface tcc needs: `fopen`, `fclose`, `fread`, `fwrite`,
`fseek`, `ftell`, `fputs`, `fputc`, `ferror`, `feof`, `remove`,
`fflush`, `fgets`, `fgetc`. A `vfprintf_` shim funnels printf-family
calls through the existing `vsnprintf_` formatter.

AdventOS has no kernel seek primitive, so `FILE *` is buffer-backed:

- `fopen(path, "r")`: `sys_fs_size` to learn the byte count, malloc
  a buffer, `sys_open` + `sys_read` the whole file in one shot,
  `sys_close`. All subsequent `fread` / `fseek` / `ftell` operate
  on the in-RAM buffer.
- `fopen(path, "w")`: allocate an initial 4 KiB growable buffer; all
  `fwrite` / `fputc` / `fputs` / `fprintf` calls append, with the
  buffer doubling when full. `fclose` flushes the whole buffer via
  `sys_fs_write` — one shot, replaces the file contents.

`stdin` / `stdout` / `stderr` are SENTINEL POINTER values `(FILE *)1`,
`(FILE *)2`, `(FILE *)3` — they don't reference real FILE entries.
Every write path checks the low-valued pointer first and dispatches
straight to `sys_write_fd` with `fd = (uintptr_t)stream - 1`. tcc's
heavy use of `fprintf(stderr, ...)` for diagnostics works without
allocating a real FILE struct.

Limits: 16 simultaneous open FILEs; max read-file size 4 MiB. The
biggest source file in AdventOS today is `kernel/kernel.c` at ~100 KiB,
well inside that cap.

`LIBC_VERSION` bumps 1 → 2; new export indices populate the previously-
unused slots (38, 45–49, 54–63). No existing indices are reordered.

### Stragglers — qsort/strtoll/strerror + POSIX-fd + setjmp/longjmp (commit 3)

Three more functions go into libc.bin: `qsort` (Lomuto partition,
recursive — tcc only ever sorts small case-value arrays), `strtoll`
(64-bit cousin of `strtol`), and `strerror` (returns a single
"I/O error" string — AdventOS has no per-syscall errno yet).

Everything else lives in `user/libuser.c` because it's either
process-state (errno, jmp_buf, environ, the fake-fd table) or a
trivial wrapper around an existing syscall:

- `exit(code)` → `sys_exit(code)`
- `abort()` → `sys_exit(134)` (POSIX shells' SIGABRT exit convention)
- `errno` — single global int
- `environ` — empty array; tcc references it for diagnostics
- `time(time_t *)` and `gettimeofday(struct timeval *, void *)` —
  wrap `sys_time`. Second-resolution, `tv_usec` always 0.
- `getenv` → NULL, `system` → -1 (deliberate stubs)
- `unlink(path)` → `sys_unlink(path)`
- `setjmp` / `longjmp` — raw asm, i386 ABI, jmp_buf = 6 ints
  (ebx, esi, edi, ebp, esp, eip). Standard textbook implementation.

The fiddliest piece is the **buffered POSIX-fd layer**. tcc opens
files via raw `open(path, O_RDONLY)` and uses `lseek` to jump around
the buffer, but AdventOS has no kernel seek primitive. The fix is a
small per-process 16-entry fake-fd table in libuser's `.data`:

- `open(path, O_RDONLY)`: load the full file into a malloc'd buffer
  at open time (same trick as fopen), return a fake fd in
  `[100, 116)`.
- `read(fd, ...)`, `lseek(fd, ...)`, `close(fd)`: dispatch on the fd
  value. Fake fds (≥100) operate on the buffer; real kernel fds
  (<100) pass through to `sys_read` / `sys_close`.
- `open(path, O_WRONLY|O_CREAT|O_TRUNC)`: pair with `fopen(path, "w")`
  so the fake fd shares a FILE * with the stdio layer. `fdopen(fd, "wb")`
  hands the same FILE * back.

That last bit is what makes tcc's output path work: tcc does
`fd = open(file, O_WRONLY|O_CREAT|...)` immediately followed by
`f = fdopen(fd, "wb")` and writes the ELF through `f`. Both pointers
target the same growing buffer.

### Cross-compile tcc.elf as an AdventOS user program (commit 4)

The actual cross-compile. Three pieces:

**1. Stub system headers** — `tcc/adventos-include/` ships ~25 empty
shims for `stdio.h`, `stdlib.h`, `string.h`, `unistd.h`, `sys/stat.h`,
etc. Each one re-includes the unified `adventos-libc.h`, which itself
pulls in `user/libuser.h` plus everything tcc references that isn't
already there: ptrdiff_t / uintptr_t / struct tm / struct stat, the
INT_MAX / LONG_MAX / PATH_MAX / etc. limits.h constants, errno value
names, `localtime` / `getcwd` / `realpath` / `freopen` / `execvp` /
`strpbrk` / `strtoul` / `strtoull` / `ldexpl` / `mprotect` / `mmap` /
`strtof` / `strtold` stubs. The unified header is pre-included via
gcc's `-include` flag.

**2. softfp stubs** — `tcc/adventos-include/softfp_stubs.c`. Two
buckets:
  - Real implementations of `__divdi3`, `__moddi3`, `__udivdi3`,
    `__umoddi3`, `__ashldi3`, `__ashrdi3`, `__lshrdi3` (adapted from
    `tcc/lib/libtcc1.c`). These are necessary because gcc compiling
    tcc emits 64-bit-int helper calls for parsing-time integer math
    on i386.
  - Zero-returning stubs for every long-double / soft-float helper
    gcc emits when compiling tcc's float-literal arithmetic in
    tccpp.c: `__addxf3`, `__mulxf3`, `__extenddfxf2`, `__truncxfdf2`,
    the comparison family, etc. AdventOS user programs are integer-
    only, so these never execute; they just satisfy the linker.

**3. build.sh wiring** — new `[5g/7]` step, runs AFTER user programs
so the freshly-built `libuser.o` reflects any libc-surface changes.
The cross-compile command is:

```bash
gcc "${USER_CFLAGS[@]}" -nostdinc \
    -U_WIN32 -U_WIN64 -U__WIN32__ -U__WIN32 -U__MINGW32__ -U__MINGW64__ \
    -DTCC_TARGET_I386 -DONE_SOURCE -DCONFIG_TCC_STATIC=1 \
    -DCONFIG_TCC_PREDEFS=1 -DCONFIG_TCC_SEMLOCK=0 \
    -DCONFIG_TCC_BACKTRACE=0 \
    -include tcc/adventos-include/adventos-libc.h \
    -Iuser -Itcc/adventos-include -Itcc -Itcc/include \
    -c -o tcc/_obj/tcc-cross.o tcc/tcc.c
```

`CONFIG_TCC_SEMLOCK=0` skips the pthread `sem_t` dependency (AdventOS
is single-threaded for now). `CONFIG_TCC_BACKTRACE=0` drops the
signal-handler-based crash dump path. `CONFIG_TCC_STATIC=1` keeps
tcc from referencing `dlopen` at link time.

The link is the standard AdventOS user-program shape:

```bash
ld -m i386pe -T user/user.ld -o tcc/_obj/tcc.elf \
    user/_obj/start.o tcc/_obj/tcc-cross.o user/_obj/libuser.o \
    tcc/_obj/softfp_stubs.o
objcopy -O binary -j .text -j .rdata -j .data \
    tcc/_obj/tcc.elf tcc/_obj/tcc.bin
```

`tcc.bin` is 386 KiB; `mkfs.py` wraps it in the standard 84-byte
ELF32 envelope at entry 0x40000000 and ships it at `/tcc.elf` in
the FS.

## Smoke test — what works inside the OS

[`smoke_tcc.py`](../smoke_tcc.py) boots QEMU and exercises three
operations on `/tcc.elf`. With the os.img produced by this branch:

| Command | Result |
|---------|--------|
| `/tcc.elf -v` | Prints `tcc version 0.9.28rc-adventos (i386 Linux)`, exits 0 |
| `/tcc.elf -h` | Prints full ~50-line usage text, exits 0 |
| `/tcc.elf -E /thello.c` | Preprocesses; emits `# 1 "/thello.c"` line markers + the source. Exits 0 |
| `/tcc.elf -c /thello.c -o /thello.o` | Hangs after 180 s. tcc starts the compile but never returns. |

So roughly two-thirds of tcc's normal flow runs end-to-end inside
AdventOS. The startup path, argument parser, version/help printing,
preprocessor, and the libc trampolines (printf-family, file I/O,
malloc, strXXX) all work. The wedge is somewhere in the codegen or
ELF-emit stage.

## Known follow-ups (next session)

**1. The `-c` hang.** Most likely candidates:

- AdventOS's user-task stack is small (a few KiB) and tcc's recursive
  parser hits a stack overflow that doesn't produce a clean exit-code
  diagnostic. Bumping the user stack to 64 KiB or moving the recursion
  to an explicit work-list would fix it.
- `malloc` thrashing — tcc allocates many small chunks; the
  first-fit free-list in `libc/stdlib.c` may be O(N) per allocation
  and gets unusably slow once N grows. Switching to a binned allocator
  (or just preallocating tcc's symbol-table arena) would help.
- A real bug in one of our libc shims (likely `fwrite` to a write-
  mode FILE *, since `-c` is the first command path that exercises
  that). The grow-on-write logic looks right but is the most complex
  piece of new code.

Adding a `tcc -bench` invocation that reports stats inside AdventOS
will tell us at what stage the hang happens.

**2. ELF output format adaptation.** Even when `-c` works, the
resulting `.o` / `.elf` files will be standard Linux ELF (multiple
PT_LOADs, a GNU_RELRO segment, a NULL segment). AdventOS's
`kernel/elf.c` loader only honours a single PT_LOAD with entry
0x40000000. Two options:

- Patch `tccelf.c::tcc_output_elf` to emit AdventOS's stricter layout.
  Self-contained but invasive — about 200 lines of code touched.
- Post-process the tcc output: read tcc's ELF, extract `.text + .rdata
  + .data`, re-emit using the same `mkfs.make_elf()` envelope. Smaller
  change, lives outside the vendored tree.

The post-process route is cleaner for keeping future tcc upstream
syncs easy.

**3. tcc-emitted binaries need the AdventOS `_start` prologue.**
`start.S` from `user/` calls `main` and forwards the return value
to `sys_exit`. tcc-emitted binaries currently don't link `start.o`;
either tcc's driver needs to inject it, or we provide a small
`libtcc1` archive that includes a compatible `_start`.

**4. tcc-emitted binaries call libuser/libc functions at runtime.**
For tcc-compiled programs to use `printf` etc., they need to link
against `libuser.o` (and libc.bin loads automatically via dyld).
Whether tcc's `-llibuser` flag wiring works correctly is unverified.

## Files touched

- `libc/file.c` — new, 359 lines. FILE * implementation.
- `libc/libc.h` — new export indices + struct FILE + stdio prototypes.
- `libc/exports.c` — table entries for new functions.
- `libc/stdlib.c` — added qsort + strtoll.
- `libc/string.c` — added strerror.
- `libc/ctype.c` — added isxdigit.
- `user/libuser.c` — ~400 lines added: trampolines, POSIX-fd layer,
  exit/abort/errno/environ/time/gettimeofday/getenv/system/unlink,
  setjmp/longjmp asm.
- `user/libuser.h` — matching prototypes + sentinel defines for
  stdin/stdout/stderr + jmp_buf typedef + struct timeval.
- `tcc/adventos-include/*.h` — ~25 stub system headers.
- `tcc/adventos-include/adventos-libc.h` — unified header with the
  type / macro / stub definitions tcc expects.
- `tcc/adventos-include/softfp_stubs.c` — 64-bit int helpers + FP
  stubs.
- `build.sh` — new `[5g/7]` cross-compile step.
- `mkfs.py` — ship `tcc.elf` and `thello.c` in the FS.
- `fs/thello.c` — minimal smoke source.
- `smoke_tcc.py` — Python smoke test harness.
- `docs/119-pathB-tcc-phase2.md` — this file.
- `.gitignore` — `tcc/_obj/` build artifacts.
- `README.md` — current-session pointer.
