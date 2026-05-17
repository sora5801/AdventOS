# Session 96 — cc Phase 2 part 6: compound operators

**Goal.** Finish Phase 2 by adding the surface-level operators that make C code feel like C: `+=`, `-=`, `*=`, `/=`, `%=`, `++` / `--` (prefix and postfix), and the ternary `?:`.

Status: **done.** First-try smoke test:

```
$ cc /ops.c -o /ops.elf
cc: wrote /ops.elf
$ ops
x += 5  -> 15
x -= 3  -> 12
x *= 2  -> 24
x /= 4  -> 6
x % 4  -> 2
y    = 10
++y  = 11
y    = 11
y++  = 11
y    = 12
--y  = 11
y--  = 11
y    = 10
counter = 214
max(5, 9) = 9
min(5, 9) = 5
|-42|     = 42
sum 0..4  = 10
```

Every line matches expectations including the canonical postfix-in-loop idiom `total += n++;` (which sums 0+1+2+3+4=10 by reading the old `n` then incrementing). Regressions verified: `mini.c` (basic char arrays), `chars.elf`, `prep.elf`, `printf.elf`, `hello.elf` all still produce identical output to their previous sessions.

**Path B Phase 2 is now complete.**

---

## Compound assignment: a one-line parser rewrite

`x += expr` is semantically identical to `x = x + expr` when `x` is a name (no side-effects on re-read). So instead of adding a `N_COMPOUND_ASSIGN` AST node + a new codegen path, the parser just builds the equivalent `N_ASSIGN(N_BIN(...))` tree:

```c
/* NAME += expr;  →  NAME = NAME + expr; */
if (op) {
    char nm[NAME_MAX];
    /* ...copy current name token into nm... */
    g_tk++;       /* skip NAME */
    g_tk++;       /* skip op= */
    struct node *rhs = parse_expr();
    expect(T_SEMI, "';'");
    struct node *left = new_node(N_NAME);
    /* ...copy nm into left->name... */
    struct node *bin = new_node(N_BIN);
    bin->op = op;
    bin->a  = left;
    bin->b  = rhs;
    struct node *n = new_node(N_ASSIGN);
    /* ...copy nm into n->name... */
    n->a = bin;
    return n;
}
```

No new codegen, no new tests. The existing arithmetic + assignment paths just work, including for globals (which N_ASSIGN already handles correctly via session 93). Cost: ~30 lines of parser, zero codegen.

This is a recurring theme in cc — when a new feature is expressible as a tree rewrite of existing nodes, do that instead of adding a code path.

---

## `++` / `--` as a separate node

Increment and decrement DO need their own AST node because the value semantics differ between prefix and postfix:

- `++x` — increment then yield new value
- `x++` — yield old value, increment
- Same shape for `--`

The AST: `N_INC_DEC` with `op = T_INC | T_DEC` and `num = 1` for prefix, `0` for postfix. Codegen branches on those:

```c
case N_INC_DEC: {
    int delta_byte = (op == T_INC) ? 0x40 : 0x48;  /* inc/dec eax */
    /* Load current value into eax (local or global). */
    load_value(name);
    if (is_prefix) {
        emit_b(delta_byte);   /* modify... */
        store_back(name);      /* ...and write. eax already has new value. */
    } else {
        e_push_eax();          /* save old */
        emit_b(delta_byte);
        store_back(name);
        e_pop_eax();           /* eax = old value */
    }
    return;
}
```

`inc eax` / `dec eax` are 1-byte instructions (`0x40`–`0x47` for inc, `0x48`–`0x4f` for dec). So a postfix increment is 6 bytes of code plus the load/store: `push; inc; store; pop`. A prefix is the same minus push/pop: `inc; store`.

The restriction "target must be a NAME" is the same one we put on `&NAME` in session 92. Lifting it to `arr[i]++` requires recomputing the address twice (once for load, once for store) or saving it across the modify. Easy in principle but not in scope.

---

## Ternary

The classic compile of `c ? a : b`:

```
gen_expr(c)              ; eax = condition
test eax, eax
jz   .else
gen_expr(a)              ; then branch — leaves result in eax
jmp  .end
.else:
gen_expr(b)              ; else branch — leaves result in eax
.end:
```

Two branches patched at emit time via `e_jz_rel32` and `e_jmp_rel32`:

```c
case N_TERNARY: {
    gen_expr(n->a);
    e_test_eax_eax();
    int jz   = e_jz_rel32();
    gen_expr(n->b);
    int jend = e_jmp_rel32();
    patch_d(jz,   g_code_len - (jz   + 4));
    gen_expr(n->c);
    patch_d(jend, g_code_len - (jend + 4));
    return;
}
```

In `parse_expr`, ternary lives at the bottom of the precedence ladder — after the binop chain returns its left-hand side, we check for `?` and recurse into `parse_expr` twice (then and else branches). Right-associative naturally falls out of right-recursion: `a ? b : c ? d : e` parses as `a ? b : (c ? d : e)`.

---

## What ternary makes possible

Some idioms that needed `if`/`else` statements before are now expressions:

```c
int m = a > b ? a : b;                /* max */
int absx = x < 0 ? -x : x;            /* abs */
return ok ? VALUE_OK : VALUE_FAIL;     /* one-line dispatch */
printf("%s\n", debug ? "yes" : "no"); /* inline format */
```

The last one is particularly nice because ternary expressions can appear inside function arguments — no need for a temporary.

---

## Bitwise compound ops, sort of

I deliberately skipped `&=`, `|=`, `^=`, `<<=`, `>>=`. Same parser rewrite trick would handle them in another half-hour, but compound bitwise ops in C are less common than arithmetic compound ops. Add when something needs them.

`++` and `--` on `arr[i]` are similarly skipped — they require either index recomputation or a temporary, both of which feel out of scope for the session.

The comma operator (`a, b`) was also skipped — it changes precedence of common idioms (`for (i = 0, j = 0; ...; i++, j++)`) but the same effect is achievable by splitting into separate statements.

---

## Phase 2 complete: what cc looks like now

Adding up sessions 90–96:

| Session | Feature |
|---|---|
| 90 | int types, basic ops, if/while/return, functions, intrinsics |
| 91 | string literals, puts/print_str |
| 92 | char, pointers, arrays, &/*, indexing |
| 93 | globals (with integer initializers) |
| 94 | printf (compile-time dispatch, %d %s %c %x %%) |
| 95 | preprocessor (#define, #include, #ifdef, #ifndef, #else, #endif, #undef) |
| 96 | compound assignment, ++/--, ternary |

A program that exercises most of the surface:

```c
#include "colors.h"
#define MAX_LINE 80

int total;

int main() {
    char buf[MAX_LINE];
    int i;
    i = 0;
    while (i < 5) {
        buf[i] = 'a' + i;
        ++i;
    }
    buf[i] = 0;
    total += i;
    printf("line: %s (%d chars; total = %d)\n", buf, i, total);
    return total > 0 ? 0 : 1;
}
```

Every line of that program compiles and runs on AdventOS today. The compiler is roughly **2,100 lines of C** across a single file (cc.c). It emits 32-bit ELF binaries that the kernel loads with the same `elf_load` path it uses for everything else.

---

## Decision point: keep extending cc, or port tcc?

The clear gaps in cc vs. real C are:

- **No struct/union/enum/typedef.** Real C programs lean on these.
- **No function pointers.** Limits dynamic dispatch.
- **No multi-file compilation (linker).** Programs are one .c file (after `#include`).
- **No real variadic functions.** `printf` is hardcoded.
- **No scaled pointer arithmetic.** `int *p; p+1` is one byte, not four.
- **No optimizations.** Bulky code; no register allocator.

Each of those is multi-session work. Cumulative: maybe 8–12 more sessions to close the gap to "compile real C". At that point we'd have basically reinvented tcc, with a unique bug surface.

The alternative is to port `tcc` itself, which is ~50 KLoC and depends on a libc subset. Bigger upfront cost but you get every C99 feature + decades of bug fixing.

I'll leave that decision for whenever it matters. Right now cc compiles enough to demonstrate self-hosting in principle, with the documented gaps. Phase 2 is done; the next session can be on something other than the compiler.

---

## Files touched

- `user/cc.c` — token types `T_PLUS_EQ`/`T_MINUS_EQ`/etc., `T_INC`/`T_DEC`, `T_QUESTION`/`T_COLON`; lexer extensions for those; AST nodes `N_COMPOUND_ASSIGN` (unused — rewritten at parse time), `N_INC_DEC`, `N_TERNARY`; parser hooks; codegen for `N_INC_DEC` and `N_TERNARY`. ~100 lines added.
- `fs/ops.c` — sample exercising every form.
- `fs/man/cc` — updated.
- `mkfs.py` — added `ops.c`.

cc.bin: 247 KiB → 249 KiB. The increment is tiny because the parser-rewrite trick for compound assign reuses existing codegen.

---

## Path B Phase 2 — done

Seven sessions, ~2100 lines of cc.c, six sample programs in `fs/`. We started with int-only with no strings; we end with int + char + pointers + arrays + globals + printf + the preprocessor + the natural operators that make C code look like C code.

Whatever comes next, cc will be sitting there ready to compile it.
