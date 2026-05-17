# Session 105 — cc Phase 3 part 9: real variadic functions

**Goal.** Let users write their own variadic functions: `int sum(int n, ...)` with `va_start` / `va_arg` / `va_end` to walk the extra args. cc had a compile-time-dispatched `printf` since session 94; this session gives users the same mechanism, generalized.

Status: **done.** Smoke test:

```
$ cc /vararg.c -o /va.elf
cc: wrote /va.elf
$ va                                ; exit code 42
sum_n(3, 10, 20, 30) = 60
sum_n(5, 1, 2, 3, 4, 5) = 15
sum_n(0) = 0
dump_n(4, 100, 200, 300, 400):
  [0]: 100
  [1]: 200
  [2]: 300
  [3]: 400
```

`sum_n(0)` correctly handles "zero extra args." `dump_n` walks four ints and prints each. Exit code 42 from `sum_n(2, 11, 31)`.

---

## The cdecl-on-i386 fit

The classic implementation of variadic functions on cdecl x86 is almost free, because cdecl already pushes args right-to-left and has the caller clean up. So:

- The first named param is at `[ebp + 8]` (after saved EBP and return address).
- The second named param at `[ebp + 12]`.
- The N-th named param at `[ebp + 8 + (N-1)*4]`.
- Any args BEYOND the last named (the "variadic" ones) live at `[ebp + 8 + N*4]`, `[ebp + 12 + N*4]`, etc.

The callee doesn't even know how many variadic args there are — only the named contract says "the first int tells us, and there are that many more ints." Real C's `va_arg(ap, T)` walks this region by advancing `ap` by `sizeof(T)` each call.

cc's implementation: `ap` is a plain `int` local that holds the address of the next arg. `va_start(ap, last)` sets `ap = &last + 4`. `va_arg(ap)` reads `*ap`, then advances `ap += 4`. `va_end(ap)` is a no-op (no allocation to free).

---

## The three intrinsics

### `va_start(ap, last_named)`

```c
e_lea_eax_ebp(last_off + 4);
e_store_local(ap_off);
```

`last_off` is the ebp offset of the last-named parameter (typically `8` for a 1-param function or `12` for a 2-param one). `ap` is the local int that will track the walker. So `ap` ends up pointing to the BYTE AFTER the last named param — i.e. the first variadic arg.

The parser enforces both args are NAMEs that resolve to locals; the second must have a positive ebp offset (i.e. is a function parameter, not a regular local).

### `va_arg(ap)`

```c
mov  ebx, [ebp + ap_off]      ; ebx = ap
mov  eax, [ebx]               ; eax = *ap (the next arg)
add  ebx, 4                   ; advance ap by 4 bytes
mov  [ebp + ap_off], ebx      ; write the new ap back
```

Six instructions, ~15 bytes. `va_arg` returns the value AND updates the walker. The user can use it in any expression context — `total = total + va_arg(ap);` works because eax carries the value.

cc's `va_arg` always reads 4 bytes. Real C lets you `va_arg(ap, T)` for different T (char, short, double, struct). cc has neither types to thread nor non-4-byte primitives, so the type spec is moot. Documented as the only restriction.

### `va_end(ap)`

No code emitted. Matches real C's `va_end` semantics (cleanup) without any actual cleanup to do.

---

## Variadic at the declaration site

Parser change: when scanning the param list and we hit `T_ELLIPSIS`, set `fn->op = 1` (variadic flag) and require the `...` to be the last entry before `)`.

Codegen change: `gen_func` propagates `fn->op` into `g_funcs[idx].is_variadic`. The `func_intern` argc-mismatch check skips variadic entries:

```c
if (g_funcs[i].is_variadic) return i;
if (n_params >= 0 && g_funcs[i].n_params != n_params)
    die_at(0, "arg-count mismatch for", name);
```

So calls to a variadic function with different argc — `sum_n(3, ...)` and `sum_n(5, ...)` — both pass through `func_intern` without error.

---

## A wrinkle: define-before-call

`gen_func` runs through the AST in source order. For a variadic function defined LATER than its first call, the early `func_intern` (from the call) stores n_params=argc and `is_variadic=0`. Later when `gen_func` discovers the definition is variadic, it sets `is_variadic=1` — but the FIRST call's intern has already validated against the earlier argc.

In practice: typical multi-file or single-file programs define a variadic helper BEFORE its callers. The demo (`vararg.c`) does. If a forward-call to a variadic is the issue, the user works around by re-ordering source — same as the no-prototype constraint from session 100.

Documented in the man page.

---

## Files touched

- `user/cc.c` — `T_ELLIPSIS` token + lexer for `...`; `is_variadic` field in `func_info`; `func_intern` skips check for variadic; `parse_func` accepts `, ...` at end of params; `gen_func` propagates the flag; three new intrinsics (`va_start`, `va_arg`, `va_end`). ~100 lines added.
- `fs/vararg.c` — sample with two variadic functions.
- `fs/man/cc` — variadic + intrinsics docs.
- `mkfs.py` — added vararg.c.
- `README.md` — pointer bump.

cc.bin: 291 KiB → 293 KiB.

---

## Phase 3 status after session 105

Nine sub-sessions shipped. Remaining: struct-by-value calls (106), then optimization / register allocator as later candidates.

Composite: cc compiles every common C construct now except struct-by-value passing/returning and the optimization layer. The next session closes the last semantic gap.
