# Session 137 — Path B: tcc UX polish

Session 135 verified that the cross-compiled tcc fully works inside
AdventOS, but the invocation surface was still raw:

```
tcc -static -nostdlib -Wl,-Ttext=0x40000000 my.c -o my.elf
```

…and the user had to hand-write `_start` and inline raw `int $0x80`
syscalls because there was no auto-linked startup code or libc. This
session closes that gap. The goal:

```
tcc /hello.c -o /myhello.elf
```

…where `hello.c` is the stock:

```c
#include <stdio.h>
int main(void) {
    printf("hi from tcc-compiled program\n");
    return 0;
}
```

Both ends now work — verified by [`smoke_tcc_polish.py`](../smoke_tcc_polish.py).

## What this required

### A wrapper at `/tcc.elf`

The raw cross-compiled binary moved from `/tcc.elf` to `/tccraw.elf`.
The new `/tcc.elf` is a tiny userland program ([`user/tcc.c`](../user/tcc.c))
that `sys_exec`s `/tccraw.elf` with AdventOS-default flags prepended:

```
/tccraw.elf -static -nostdlib -Wl,-Ttext=0x40000000 \
            /tcc/lib/start.c /tcc/lib/libuser.c \
            <user-args>
```

The wrapper scans the user's argv for escape-hatch flags (`-c`, `-E`,
`-v`, `-h`, `-r`, `-nostdlib`, `-nostartfiles`) and skips the
linker-time defaults when any of those is present, so existing flows
like `tcc -c foo.c -o foo.o` still produce a plain object file
without the runtime files being dragged in.

### Cross-compiling tcc with `-DCONFIG_TCCDIR='"/tcc"'`

tcc's CONFIG_TCC_SYSINCLUDEPATHS is `"{B}/include"` where `{B}` is
the runtime value of `tcc_lib_path`, set from CONFIG_TCCDIR at build
time. Baking in `/tcc` means the in-AdventOS tcc auto-searches
`/tcc/include` for `<stdio.h>` etc. without any `-I` flags.

**Gotcha bug (MSYS path mangling):** bash on MSYS rewrites bare
`/tcc` strings to Windows paths. The first attempt baked
`C:/Program Files/Git/tcc` into the binary — discovered via
`strings tcc/_obj/tcc.bin | grep tccdir`. Fix: prefix the gcc
invocation with `MSYS_NO_PATHCONV=1`. After that the embedded
string is the literal `/tcc`.

### A `/tcc/lib` + `/tcc/include` directory tree on the FS

Session 137 adds two nested directories under a new `/tcc` root via
`mkfs.py` DIRECTORIES. Shipped inside:

| Path | Source | Purpose |
|------|--------|---------|
| `/tcc/lib/start.c` | `fs/_tccrt_start.c` | Hand-asm `_start` — converts kernel stack frame to cdecl `main(argc, argv)` + forwards exit code through `sys_exit` |
| `/tcc/lib/libuser.c` | `user/libuser.c` | The whole AdventOS user runtime — sys_* syscall wrappers + libc.bin trampolines for printf / malloc / etc. |
| `/tcc/include/libuser.h` | `user/libuser.h` | Header for the above |
| `/tcc/include/adventos-libc.h` | `tcc/adventos-include/adventos-libc.h` | Re-exports libuser.h + adds the typedefs / macros / inline stubs tcc references but AdventOS doesn't directly provide |
| `/tcc/include/{stdio,stdlib,string,...}.h` | `tcc/adventos-include/*.h` | Stub system headers — each just `#include "adventos-libc.h"` |
| `/tcc/include/{stdarg,stddef,stdbool,float,...}.h` | `tcc/include/*.h` | tcc's own runtime headers (va_list, size_t, etc.) — required because we set `-nostdinc` for the cross-build |

20+ new files total — see "FS table bump" below.

### libuser.c needed two #ifdef guards for tcc

Mingw GCC and tcc differ in two i386 symbol conventions:

1. Mingw auto-emits `call ___main` at the start of every `main()`
   function (a libgcc hook for C++ static constructors). tcc emits
   no such call. So the libuser-provided `__main` stub is wrapped
   in `#ifndef __TINYC__`.

2. Mingw prepends `_` to every C symbol name; ELF gcc does not.
   libuser's signal-handling block hand-rolls
   `.global _sigreturn_tramp` in asm to match the mingw underscore;
   under tcc that asm label would be `_sigreturn_tramp` while
   `extern void sigreturn_tramp(void)` expects bare
   `sigreturn_tramp` — link mismatch. Wrapped the asm block in
   `#ifndef __TINYC__` and stubbed `sigaction`/`signal` to no-ops
   in the tcc branch. Programs that need real signal handling can
   hand-roll their own sigaction wrapper.

### `%*s` in libc/stdio.c

tcc uses `printf("%s %*s%s\n", ...)` for `-v` / `-vv` indent output.
Our libc's printf only knew `%0Nd` / `%0Nx` for widths — `%*s` (width
from a va_arg int) was unrecognized and the parser fell through,
emitting `%*s` literally and leaving the va_list misaligned. The
misalignment then page-faulted in the next `%s` because the int
width got interpreted as a `char *`.

Added a `*` branch in libc/stdio.c's `do_format`: consume an
int from va_args, treat as the width, and right-pad (space-fill the
left side) for `%s`. Same width path also reused for the existing
`%x` / `%X` zero-pad codepath.

### FS table bump 160 → 192

The 25-ish new runtime files pushed the entry count from ~150 to
~182, over the previous `FS_MAX_FILES = 160` cap. Bumped to 192,
which also required `FS_SUPER_SECTORS = 11 → 13` (192 × 32 = 6144 B
entry table + 512 B header = 6656 B = 13 sectors). Old fs.img files
are incompatible; build.sh creates a fresh one each run so this is
internal-only churn.

## What this enables

- `tcc /hello.c -o /myhello.elf` on stock printf-using source. The
  built `/myhello.elf` runs cleanly and prints the expected line.
- `tcc -c foo.c -o foo.o` still produces a plain ELF object (no
  startup code, no libuser dragged in).
- `tcc -E foo.c` preprocesses normally.
- `tcc -v` / `tcc -h` forward to the underlying tccraw.

Three smoke tests cover the surface:
- [`smoke_tcc_polish.py`](../smoke_tcc_polish.py) — the new
  end-to-end "stock hello.c" verification
- [`smoke_tcc.py`](../smoke_tcc.py) — the 6-step session-135 sweep
- [`smoke_fwtest.py`](../smoke_fwtest.py) — regression for the FILE *
  layer (still passes after the libuser ifdef changes)

## Files touched

- `user/tcc.c` — new (993 bytes once compiled) wrapper.
- `user/libuser.c` — two #ifndef __TINYC__ guards.
- `libc/stdio.c` — `%*s` width-from-arg support in `do_format`.
- `kernel/fs.h` + `mkfs.py` — FS_MAX_FILES 160→192, FS_SUPER_SECTORS 11→13.
- `mkfs.py` — `/tcc`, `/tcc/lib`, `/tcc/include` dirs + 24 runtime/header entries.
- `build.sh` — `MSYS_NO_PATHCONV=1` + `-DCONFIG_TCCDIR='"/tcc"'`.
- `fs/_tccrt_start.c` — new, 21 lines of asm.
- `fs/_tccrt_hello.c` — new, the stock-printf smoke target.
- `smoke_tcc_polish.py` — new end-to-end UX smoke.
- `docs/122-pathB-tcc-ux-polish.md` — this file.
- `README.md` — current-session pointer.

## Genuine follow-ups

- `tcc /foo.c` (no `-o`) currently outputs to `a.out` per tcc default;
  may or may not be what AdventOS users expect (host bash convention
  is `./a.out`). Could add a wrapper override to default `-o $(basename foo).elf`.
- Compiling `libuser.c` from source on every invocation is ~50 KB of
  C lex/parse work. Building it once into `/tcc/lib/libuser.o` and
  linking the .o would speed the per-program path up significantly.
  Same for `start.c` (smaller savings, but free).
- tcc's auto-link of `crt1.o` / `crti.o` / `libc.a` etc. is still
  side-stepped by passing `-nostdlib` in the wrapper. Could provide
  AdventOS-flavored versions of those files in `/tcc/lib/` so plain
  `tcc -static` works too.
- libuser's signal handlers are stubbed under tcc. Programs that
  need real sigaction would need to copy + adapt the mingw asm
  block.
