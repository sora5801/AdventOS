# Session 95 — cc Phase 2 part 5: the preprocessor

**Goal.** Add `#define`, `#include`, and `#ifdef` so cc programs can split across files and define compile-time constants. The classic header-guard idiom should work end-to-end.

Status: **done.** First-try smoke test:

```
$ cc /prep.c -o /prep.elf
cc: wrote /prep.elf
$ prep                                              ; exit 7 (WHITE)
preprocessor lives
RED=1  GREEN=2  BLUE=4  WHITE=7
MAX_NAME=16
hex(255) = 0xff
(no HIDE_FOOTER macro)
DBG was just defined
DBG is gone after #undef
```

`prep.c` includes `colors.h` twice (the second inclusion is a no-op thanks to the `#ifndef COLORS_H` / `#define COLORS_H` / `#endif` guard); it defines and undefines macros; it uses both `#ifdef` and `#ifndef`/`#else`/`#endif`. All as expected.

No regressions to sessions 90–94: `prep`, `printf`, `hello` all verified end-to-end on a fresh boot.

---

## Where the preprocessor sits

The preprocessor is a pass that runs BEFORE the lexer. Pipeline:

```
source.c
   │
   ▼
slurp()  →  raw bytes
   │
   ▼
pp_process_buf()  →  g_pp_buf (preprocessed bytes)
   │
   ▼
lex_all(g_pp_buf, g_pp_len)
   │
   ▼
parse_program()
   │
   ▼
codegen + linking + ELF
```

Macro expansion is single-pass text substitution: when the scanner sees an identifier, it looks up `g_macros[]` and emits the body's bytes inline if it finds a match. The body is NOT re-scanned for further macro expansion — a macro that expands to another macro name only does the inner expansion if the inner name is processed by the parser later (which it isn't, since the parser only sees the post-preprocess token stream).

Real C has rescanning. We don't. Most code doesn't depend on it; for the few cases that do, you can chain the expansion manually.

---

## The six directives

| Directive | What it does |
|---|---|
| `#define NAME body` | Maps NAME to `body` (everything until end-of-line, with trailing whitespace stripped). |
| `#undef NAME` | Removes a macro from the table. |
| `#include "FILE"` | Recursively preprocess `/FILE` (relative paths auto-prefixed with `/`). |
| `#ifdef NAME` | Pushes `1` on the if-stack if NAME is defined, else `0`. |
| `#ifndef NAME` | Inverse — pushes `1` if NAME is NOT defined. |
| `#else` | Flips the top of the if-stack. |
| `#endif` | Pops the if-stack. |

The if-stack is a plain `int` array. Output is suppressed iff any frame on the stack is `0`. That handles arbitrary nesting cleanly:

```c
static int pp_output_active(void) {
    for (int i = 0; i < g_if_depth; i++) if (!g_if_stack[i]) return 0;
    return 1;
}
```

A subtle point: even when output is suppressed (we're inside a skip block), the preprocessor STILL needs to scan for nested `#if`/`#endif` directives to keep the nesting count correct. So directive parsing always runs; only non-directive emission is gated by `pp_output_active()`.

---

## Why `#include` recurses

When the directive parser sees `#include "x.h"`, it slurps `/x.h` from disk and calls `pp_process_buf(buf, len, depth + 1)` on the contents. The recursion shares the same `g_pp_buf` output (appending to wherever `g_pp_len` currently is), the same `g_macros[]` table, and the same `g_if_stack[]`. Depth-limited at 8 to prevent runaway include cycles.

The depth limit is also what makes circular includes safe in practice without per-file include-once tracking. The user-facing safety net is header guards (`#ifndef X / #define X / #endif`), which work because the SECOND include adds nothing to the output: the body is in an `#ifndef X` block where `X` was already defined by the first include's `#define X`.

```c
/* colors.h — protected against double-inclusion. */
#ifndef COLORS_H
#define COLORS_H
#define RED 1
#define GREEN 2
/* ... */
#endif
```

First include: `COLORS_H` is undefined → push `1` on if-stack → emit the body → `#endif` pops. After the include, `COLORS_H` is defined.

Second include (later in the same file or in another included file): `COLORS_H` IS defined → push `0` → suppress emission until `#endif`. The header guard does its job.

---

## What's NOT in this implementation

| Feature | Why not |
|---|---|
| Function-like macros `#define ADD(a,b) ((a)+(b))` | Needs argument substitution; nontrivial. Use inline functions instead — cc doesn't optimize but it's fine. |
| Line-continuation `\` | Macros are single-line. |
| `#if EXPR` with arithmetic | `#ifdef`/`#ifndef` are enough for the headers and guards we'd write. |
| `#pragma`, `#error`, `#warning`, `#line` | Niche; skipped. |
| Macro rescanning | One-pass expansion. A macro body that mentions another macro name won't get re-expanded. |
| Stringification (`#X`), token-pasting (`##`) | Not common in straight C99 source we'd compile; skipped. |
| System includes `<stdio.h>` | We have no stdlib to include — `puts` / `printf` are built-in intrinsics, not declared in a header. |

For the use case ("split a program across header + .c files, define some constants"), the supported set is enough.

---

## The line-and-newline rule

`pp_process_buf` preserves newlines from the input even when the line content is suppressed. That's so the lexer's `g_line` counter and the resulting `die_at(line, ...)` error messages stay aligned with the user's source coordinates:

```c
if (has_newline) pp_emit_byte('\n');
```

Every line emits exactly one `\n` to the output buffer (or zero, for the final line if the source didn't end in newline). Whether the content is included or not, the newline counter still advances. So a syntax error on `prep.c:42` after preprocessing still reports line 42 of `prep.c`, not "line 7 after suppressing some blocks."

(This isn't quite as good as real C, where `#include`'s contents reset the line counter for the included file — we just emit the include body inline and the line numbers conflate. A `#line` directive would fix this; deferred.)

---

## Files touched

- `user/cc.c` — `g_macros[128]`, `g_if_stack[16]`, `g_pp_buf[16384]`, the directive parser, `pp_process_buf`, `pp_include_file`, hookup in `main()` between slurp and lex_all. ~250 lines added.
- `fs/prep.c` — sample exercising all six directives.
- `fs/colors.h` — header with classic `#ifndef`/`#define`/`#endif` guard.
- `fs/man/cc` — preprocessor docs.
- `mkfs.py` — added `prep.c` + `colors.h`.

cc.bin: 205 KiB → 247 KiB (BSS growth from `g_pp_buf[16384]` + `g_macros[128]`).

---

## Phase 2 status

Five of six sub-sessions shipped — one left to call Phase 2 done:

- ✅ 91 — string literals + puts
- ✅ 92 — char/pointers/arrays
- ✅ 93 — globals
- ✅ 94 — printf
- ✅ 95 — preprocessor
- ⏳ 96 — compound operators: `+=`, `-=`, `++`, `--`, ternary `?:`, comma

After 96 the surface-level ergonomics match real C. cc compiles things that look like real programs.
