# Session 92 — cc Phase 2 part 2: char, pointers, & / *, arrays

**Goal.** Add the type-system pieces that unlock general string handling: `char`, `char *`, `int *`, `&NAME`, `*p`, `a[i]`, and local arrays. With these in, `my_strlen(char *s)` and `my_strcpy(char *dst, char *src)` can be written in cc-flavored C and exercise the same byte-level ops a real C program would.

Status: **done.** First-try smoke test:

```
$ cc /chars.c -o /chars.elf
cc: wrote /chars.elf
$ chars                              ; exit code 99
Hi!
12
copied via my_strcpy
hey
500
60000
```

The `my_strlen` and `my_strcpy` in `fs/chars.c` are written in the cc-subset itself — same byte-load and byte-store patterns the libc versions use, just with no register allocator behind them. No regressions to session 90's `hello` or session 91's `strs`.

---

## What the type system actually does

`cc` stays untyped at the expression level — `gen_expr` returns "the value is in eax" and never carries a static type. The minimal type info lives on declarations:

```c
enum {
    LK_INT,         /* int x  — 4-byte scalar */
    LK_INT_PTR,     /* int *p — 4-byte pointer, *p reads 4 bytes */
    LK_CHAR_PTR,    /* char *p — 4-byte pointer, *p reads 1 byte */
    LK_INT_ARR,     /* int x[N] — 4*N bytes, name decays to int* */
    LK_CHAR_ARR,    /* char x[N] — N bytes, name decays to char* */
};
```

Each `local_slot` carries an `LK_*` tag. Function param nodes carry the tag in `op` so the body sees the same kind the caller declared.

Two helpers fall out of this:

```c
static int kind_elem_size(int k) {
    if (k == LK_CHAR_PTR || k == LK_CHAR_ARR) return 1;
    if (k == LK_INT_PTR  || k == LK_INT_ARR)  return 4;
    return 0;
}
static int kind_is_array(int k) {
    return k == LK_INT_ARR || k == LK_CHAR_ARR;
}
```

`kind_elem_size` decides byte vs word load/store and index scaling. `kind_is_array` is the "decay to address" bit — for array kinds, `gen_expr` of the name emits `lea eax, [ebp+off]` (address), not `mov eax, [ebp+off]` (value).

That's all the type system that this session needs. No widening, no implicit conversions, no diagnostics for mixing kinds. Mostly because the expressions we generate aren't sensitive to width — they all run in eax as 32-bit ints.

---

## Codegen for the new ops

All four new ops follow the same shape: figure out a load width or store width from the LK_* tag, then emit straight-line code.

### `&NAME`

```
lea eax, [ebp + off]
```

That's it. The kind doesn't matter — addresses are addresses.

### `*p` (where `p` is a NAME)

```
mov  eax, [ebp + off]      ; eax = pointer value
mov  eax, [eax]            ; LK_INT_PTR / LK_INT_ARR
   OR
movzx eax, byte [eax]      ; LK_CHAR_PTR / LK_CHAR_ARR (zero-extended)
```

`movzx` produces an unsigned byte read so the higher 24 bits are zero. That's the choice both gcc and tcc make for unsigned char; signed char would `movsx`. We treat `char` as unsigned.

### `a[i]` (where `a` is a NAME)

```
;   base address into eax
lea  eax, [ebp+off]        ; for arrays
   OR
mov  eax, [ebp+off]        ; for pointers
push eax
;   index → eax
<gen_expr(i)>
shl  eax, 2                ; only for elem_size == 4
;   add and load
pop  ebx                   ; ebx = base
add  eax, ebx              ; eax = base + scaled-idx
;   load through the address
mov  eax, [eax]            ; or movzx eax, byte [eax]
```

The push/pop dance keeps `i` free to use any registers the inner expression needs.

### `a[i] = val` and `*p = val`

Symmetric to the load form but with one extra `push` to keep the right-hand side around while we compute the destination address:

```
<gen_expr(val)>            ; eax = value to store
push eax
<address-of-a[i] into eax> ; same shape as load path, minus the final load
mov  ebx, eax              ; ebx = addr
pop  eax                   ; eax = value
mov  [ebx], eax            ; or mov [ebx], al
```

The choice of `mov [ebx], al` (`88 03`) vs `mov [ebx], eax` (`89 03`) is again driven by `kind_elem_size`. The `mov [ebx], al` form uses just the low 8 bits, so the upper 24 bits of the value silently truncate — same as C's "narrowing conversion."

---

## Arrays and stack frame growth

`local_declare` used to always add 4 to `g_locals_bytes`. Now there's `local_declare_sized(name, size, kind)`:

```c
int padded = (size + 3) & ~3;     /* keep 4-byte alignment */
g_locals_bytes += padded;
```

So `char buf[64]` reserves 64 bytes (already padded). `char buf[5]` reserves 8. `int v[4]` reserves 16. The `(size+3) & ~3` keeps every subsequent local at a 4-byte-aligned `ebp` offset, which matters for `int` loads / stores via `mov eax, [ebp+off]`.

Array element 0 lives at the lowest address of the padded block, which is also `ebp + off`. Element i is at `ebp + off + i * elem_size`. The earlier "watermark" approach to function-prologue stack reservation still works — `gen_func` patches `sub esp, g_locals_bytes` after the whole body is generated.

---

## Parser: where the type info comes in

`parse_stmt` and `parse_func` both now lead with `T_INT` *or* `T_CHAR`, optionally followed by `T_STAR`, then a name, then optionally a `[N]`:

```c
int  x;            /* LK_INT */
char x;            /* LK_INT — scalar char is int-shaped */
int  *p;           /* LK_INT_PTR */
char *p;           /* LK_CHAR_PTR */
int  v[4];         /* LK_INT_ARR,  num=4 */
char buf[64];      /* LK_CHAR_ARR, num=64 */
```

For variable decls, the kind is stuffed into the AST node's `op` field; for params, into the param-name node's `op`. `gen_func` reads `params[i]->op` when binding locals at function entry.

Assignment statements need to recognize three new shapes:

```c
NAME = expr;          /* old N_ASSIGN */
*NAME = expr;         /* N_DEREF_ASSIGN */
NAME[idx] = expr;     /* N_INDEX_ASSIGN */
```

The third shape has a lookahead problem — `a[i]` can be either a statement (`a[i] = x;`) or an expression-statement (`a[i];`). The parser commits to the index expression, then peeks for `=`; if it's missing, it rolls `g_tk` back to before the name and falls through to the generic expr-stmt parse.

Indexing as an rvalue is handled in `parse_primary` as a postfix operator immediately after a NAME, so `a[i]` parses fine in any expression context that takes a primary.

---

## What this unlocks

`my_strlen` is the canonical example:

```c
int my_strlen(char *s) {
    int n;
    n = 0;
    while (*s != 0) {
        n = n + 1;
        s = s + 1;          /* pointer arithmetic: s++ */
    }
    return n;
}
```

`s + 1` works because the pointer is just an int internally — adding 1 advances by 1 byte, which is right for `char *`. The compiler doesn't scale by `sizeof(*s)` the way real C does, so `int* + 1` would advance by 1 byte instead of 4. That's a wart I'll fix in a later session if it bites.

`my_strcpy`:

```c
int my_strcpy(char *dst, char *src) {
    int i;
    i = 0;
    while (src[i] != 0) {
        dst[i] = src[i];
        i = i + 1;
    }
    dst[i] = 0;
    return i;
}
```

`src[i]` and `dst[i]` both use the byte-load and byte-store paths because the params are `char *`. The same code with `int *` params would use dword-wide loads and stores.

And `puts(buf)` works even though `buf` is a `char[]`, because the name-decay-to-address rule converts it to a `char *` automatically.

---

## Two limits worth knowing about

1. **`a[i]` only works when `a` is a NAME.** `f()[i]`, `(p+3)[i]`, `arr[i][j]`, `*(p+i)` all fail to parse cleanly. The parser is restricted to recognize `NAME [`, partly to keep the assignment lookahead manageable. Fixing it means making indexing a real postfix expression operator over `parse_primary`'s output, which I'd rather do alongside more operators in a later session.

2. **Pointer arithmetic doesn't scale by `sizeof(*p)`.** `int *p; p = p + 1;` advances p by 1 byte, not 4. Real C scales by element size. For `char *` (the more common case) the difference is invisible. For `int *` you have to manually multiply by 4. Documented limit; cleanup candidate.

Neither bites the kinds of programs cc is intended to compile (small CLI tools, simple data manipulations).

---

## Files touched

- `user/cc.c` — `T_CHAR` keyword, `[ ]` tokens, char literals, LK_* enum, `local_declare_sized`, `local_kind`, new helpers `e_lea_eax_ebp` / `e_load_eax_at_eax` / `e_loadb_eax_at_eax` / `e_store_eax_at_ebx` / `e_storeb_al_at_ebx` / `e_shl_eax_imm8`, parser support for `char`/`*`/`[N]` decls + `&NAME` + `*expr` + postfix `[]`, codegen for N_DEREF / N_ADDR_OF / N_INDEX / N_INDEX_ASSIGN / N_DEREF_ASSIGN / N_ARR_DECL, and array-name decay in N_NAME. ~250 lines added.
- `fs/chars.c` — new sample.
- `fs/man/cc` — char/pointer/array docs.
- `mkfs.py` — added `chars.c`.

cc.bin: 179 KiB → 185 KiB.

---

## Next on Path B

The remaining Phase 2 sub-sessions, ordered:

- **93** — global variables. One new symbol table; `.data` section in the binary; address-resolution for global names.
- **94** — `printf` (variadic functions + format-string parsing).
- **95** — preprocessor: `#define`, `#include`, basic `#ifdef`/`#endif`.
- **96** — extra operators: `+=`, `++`, `--`, ternary, comma.
- Maybe **97** — scaled pointer arithmetic, multi-dimensional indexing, the rest of the indexing flexibility.

After all that, cc compiles small real programs end-to-end. The next decision is whether to keep extending cc or pivot to a real `tcc` port — at that point the tradeoffs are visible (cc is small and ours; tcc is the standard).
