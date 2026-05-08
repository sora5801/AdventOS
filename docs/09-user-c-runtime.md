# Session 9 — Real userspace runtime: writing user programs in C

**Goal:** Stop writing user programs as naked PIC asm. Compile them as ordinary C with `printf`, globals, locals, and a `main()` — link against a tiny `libuser`, package as ELF, ship on disk, load via `exec`.

After session 8 the loader was real, but every "user program" was still a single naked function full of `call/pop` + `add $(msg-1b)` PIC arithmetic. That worked for printing strings; it was actively painful for anything that needed a loop with a variable, a struct, two strings, a function call, or a printf format.

This session builds the user side of the contract: a libc-equivalent (`libuser`), a `_start` stub, a linker script that lays everything out at `0x40000000`, and a build pipeline that produces real C-compiled user binaries.

## The user side of the contract

```
user/
  libuser.h         ← syscall wrappers + printf + mem*/str*
  libuser.c
  start.S           ← _start: call main, sys_exit(main's return)
  user.ld           ← link script: 0x40000000 entry, .startup first in .text
  hello.c           ← real C user program
  count.c           ← real C user program
```

Build steps per program:

```
cc start.S        → start.o
cc libuser.c      → libuser.o
cc <program>.c    → <program>.o
ld start.o + program.o + libuser.o + user.ld → program.elf
objcopy -O binary → program.bin
mkfs.py            → wraps program.bin in ELF32 → fs.img
```

Same toolchain as the kernel build (mingw-w64 GCC, PE/COFF intermediate, `objcopy -O binary` final).

## Why no .bss

Letting user programs have `.bss` would mean `filesz < memsz` in the ELF, which means the kernel loader has to zero-fill bytes the file doesn't cover. The loader can do that — it already does for the kernel's own usage. But it complicates `mkfs.py`: the wrapper would need to know each program's `.bss` size and emit a different `p_memsz` than `p_filesz`.

The cheap way out: compile every user file with

```
-fno-zero-initialized-in-bss   # uninitialized globals → .data
-fno-common                    # tentative defs → .data, not COMMON
```

and discard `.bss` in the linker script. Now every uninitialized `int g;` ends up as 4 explicit zero bytes in `.data`, which gets included in the binary. `filesz == memsz` always. mkfs.py doesn't need section size info.

The cost: a `static char buf[4096];` shows up as 4 KiB of zeros on disk. For demo programs this is fine; for "real" programs you'd revisit. With sub-4 KiB `.data`, the disk overhead is one extra sector.

## libuser internals

Each syscall is a one-line inline asm wrapper:

```c
int sys_getpid(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_GETPID)
                      : "memory");
    return ret;
}
```

`"=a"(ret)` says "EAX is an output, store it in `ret` after the asm". `"a"(SYS_GETPID)` says "load EAX with this value before the asm". `"b"(arg)` for ones that take an argument. `"memory"` clobber tells the compiler the syscall might touch memory the function doesn't statically reference (it does — kputc writes VGA + serial state).

Every syscall handler in the kernel reads from `r->eax`/`r->ebx`/`r->ecx`/`r->edx` and writes the return into `r->eax`. The user-side wrapper produces exactly that. (See [docs/05-userspace-and-syscalls.md](05-userspace-and-syscalls.md) for the kernel side.)

`printf` is straightforward — walk the format string, dispatch on the conversion character, push digits into a stack buffer for `%d`/`%u`/`%x` and write them out. No floating point, no width/precision, no `*` modifiers. About 80 lines.

The arithmetic does need `(uint32_t) % 10` and `>>= 4`. 32-bit `%` and `/` lower to `divl` on i386 — no libgcc helper required. 64-bit divide would pull `__udivdi3` and break the link, so the formatter stays 32-bit.

## _start

```asm
.code32
.section .startup, "ax"
.global _start

_start:
    call    _main
    push    %eax
    call    _sys_exit
1:  hlt
    jmp     1b
```

Three things to notice:

1. **Section is `.startup`, not `.text`.** The linker script picks `*(.startup)` first inside the output `.text`, so the bytes of `_start` are at the very beginning of the linked binary. mkfs.py hardcodes `e_entry = 0x40000000`, which only works if `_start` lives at exactly that address. PE/COFF section name is 8 chars exactly — under the limit.

2. **`_main`, `_sys_exit` (with leading underscore).** mingw32 PE prepends `_` to every C symbol, so `int main(...)` becomes `_main` in the object file. From assembly we have to use the prefixed name.

3. **No prologue/epilogue, no stack alignment dance.** ESP arrives at `USER_STACK_VA + PAGE_SIZE` = 0x40101000, naturally 16-byte aligned. After `call _main` ESP%16 == 12, which is the i386 SysV ABI invariant at function entry. main can have any prologue it wants without alignment surprises.

## The mingw32 `__main` trap

First link attempt:

```
ld: user/_obj/hello.o:hello.c:(.text.startup+0xb): undefined reference to `__main'
```

mingw32 GCC, when it compiles a function literally named `main`, automatically emits a `call ___main` (three underscores in the object file) at the start of the function. `__main` is a libgcc/mingw runtime hook that — on a hosted Windows program — runs C++ static initializers, `atexit` setup, and other things we have no use for.

The bug-class is "host-only-platform leaked into freestanding compile." `-ffreestanding` is supposed to disable this kind of automatic-init injection. On mingw32 it doesn't.

Fix: provide an empty `__main` stub. Two layers of underscore mangling apply:
- **C source:** `void __main(void) {}` (two underscores in the source name)
- **mingw32 PE compile:** prepends one more underscore → object symbol `___main`
- **GCC's emitted call site:** `call ___main`

So defining it in C with the source name `__main` gets the underscores right. (Defining `__main` in assembly would instead require literally `___main:` to match — and remembering that's no fun later.)

```c
/* libuser.c */
void __main(void) {}
```

One four-line workaround for a host-isms-leaking-through-freestanding gotcha.

## Linker script

```
ENTRY(_start)
SECTIONS {
    . = 0x40000000;

    .text : SUBALIGN(4) {
        *(.startup)               /* _start, must be first */
        *(.text*)
        *(.gnu.linkonce.t.*)
    }
    .rdata : SUBALIGN(4) {
        *(.rdata*)
        *(.rodata*)
    }
    .data : SUBALIGN(4) {
        *(.data*)
    }

    /DISCARD/ : {
        *(.bss*) *(COMMON)
        *(.comment) *(.note*) *(.eh_frame*) *(.idata*)
        *(.xdata*) *(.pdata*) *(.CRT*) *(.tls*)
        *(.ctors*) *(.dtors*) *(.reloc*) *(.debug*)
    }
}
```

`SUBALIGN(4)` keeps inputs 4-byte aligned in the output, which is enough for i386 instruction fetch and any non-SSE struct. `*(.bss*)` in `/DISCARD/` is a belt to the suspenders of `-fno-zero-initialized-in-bss` — anything that snuck through ends up nowhere.

`-Ttext` is intentionally **not** set on the ld command line. The linker script's `. = 0x40000000` sets the location counter for the first section; everything else flows from there. Cleaner than fighting linker-flag precedence.

## Building two binaries from one shared object set

```bash
"$CC" "${USER_CFLAGS[@]}" -c -o user/_obj/start.o   user/start.S
"$CC" "${USER_CFLAGS[@]}" -c -o user/_obj/libuser.o user/libuser.c

for name in hello count; do
    "$CC" "${USER_CFLAGS[@]}" -c -o "user/_obj/${name}.o" "user/${name}.c"
    "$LD" -m i386pe -T user/user.ld -o "user/_obj/${name}.elf" \
        user/_obj/start.o "user/_obj/${name}.o" user/_obj/libuser.o
    "$OBJCOPY" -O binary -j .text -j .rdata -j .data \
        "user/_obj/${name}.elf" "user/_obj/${name}.bin"
done
```

`start.o` and `libuser.o` are compiled once. Each user program produces one new `.o`, one final `.elf`, and one flat `.bin`. mkfs.py picks up the `.bin` files, wraps them in ELF, and writes `fs.img`.

## hello.c — the user program looks like a user program

```c
#include "libuser.h"

static const char *greet =
    "Hello from C in ring 3! This program uses libuser's printf.\n";

int main(void) {
    int pid = sys_getpid();
    puts(greet);
    printf("  pid     : %d\n", pid);
    printf("  hello   : %s\n", "yes, I am a real C program");
    printf("  hex pid : 0x%x\n", (uint32_t)pid);
    printf("yielding...\n");
    sys_yield();
    printf("...woke back up. exiting.\n");
    return 0;
}
```

That's the whole program. Compile, link, run. The static string is in `.rdata`, the format strings inline are in `.rdata`, the literal "yes, I am a real C program" is in `.rdata` — all picked up by the linker script's `*(.rodata*)` glob and shipped at runtime.

```
$ exec hello.elf
exec: pid=4  cr3=0x0000b000  entry=0x40000000  esp=0x40101000  (loaded hello.elf)
Hello from C in ring 3! This program uses libuser's printf.
  pid     : 4
  hello   : yes, I am a real C program
  hex pid : 0x4
yielding...
...woke back up. exiting.
[user task pid=4 exited code=0]
[reaper] freed pid=4 (user task), slot 4 now UNUSED
```

Every piece works: `%d` for pid, `%s` for an inline string literal, `%x` for hex, real `\n` newlines, real string concatenation (the `static const char *greet`).

## count.c — uses sleep + time

```c
int main(void) {
    uint32_t start = sys_time();
    printf("Counter (epoch start = %u):\n  ", start);
    for (int i = 0; i < 5; i++) {
        printf("%d ", i);
        sys_sleep_ms(200);
    }
    uint32_t end = sys_time();
    printf("\n  elapsed = %u seconds\n", end - start);
    return 0;
}
```

`sys_time()` returns `uint32_t` (UNIX epoch). `end - start` is normal C arithmetic. The kernel demo tasks `[A]/[B]` keep emitting throughout, proving each `sys_sleep_ms` cooperates with the scheduler:

```
$ exec count.elf
Counter (epoch start = 1778273466):
  0 [A][B][A]1 [B][A]2 [A][B]3 [A][A]4 [B][A]
  elapsed = 1 seconds
```

5 × 200 ms = 1 s. Math checks out.

## What changed kernel-side

Almost nothing. The session-8 loader already handles arbitrary-size PT_LOAD segments correctly — `hello.elf` (1672 bytes of text+rdata+data) loads exactly the same way the 192-byte session-7 program did. The only kernel change was a one-character cleanup: `kernel/elf.c` had its own `#define PAGE_SIZE 4096u` that conflicted with `paging.h`'s; dropped the local one.

The `userprog` and `userprog2` shell commands still exist and still work — they spawn the embedded asm `.up1`/`.up2` programs. Those are the historical record. The C programs run via `exec hello.elf` / `exec count.elf` from the filesystem.

## Files added

| File | Role |
|---|---|
| `user/libuser.h`, `user/libuser.c` | Syscall wrappers, printf, mem*/str*, `__main` stub |
| `user/start.S` | `_start` — calls `_main`, forwards return to `_sys_exit` |
| `user/user.ld` | Linker script, places `.startup` first at 0x40000000 |
| `user/hello.c`, `user/count.c` | Real C user programs |
| `build.sh`, `Makefile` | Step 5 = compile + link user programs |
| `mkfs.py` | Reads `user/_obj/*.bin` instead of extracting `.up1/.up2` |
| `kernel/elf.c` | Removed redundant local `PAGE_SIZE` define |
| `.gitignore` | Added `user/_obj/` |

## Design decisions

**Same toolchain as kernel.** mingw32 GCC produces PE/COFF, `ld -m i386pe` links, `objcopy -O binary` strips. We get PE intermediates that nobody outside this build pipeline ever sees; the kernel loader only ever sees the ELF wrapper that mkfs.py builds.

**`mkfs.py` produces ELF.** Could have invented a custom format and saved 84 bytes per program. Real ELF is the format the kernel loader already speaks; using it consistently means a more sophisticated user program (multiple PT_LOADs, real `.bss`) just plugs in.

**No relocations.** Every program is linked at `0x40000000` and won't run anywhere else. We could add ELF relocation processing in the kernel and let user programs be loaded at varying addresses — but with one process per address space, there's no reason to. Single fixed VA keeps the loader simple.

**Stub `__main` instead of working around the symbol.** Avoids fighting the toolchain for marginal cleanliness gains. Worth noting in the deep dive so the next person sees it once and understands.

**`-fno-zero-initialized-in-bss` instead of teaching the loader about `memsz`.** The loader is already correct for the general case (allocate memsz pages, zero them, read filesz from disk). The simplification is on the *build* side — mkfs.py doesn't have to query section sizes from the linked PE file.

**No `argv` / `envp`.** `_start` calls `main()` with no arguments. Adding `argv` would mean: kernel pushes the args onto the user stack before iret, the calling convention pops them, etc. None of our programs need it.

**Shared `libuser` linked statically into every program.** No dynamic linker, no PLT, no `ld.so`. The bytes of `printf` exist in every program's binary. For two programs of ~1.5 KiB each that's 1.5 KiB of duplication on disk. Not worth a dynamic linker yet.

## What `cat hello.elf` shows

```
hello.elf (1756 bytes):
  0000: 7f 45 4c 46 01 01 01 00 ...                   .ELF.....
  ...
```

The hex dump still starts with the ELF magic `7f 45 4c 46` because mkfs.py's wrapper is identical between the asm and C versions. The 84-byte ELF header is the same; only the payload differs in size and content.

## Deferred

- `.bss` support in user programs (just need to compute memsz vs filesz in mkfs)
- Multiple PT_LOAD segments per program (loader is ready; mkfs.py would need to emit multiple program headers)
- ELF relocations (not needed; we link at a fixed VA)
- Dynamic linking (not needed)
- `argv` / `envp` to user main (would push from kernel before iret)
- A proper user-mode `malloc` (currently no heap; only stack-allocated everything)
- `errno` and a real syscall error convention (currently every syscall returns 0/-1 in EAX)
- Threading (no `pthread_create` equivalent)
- A user-side shell program

## Pitfalls

1. **mingw32 PE prepends `_` to every C symbol.** Assembly references must match: `_main`, `_sys_exit`, etc. The compiler-emitted reference to `__main` is *also* prefixed → `___main`.
2. **Sections in PE/COFF are 8-char-max names.** `.startup` fits exactly; `.start_section` would silently truncate.
3. **`-fno-zero-initialized-in-bss` + `-fno-common` together** are what actually keep `.bss` empty. Either alone leaks.
4. **`_start` must be at the entry-point VA.** Either the linker script puts it first (via a dedicated section that the script picks up before `*(.text*)`), or you need a way to communicate the entry-point address from the build to the ELF wrapper.
5. **`objcopy -j` needs the truncated name** if you ever use a >8-char source section name. Easier to keep names ≤ 8 in the first place.
6. **`-ffreestanding` does not stop mingw32 from emitting `call ___main`.** It would on a Linux gcc; on mingw32 you need either to disable it some other way (no clean flag exists) or provide the stub.
