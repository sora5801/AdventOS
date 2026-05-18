# Session 125 — cc optimization passes (register allocator, constant folding, peephole, DCE)

**Goal.** Path B Phase 4 (session 121) declared the C-subset language
surface complete, leaving optimization as the next-most-impactful
follow-up. This session ships four passes that together shrink the
output of `cc /capstone.c -o /cap.elf` from **2114 bytes → 1955 bytes
(-7.5%)** with zero correctness regressions.

The passes, in commit order:

1. **Register allocator** (smart codegen with EBX + collapsed addr chains): -103 bytes
2. **Constant folding** (AST-level): no change on capstone.c, foundational for DCE
3. **Peephole** (rolling `mov eax, imm; push eax` → `push imm`): -56 bytes
4. **Dead code elimination** (unreachable-after-return, `if (const)`, `while (const)`): no change on capstone.c

Each is a separate commit so the bisect surface stays clean.

---

## 1. Register allocator — skip push/pop when one operand is simple

The default N_BIN codegen evaluates a binop with a full EAX
push/pop round-trip:

```
gen_expr(lhs)   ; eax = lhs
push eax        ; save lhs
gen_expr(rhs)   ; eax = rhs (clobbers eax)
mov  ebx, eax   ; ebx = rhs
pop  eax        ; eax = lhs (restored)
<op>            ; eax = lhs op rhs
```

For the *very* common case where one operand is "simple" — `N_NUM`,
`N_STR`, `N_NAME`, or `N_ADDR_OF` — we can load the simple side
directly into EBX without going through EAX. The push and pop
disappear:

```
gen_expr(lhs)              ; eax = lhs
gen_simple_into_ebx(rhs)   ; ebx = rhs (mov / lea — no eax touch)
<op>                       ; eax = lhs op rhs
```

`is_simple_load` is the gate; `gen_simple_into_ebx` mirrors the
existing EAX codegen but writes to EBX via:

- `e_load_local_ebx(off)` → `mov ebx, [ebp + disp]`
- `e_lea_ebx_ebp(off)` → `lea ebx, [ebp + disp]` (for array decays)
- `e_mov_ebx_at_abs()` / `e_mov_ebx_imm_for_fixup()` for globals + function VAs

Same trick for the LHS-simple, RHS-complex case (evaluate RHS into
EAX, `mov ebx, eax`, then load LHS into EAX — gen_expr on a simple
N_NUM/N_NAME/etc. is statically known not to touch EBX, so it's
safe to load over the just-saved RHS).

### Collapsed address chains: N_MEMBER, N_ARROW, N_MEMBER_ASSIGN

The other big single change in pass 1: collapse the
`base-address + field-offset + dereference` chain into a single
addressed load/store.

Before:

```
N_MEMBER local:   lea  eax, [ebp + off]      ; 3 bytes
                  add  eax, field_off        ; 5 bytes (skipped if 0)
                  mov  eax, [eax]            ; 2 bytes
                                              total: 10 bytes
```

After:

```
N_MEMBER local:   mov  eax, [ebp + off + field_off]
                                              total: 3 or 6 bytes
```

Same trick for `N_MEMBER_ASSIGN`:

```
LOCAL.field = expr   →   mov [ebp + off + field_off], eax
GLOBAL.field = expr  →   mov [GLOBAL_VA + field_off], eax
```

The global case requires the fixup loop to support **addends**: the
codegen emits `emit_d(field_off)` as the imm32 placeholder, and the
patch loop reads that value as an addend before patching in
`GLOBAL_VA`. Existing call sites that emit `emit_d(0)` continue to
work unchanged (addend = 0 ⇒ patch is plain VA).

For N_ARROW (struct pointer): load pointer first, then dereference
with displacement (`e_load_eax_at_eax_disp(field_off)` emits
`mov eax, [eax + disp]` in the shortest form).

`p.x` and `p.y` are absolutely everywhere in struct-heavy code; this
pass alone is responsible for 103 bytes of the 159-byte total drop.

---

## 2. Constant folding — pre-codegen AST tree walk

Post-order recursion over each function body. When a `N_BIN` or
`N_UN` has all-literal operands, replace the node in place with a
single `N_NUM` holding the folded result. `N_TERNARY` with a literal
condition gets shallow-spliced with whichever arm is chosen.

```
   N_BIN(+, 7, N_BIN(*, 3, 4))
        ↓ fold inner
   N_BIN(+, 7, 12)
        ↓ fold outer
   N_NUM(19)
```

Operators supported: `+ - * / % & | ^ << >> == != < > <= >= && || ! - ~`.
Division/modulo by zero are left as-is — codegen still emits the
divide and the program traps at runtime, matching the existing
behavior.

No size change on the capstone smoke test because capstone.c has
no constant-only sub-expressions. The pass is foundational for
pass #4 (DCE): `if (CONST)` becomes `if (literal)`, and DCE strips
the dead branch.

---

## 3. Peephole — rolling rewrite of `mov+push` into `push imm`

Implemented inside `e_push_eax` as a rolling rewrite: when about
to emit `push eax`, look at the previous 5 bytes. If they're
`b8 imm32` (`mov eax, imm32`) AND there's no fixup attached to the
imm32, rewrite to a single `push imm`:

```
b8 imm32 50   (6 bytes)   →   6a imm8        (2 bytes, if imm fits sbyte)
                              or 68 imm32    (5 bytes, otherwise)
```

The fixup guard (`has_imm_fixup_at`) scans the three imm-style fixup
tables (`g_str_fixups`, `g_glob_fixups`, `g_addr_fixups`) to detect
the case where the imm32 isn't a literal — it's a placeholder for
a string VA, global VA, or function VA waiting to be patched. Those
shouldn't be folded into a `push imm`.

### Why rolling, not a post-pass

A post-codegen peephole that shrinks bytes is conceptually clean
but painful in practice: every captured branch offset (`int jz =
e_jz_rel32()`), every fixup `code_off`, and every patch
displacement would need to be updated when bytes disappear. By
shrinking only at the *end* of `g_code` (immediately before the
new emit), no previously-captured offset can refer to a byte that
gets removed. Forward branches haven't been patched yet — they
read `g_code_len` at patch time which already reflects the shrink.
Backward branches were patched using `g_code_len` at the time of
*their* emission, also post-shrink. Everything just works without
extra bookkeeping.

This is the highest-impact single pattern in cc-emitted code: 56
bytes from capstone.c alone. Every cdecl-style call arg that's
a small literal benefits — and there are many.

---

## 4. Dead code elimination

Three patterns at the `gen_stmt` level:

- **`N_BLOCK` after `return`** — stop emitting at the first `return`
  in a block. Anything after is unreachable.

- **`N_IF` with constant condition** — emit only the taken branch.
  Drops the conditional test + `jz` and the dead-branch body
  entirely.

- **`N_WHILE` with constant condition** — `while (0)` emits nothing
  at all (zero bytes). `while (NONZERO_CONST)` emits the body in
  a tight infinite loop without the conditional test (only the
  unconditional backward jmp at the bottom).

These depend on pass #2 having normalized arithmetic conditions
into `N_NUM` literals first. Without const-fold, `if (DEBUG && 0)`
would still look like a binop to DCE.

No size change on capstone.c because it has no dead code. DCE
shines in programs with debug flags, feature toggles, and macros
like `#define LOG(...)` that compile to no-ops when disabled —
extremely common in real C.

---

## Cumulative result on `/capstone.c`

| Pass | cap.elf bytes | Δ from baseline |
|------|--------------:|----------------:|
| baseline (session 121)           | 2114 |          |
| 1: register allocator            | 2011 | −103 (−4.9%) |
| 2: constant folding              | 2011 | (foundational) |
| 3: peephole                      | 1955 | −159 (−7.5%) |
| 4: DCE                           | 1955 | (foundational) |

The capstone sample is small (a few struct ops, one extern, one fp
typedef, four printf calls), so the percentage understates the
real-world impact. Programs with hot loops, heavy struct-field
access, debug macros, or constant-sized buffers will see
significantly more.

---

## Files touched

- `user/cc.c` — ~370 lines net additions across four commits:
  - EBX-targeted emission primitives + `is_simple_load` +
    `gen_simple_into_ebx`
  - N_BIN, N_MEMBER, N_ARROW, N_MEMBER_ASSIGN fast paths
  - `e_load_eax_at_eax_disp` helper
  - Additive-addend handling in the global fixup loop
  - `fold_node` and its invocation post-parse
  - Rolling peephole inside `e_push_eax` + `has_imm_fixup_at`
  - DCE branches in `gen_stmt`'s N_BLOCK, N_IF, N_WHILE

- `smoke_pathB.py` — runs `wc -c /cap.elf` after the capstone test
  and prints the size so size regressions surface in CI-style runs.

- `docs/112-pathB-optimizations.md` — this file.
- `README.md` — pointer to session 125.
