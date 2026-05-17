# Session 100 — cc Phase 3 part 4: multi-file compilation

**Goal.** Let cc accept multiple `.c` source files and produce a single linked ELF. Shared headers with `#ifndef` guards now work the way real C programmers expect. After this, the only "compile-time" infrastructure cc is still missing is intermediate object files — single-step multi-file is enough for the small projects cc targets.

Status: **done.** Smoke test:

```
$ cc /mmain.c /mlib.c -o /multi.elf
cc: wrote /multi.elf
$ multi
add(7, 35) = 42
MULTIPLIER = 100
dot_self((3,4)) = 25
banner = AdventOS
```

`mmain.c` calls `add`, `dot_self`, and `banner` — all defined in `mlib.c`. The shared `struct point` definition lives in `mlib.h`, which both files include; the `#ifndef MLIB_H` guard makes the second inclusion a no-op.

---

## Two approaches; cc takes the simpler one

**Real-C path:** compile each `.c` to an intermediate `.o` (with a symbol table + relocations + section data), then run a linker that resolves cross-file symbols and produces the final ELF. Each `.o` is its own translation unit.

**cc path:** preprocess each file in turn, with shared preprocessor state, concatenating the output into a single token stream. One lex + parse pass. One codegen pass. The existing call-fixup machinery (sessions 90 and 98) handles cross-file references automatically because all functions live in the same `g_funcs[]` table.

cc takes the second path. No object files, no separate linker tool, no symbol-table format. The downside: you can't pre-compile a `.c` once and reuse the result across builds. The upside: a few dozen lines of changes in cc's `main()` and you're done.

---

## What changed

### `main()` now accepts multiple inputs

```
cc FILE.c [FILE.c ...] [-o OUT.elf]
```

The argument parser collects up to 16 input paths. The output path defaults from the FIRST input (so `cc a.c b.c` writes `a.elf`).

### Preprocessor persists across files

This is the key behavior. Each input file's bytes are slurped, then `pp_process_buf` is called with the shared `g_macros[]` table and `g_if_stack[]` intact:

```c
g_pp_len = 0;
g_n_macros = 0;
g_if_depth = 0;
for (int i = 0; i < n_inputs; i++) {
    char *src = slurp(in_paths[i], &sz);
    pp_process_buf(src, sz, 0);
}
```

Because the preprocessor state survives across files, a header guard in a shared `.h` file does what you expect: the first `#include "x.h"` defines a guard macro; the second time any file includes `"x.h"`, the macro is already defined and the header body is skipped.

### Pointer return types

The parser previously didn't accept `char *foo()` — only `int foo()` or `char foo()`. The multi-file demo had `char *banner()` returning a string literal, which forced the fix. `parse_func` now accepts an optional `*` between the return type and the function name. The `*` is consumed but otherwise ignored (cc doesn't typecheck return values).

### Duplicate-function check

If two files define the same function, the parser would happily codegen both — the second would overwrite the first's `entry_off`. Now `gen_func` errors with `duplicate function definition`. Single-file builds never trigger this; multi-file demos that copy-paste a function across files do.

---

## What multi-file unlocks

```
// mlib.h — shared types and constants
#ifndef MLIB_H
#define MLIB_H
struct point { int x; int y; };
#define MULTIPLIER 100
#endif

// mmain.c — entry point
#include "mlib.h"
int main() {
    struct point p; p.x = 3; p.y = 4;
    printf("dot_self((%d,%d)) = %d\n", p.x, p.y, dot_self(&p));
    return 0;
}

// mlib.c — implementation
#include "mlib.h"
int dot_self(struct point *p) {
    return p->x * p->x + p->y * p->y;
}
```

Build with `cc /mmain.c /mlib.c -o /prog.elf`. The struct definition is shared; the function `dot_self` is defined in mlib.c and called from mmain.c.

A real-world demo would add more files and more functions. Each gets its own `cc /a.c /b.c /c.c ...` compile line.

---

## What's still missing

| Feature | Why not |
|---|---|
| Intermediate `.o` files | Would need an object-file format, relocation table emission, and a separate linker. Multi-step compilation is useful when you want to recompile only the changed files, but for the size of projects cc compiles, single-step is fine. |
| Function prototypes (`int add(int, int);`) | cc resolves cross-file calls via the existing fixup pass — no prototype needed. Adding prototypes would require parsing a function declaration without a body, which the grammar doesn't currently distinguish from `int add` (the start of a global declaration). Implementable but not urgent. |
| `extern int x;` declarations for globals | Currently a global must be DEFINED (with allocation) in exactly one file. Adding `extern` would let one file declare-only and another define. |
| `static` storage class | Every function and global has external linkage in cc. No way to mark something file-private. |

Each is a small parser/semantic change; collectively they'd take a session each.

---

## FS budget note

Adding `mlib.h` + `mlib.c` + `mmain.c` to the boot image pushed the FS file count past `FS_MAX_FILES = 128`. Bumping the cap to 160 was attempted and rejected — the extra 2 KiB of super-block buffer pushed kernel .bss past the `0xA0000` VGA-RAM boundary that build.sh enforces. Workaround: removed the older `hello.c` / `strs.c` / `chars.c` sample sources from `mkfs.py`. Their content remains documented in `docs/77-79`; the sessions' deep-dive `.md` files are still committed.

The pattern is the same as in previous sessions: cc compiles produce new files which consume `FS_MAX_FILES` slots; after several compiles in one boot, future writes fail. Reboot between batches. A future session could implement non-contiguous block allocation or compress the mkfs payload to free more slots.

---

## Files touched

- `user/cc.c` — `main()` accepts up to 16 input files; preprocessor persists across files; `parse_func` accepts pointer return types (`char *foo()`); `gen_func` rejects duplicate function definitions. ~30 lines added.
- `fs/mmain.c`, `fs/mlib.c`, `fs/mlib.h` — multi-file demo.
- `fs/man/cc` — updated synopsis + restrictions.
- `mkfs.py` — added the demo files; removed hello.c/strs.c/chars.c to keep file_count under cap.
- README.md — pointer bump.

cc.bin: 279 KiB → 279 KiB (the changes are all in main(); the codegen path didn't grow).

---

## Phase 3 status after session 100

Four of N+ shipped:

- ✅ 97 — structs (member access, struct pointers, linked lists)
- ✅ 98 — function pointers
- ✅ 99 — sizeof + scaled pointer arithmetic
- ✅ 100 — multi-file compilation
- ⏳ 101 — struct value assignment (memcpy emit)
- ⏳ 102 — array-of-struct + member access through index expressions
- ⏳ 103+ — enum, typedef, real variadics, optimization, …

After this session, cc accepts the natural shape of a small C project: header + several .c files, shared macros and structs, no function prototypes needed. The next visible quality-of-life gap is struct-value assignment.
