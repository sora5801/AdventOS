# Session 91 — cc Phase 2 part 1: string literals + `puts`

**Goal.** Add the missing piece every "is the compiler real" check looks for: `puts("hello, world")`. That requires three things — a lexer that handles `"..."`, a way to embed string bytes in the output binary, and a `puts` intrinsic that ties them together.

Status: **done.** First-try smoke test on the booted guest:

```
$ cc /strs.c -o /strs.elf
cc: wrote /strs.elf
$ strs                                 ; exit code 7
hello, world
two words, one line
tab	here	and	here
quote: "yes" backslash: \
hi
hi
hello, world
```

No regressions to session 90's `hello.c` (still prints `3628800 / 5050 / 21`, exits 42).

---

## The four-piece shape

Adding string literals to a compiler that already has `int` is mostly plumbing — the codegen barely changes. The pieces:

1. **Lexer.** New `T_STR` token. Body of the literal is decoded (with `\n` / `\t` / `\\` / `\"` / `\'` / `\r` / `\0` escapes), then interned into a global pool. The token's `num` field stores the pool index, not a raw offset, because the pool's final base VA isn't known yet.

2. **AST.** New `N_STR` node carrying the same pool index. Parser slot is in `parse_primary` right next to `N_NUM`.

3. **Codegen.** `N_STR` emits a 5-byte placeholder `mov eax, imm32` and records a `g_str_fixup{ code_off, str_idx }`. Once all code is emitted, we know how big `g_code` is, so we know the VA where the pool will start (`ENTRY_VA + g_code_len`), and we walk the fixup table patching each imm32 with the per-string VA.

4. **Helpers.** Two new emitted-once helper functions in the binary:
   - `__print_str_helper(char *s)` — inline-strlen loop + `sys_write(1, s, len)`.
   - `__puts_helper(char *s)` — calls `__print_str_helper`, then `sys_write(1, "\n", 1)` via a one-byte stack scratch.

The compiler exposes `puts` and `print_str` as intrinsics; each emits a `call rel32` to the corresponding helper with a fixup.

---

## How strings live in the binary

Before session 91, the ELF was:

```
[0..83]    header + phdr   ; offset=84, vaddr=0x40000000, filesz=memsz=code_size
[84..]     g_code          ; machine code
```

After session 91, the pool is appended to `g_code` and the same PT_LOAD covers both:

```
[0..83]    header + phdr   ; filesz=memsz=(code_size + pool_size)
[84..C-1]  g_code          ; machine code
[C..C+P-1] g_str_pool      ; NUL-terminated strings, deduped
```

`C` = `code_size` at the moment strings start. `g_str_pool_base_va = ENTRY_VA + C`. A string at pool offset `o` lives at VA `g_str_pool_base_va + o`.

The user.ld trick about RWX (and the kernel mapping PT_LOAD as RWX) means the pool is readable from user code without any extra protection setup. Real systems would put strings in a `PT_LOAD` with `PF_R` only; we don't care about the segregation yet.

---

## Dedup as a freebie

`str_intern(src, len)` scans the existing pool entries before adding. Two identical literals share one pool entry — `puts("hi"); puts("hi");` patches both `mov eax, imm32` placeholders with the same VA, costing 3 bytes of pool ("hi\0") for any number of uses.

That falls out of having a single intern function called from the lexer. For a written-by-hand compiler with no constant folding it's the easiest optimization to leave in.

---

## The `__print_str_helper` body

This is one of those functions that's longer in english than in machine code:

```
push ebp                  ; 55
mov  ebp, esp             ; 89 e5
push ebx                  ; 53

mov  ecx, [ebp+8]         ; 8b 4d 08    — ecx = ptr (also sys_write addr)
xor  edx, edx             ; 31 d2       — len = 0
.loop:
  cmp byte [ecx+edx], 0   ; 80 3c 11 00 — note SIB: base=ecx, idx=edx
  je  .done               ; 74 ??       — disp8 patched after .done known
  inc edx                 ; 42
  jmp .loop               ; eb ??       — backward rel8
.done:
mov  ebx, 1               ; bb 01 00 00 00 — stdout
mov  eax, 12              ; b8 0c 00 00 00 — SYS_WRITE_FD
int  0x80                 ; cd 80

pop  ebx                  ; 5b
mov  esp, ebp             ; 89 ec
pop  ebp                  ; 5d
ret                       ; c3
```

Two jump distances are patched at emission time — both `loop_top` (we have it before emitting the `jmp`) and `.done` (filled in after the loop body is closed, via `g_code[je_off] = ...`). After session 90's painful lesson with hand-counted displacements, both of these are computed from `g_code_len` snapshots rather than counted bytes; the only number that gets to be literal is the `jmp .loop` backward rel8, and even that is `loop_top - (g_code_len + 1)` at emit time.

---

## The `__puts_helper` body

Layered on top of `__print_str_helper`. Forward the arg, then write `'\n'` from a fresh 1-byte stack slot:

```
push ebp; mov ebp, esp
push [ebp+8]              ; forward the arg
call __print_str_helper   ; rel32 with fixup
add  esp, 4               ; cdecl cleanup

push 10                   ; 6a 0a  — '\n' on the stack (4 bytes pushed, 1 used)
mov  ecx, esp             ; 89 e1
mov  edx, 1               ; ba 01 00 00 00
mov  ebx, 1               ; stdout
mov  eax, 12              ; SYS_WRITE_FD
int  0x80
add  esp, 4

mov  esp, ebp; pop ebp; ret
```

The `push 10` is cleaner than reserving a buffer slot at function entry — three bytes of code beats fiddling with frame offsets.

---

## Why `puts` of a string-literal *and* `puts` of any char* both work

The intrinsic is symmetric: it generates `gen_expr(arg); push eax; call __puts_helper; add esp, 4`. The expression in argument position can be anything that produces a `char*`-shaped value:

- `puts("literal")` — `gen_expr` of `N_STR` emits `mov eax, imm32` (pool VA).
- `puts(s)` where `s` is a local — `gen_expr` of `N_NAME` does `mov eax, [ebp + off]`.
- `puts(maybe_get_msg())` — `gen_expr` of `N_CALL` puts the return value in eax.

Right now nothing in the parser actually *produces* a `char*` other than `N_STR`, because we haven't added `char` / pointers / arrays yet. So the symmetry is theoretical. But the codegen is general — when session 92 adds char/char*/&/* etc., `puts` will Just Work without re-touching the helper.

---

## Sizing and budget

`cc.bin` grew from 170 KiB (session 90) to 179 KiB. The growth is the BSS for `g_str_pool[4096]`, `g_str_offs[256]`, `g_str_fixups[256]`. user.ld folds .bss into .data — that's still the dominant cost, same as agentd.

`STR_POOL_MAX = 4 KiB` is the per-program string budget. For comparison, `hello, world\n` is 13 bytes — 4 KiB fits ~300 average-sized error messages or a few dozen long ones. Bumpable if a program ever needs more, but the cap exists to keep the BSS tight.

`MAX_STRS = 256` distinct literals. Same reasoning.

---

## What didn't make it

`printf` would have been the headliner. It needs:
- Variadic functions (`int printf(const char *fmt, ...)`).
- `va_arg` machinery on the callee side.
- Format-string parsing (`%d`, `%s`, `%x`, …).
- A flexible output buffer.

Each of those is small individually but they don't compose neatly without the char/pointer type system. Saving `printf` for after session 92 (char + pointers + &/* + arrays). For now, `puts` + `print_int` covers the actual demonstration case: producing decimal + text output to stdout.

`gets`/stdin is also not in. Reading would need char-array buffers and the `sys_read` syscall plumbed through.

---

## What's next for Path B

Phase 2 remaining sub-sessions, in the order I'd take them:

1. **Session 92** — char, char\*, & (address-of), \* (deref), array indexing. Unlocks general string handling: `int strlen(char *s)`, `int strcmp(char *a, char *b)`, etc.
2. **Session 93** — global variables. One new symbol-table path; .data section in the binary.
3. **Session 94** — `printf` itself (variadic, format strings).
4. **Session 95** — preprocessor: `#define` (object-like and function-like), `#include`, conditional `#ifdef`.
5. **Session 96** — extra operators: `+=`, `++`, ternary, comma.

After all that, the compiler can compile small real programs that read stdin, manipulate strings, and print to stdout. That's also the point where porting `tcc` for real starts to make sense — we'd already have a base to compare against.

---

## Files touched

- `user/cc.c` — string pool (`g_str_pool`/`g_str_offs`), `T_STR` lexer, `N_STR` AST + codegen, `emit_print_str_helper`, `emit_puts_helper`, `puts`/`print_str` intrinsics. ~150 lines added.
- `fs/strs.c` — new sample.
- `fs/man/cc` — added puts/print_str docs.
- `mkfs.py` — added `strs.c`.

cc.bin: 170 KiB → 179 KiB.
