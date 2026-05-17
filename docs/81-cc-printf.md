# Session 94 — cc Phase 2 part 4: `printf`

**Goal.** Add `printf` so cc programs can produce formatted output. This is the feature most "is the compiler real" comparisons check.

Status: **done.** First-try smoke test:

```
$ cc /printf.c -o /printf.elf
cc: wrote /printf.elf
$ printf                                            ; exit code 17
hello, world
n = 42
ten percent of 100 is 10
first letter: H
count: 1 2 3
decimal 255 in hex is ff
VA 0x40000000 maps the user code
-1 as hex = ffffffff
100% done!
(1, 2, 3) — done.
```

No regressions to sessions 90–93. All four prior sample programs still produce identical output.

---

## Why this isn't a real variadic function

A real-C printf is a variadic function: `int printf(const char *fmt, ...)`. The callee walks the format string at runtime, and each `%X` consumes the next arg via `va_arg(ap, T)`. The machinery for `va_arg` is small in cdecl (just bump a pointer by `sizeof(T)`), but plumbing it into cc's parser, type system, and codegen would be a meaningful refactor:

- New `T_ELLIPSIS` token for `...` in param lists
- New "varargs flag" on the function descriptor
- New AST node for `va_start` / `va_arg`
- Type-aware arg promotion (chars promote to int when passed to varargs)
- Runtime format-string parser inside the printf function body

That's all fine, but it's a session by itself and the payoff is the same: programs can call `printf`.

**Shortcut.** Make `printf` a compiler intrinsic. The format string must be a literal (`N_STR`), so the compiler can:

1. Parse the format string at compile time.
2. Split it into literal-text segments and `%X` directives.
3. For each segment, intern it into the string pool as its own entry, then emit `print_str(segment)`.
4. For each `%X`, emit `gen_expr(next_arg); push eax; call __helper_for_X; add esp, 4`.

The result is a sequence of "plain" function calls — no variadics anywhere. A real `printf(fmt, ...)` with a dynamic format string can come later with the proper machinery; the literal-only form covers ≥95% of uses.

---

## The four helper functions

`printf` decomposes into calls to four runtime helpers, all emitted once near the top of the binary alongside `__print_int_helper` and friends:

```
__print_int_nonl_helper(int n)      ; like print_int but no trailing '\n'
__print_str_helper(char *s)         ; (already existed for puts)
__print_char_helper(int c)          ; writes the low byte of c
__print_hex_helper(unsigned n)      ; lowercase hex, no padding
```

`__print_int_nonl_helper` is a copy of `__print_int_helper` minus the initial `'\n'` setup; the loop reverses its `dec esi` / `mov [esi]` ordering so the cursor pre-decrements instead of post-decrementing. (The original keeps post-decrementing because it had to leave room for the trailing `'\n'`.)

`__print_char_helper` is the smallest one — `push [ebp+8]; mov ecx, esp; sys_write(fd=1, ecx, len=1); add esp, 4`. Five instructions plus the prologue/epilogue.

`__print_hex_helper` is the most interesting. It treats the arg as unsigned (`div ebx` instead of `idiv ebx`) so that `-1` prints as `ffffffff` instead of `-1`. The digit-to-char step has two branches: 0–9 add `'0'`, 10–15 add `'a' - 10 = 0x57`:

```
xor edx, edx       ; clear high half for unsigned div
mov ebx, 16
div ebx            ; eax /= 16, edx = remainder
cmp dl, 10
jl  numeric
add dl, 0x57       ; 'a' - 10 = 87
jmp +3             ; skip the numeric add
numeric:
add dl, '0'        ; '0' = 0x30
dec esi
mov [esi], dl
test eax, eax
jnz loop_top
```

Loop body is exactly 29 bytes (counted carefully — relative `jnz -29` is on the line right after the body). One of the easy bugs in this pattern is that the alpha-branch `jmp +3` distance must skip the 3-byte `add dl, '0'`, not `jmp +0` (which would no-op and produce wrong characters).

---

## The format-string dispatcher

The interesting code in `emit_syscall_intrinsic`:

```c
const char *fmt = &g_str_pool[g_str_offs[fmt_node->num]];
int arg_idx = 1;
char chunk[256];
int  chunk_n = 0;

for (int i = 0; fmt[i]; i++) {
    if (fmt[i] != '%') {
        chunk[chunk_n++] = fmt[i];
        continue;
    }
    flush_chunk();                              // emit print_str of chunk
    char spec = fmt[++i];
    if (spec == '%') { chunk[chunk_n++] = '%'; continue; }
    int helper = pick_helper(spec);            // 'd' → int, 's' → str, etc.
    gen_expr(call->list[arg_idx++]);
    e_push_eax();
    emit_call_with_fixup(helper);
    e_add_esp_imm32(4);
}
flush_chunk();                                  // tail
```

`flush_chunk` interns the buffered text via `str_intern`, emits `mov eax, imm32` with a string fixup, then a call to `__print_str_helper`. The same string-fixup table that handles plain `N_STR` nodes handles these mid-codegen interns. No new table needed.

The fact that `str_intern` is called *during* codegen — long after the original lexer pass — is fine because the pool's address is still unresolved. The pool gets its base VA at the very end (after all codegen), and every fixup (lex-time or codegen-time) gets patched in the same pass.

---

## What's deferred

- **Dynamic format strings.** `printf(fmt, arg)` where `fmt` is a variable. Requires actual variadic-function support and a runtime format parser. Probably worth doing eventually so `vprintf` can be implemented; not today.

- **Width / precision** (`%5d`, `%.3s`, `%08x`). Each is straightforward modifications to the helpers; not in scope for "the minimum to call this printf done."

- **`fprintf`/`sprintf`.** These need an fd argument or output buffer. Easy to add later — same dispatcher, different helper signatures.

- **`%f` (floats).** AdventOS user code builds with `-mgeneral-regs-only` and no FPU support, so floats just don't exist. There's no path to add `%f` without first adding software-float helpers.

---

## Files touched

- `user/cc.c` — three new helper-emit functions (`emit_print_int_nonl_helper`, `emit_print_char_helper`, `emit_print_hex_helper`), the `printf` intrinsic dispatcher, helper registration in `main()`. ~250 lines added.
- `fs/printf.c` — new sample.
- `fs/man/cc` — printf docs.
- `mkfs.py` — added `printf.c`.

cc.bin: 202 KiB → 205 KiB.

---

## Phase 2 status after session 94

Four of five sub-sessions shipped:

- ✅ 91 — string literals + puts
- ✅ 92 — char/pointers/arrays
- ✅ 93 — globals
- ✅ 94 — printf
- ⏳ 95 — preprocessor (`#define`, `#include`, `#ifdef`)
- ⏳ 96 — compound operators (`+=`, `++`, ternary)

After 95 the compiler can handle multi-file projects; after 96 the source-level ergonomics match real C. At that point we have a real comparison point against `tcc`, and the next decision is "keep extending cc" vs. "port tcc."
