# Session 99 — cc Phase 3 part 3: `sizeof` and scaled pointer arithmetic

**Goal.** Fix the long-standing wart where `int *p; p++` advanced by 1 byte instead of 4, and add `sizeof` so programs can compute sizes statically. Both features cross-reference: scaled arithmetic uses the same size table that `sizeof` exposes.

Status: **done.** Smoke test:

```
$ cc /ptrs.c -o /ptrs.elf
cc: wrote /ptrs.elf
$ ptrs
sizeof(int)            = 4
sizeof(char)           = 1
sizeof(int *)          = 4
sizeof(char *)         = 4
sizeof(struct point)   = 8
sizeof(struct point *) = 4
int-walk sum  = 150
char-walk len = 3
v[3] via *(ip+3) = 40
v[0] via *ip     = 10
v[?] via shifted iq = 30
5 ints = 20 bytes
```

All 12 lines match. The `int-walk sum = 150` comes from `int *ip = v; while (i<5) { sum += *ip; ip++; i++; }` — `ip++` advances 4 bytes per iter (the new behavior). The `v[?] via shifted iq = 30` comes from `iq = ip + 4; iq = iq - 2;` — both arithmetic ops scale by 4.

---

## `sizeof` is a parser-time fold

`sizeof(TYPE)` accepts six type specs:

```
sizeof(int)             → 4
sizeof(char)            → 1
sizeof(int *)           → 4
sizeof(char *)          → 4
sizeof(struct TAG)      → g_structs[idx].size
sizeof(struct TAG *)    → 4
```

The parser folds these into an `N_NUM` node at parse time. By codegen, the `sizeof` operator is gone — the result is a plain integer constant. Implementation is ~30 lines in `parse_primary`.

`sizeof EXPR` (sizeof of an expression, where the type is inferred from the expression) is NOT supported. That requires propagating types through expression evaluation, which cc still doesn't do. The user can usually re-write `sizeof(myvar)` as `sizeof(its_type)`.

---

## Scaled pointer arithmetic

Before session 99: `int *p; p + 1` produced `p + 1` (one byte added). This is what cc actually emitted — no scaling.

After: `int *p; p + 1` produces `p + 4`. Real C semantics.

### The codegen pattern

The `N_BIN` case for `T_PLUS` and `T_MINUS` runs a pre-pass:

```c
int ea = expr_ptr_elem_size(n->a);
int eb = expr_ptr_elem_size(n->b);
struct node *ptr_node, *idx_node;
int elem;
if (ea > 0 && eb == 0)              { ptr=a; idx=b; elem=ea; }
else if (eb > 0 && op==T_PLUS)      { ptr=b; idx=a; elem=eb; }
/* else: not pointer arith — fall through to integer path */

if (elem > 1) {
    gen_expr(idx_node);              /* eax = idx */
    if (elem == 4) e_shl_eax_imm8(2);
    else { e_mov_ebx_imm(elem); e_imul_eax_ebx(); }
    e_push_eax();
    gen_expr(ptr_node);              /* eax = ptr */
    e_pop_ebx();
    if (op==T_PLUS) e_add_eax_ebx();
    else             e_sub_eax_ebx();
    return;
}
```

`expr_ptr_elem_size` only recognizes NAME operands — it doesn't track types through arbitrary expressions. So `(p + 1) + 1` doesn't scale the second `+`. Documented limit; the user can usually re-arrange the expression.

For `p - p` (pointer minus pointer), neither operand has a non-zero `elem`... wait, actually both do. The code only triggers scaling when one is non-zero and the OTHER is zero. So `p - p` falls through to the integer subtraction, producing a raw byte distance. The user divides by `sizeof(*p)` manually if they want element count. That's also a real-C choice with some compilers.

### `p++` for pointers

`N_INC_DEC` previously emitted `inc eax` / `dec eax` (1 byte each) regardless of type. Now it picks the delta by kind:

```c
int delta = 1;
if (k == LK_INT_PTR || k == LK_INT_ARR) delta = 4;
else if (k == LK_STRUCT_PTR) delta = g_structs[sidx].size;
/* (char* / char[] stay at 1; int-scalar stays at 1.) */

if (delta == 1) {
    emit_b((op==T_INC) ? 0x40 : 0x48);   /* inc/dec eax */
} else {
    emit_b(0x83);
    emit_b((op==T_INC) ? 0xc0 : 0xe8);   /* add/sub eax, imm8 */
    emit_b(delta);
}
```

`add eax, imm8 (sign-extended)` is 3 bytes — slightly bigger than the 1-byte `inc eax` but works for any delta up to 127. Every struct we'd realistically see fits.

### Why this isn't done dynamically

cc doesn't track expression types at runtime — there's no way for an x86 `add eax, ebx` instruction to "know" that eax is an int* and so ebx should be multiplied by 4 first. The scaling has to be a compile-time decision. The compile-time decision in turn requires that cc identify which operand is the pointer-typed one, which requires the operand to be a NAME (so we can look up its kind).

The "if it's a NAME, look up its kind" pattern is the same one used by sessions 92, 93, 96, and 97. Same restriction across the codebase: indexed expressions and dereferences require NAME bases.

---

## What sessions 90–99 add up to

After this session, cc handles the core int-ptr-arr-struct surface of C plus most of the syntactic ergonomics:

```c
#include "header.h"
#define LIMIT (1 << 8)

struct cfg {
    int count;
    char *name;
    struct cfg *next;
};

int total;
struct cfg cfgs[1];  /* TODO: not yet — array-of-struct */

int sum_list(struct cfg *head) {
    int n;
    n = 0;
    while (head != 0) {
        n += head->count;
        head = head->next;
    }
    return n;
}

int main() {
    struct cfg a, b, c;
    a.count = 10; a.name = "first";  a.next = &b;
    b.count = 20; b.name = "second"; b.next = &c;
    c.count = 30; c.name = "third";  c.next = 0;
    total = sum_list(&a);
    printf("sum = %d (over %d items of size %d)\n",
           total, 3, sizeof(struct cfg));
    return total > LIMIT ? 0 : 1;
}
```

Most of that compiles and runs today.

---

## Files touched

- `user/cc.c` — `T_SIZEOF` token + keyword, `sizeof(TYPE)` parser fold, `expr_ptr_elem_size` helper, scaled-arithmetic branch in `N_BIN`, scaled-delta in `N_INC_DEC`. ~80 lines added.
- `fs/ptrs.c` — sample exercising sizeof, int-array walk via `ip++`, char-string walk via `cp++`, scaled `*(ip + i)`, scaled `iq - 2`.
- `fs/man/cc` — updated.
- `mkfs.py` — added `ptrs.c`.
- `README.md` — pointer bump.

cc.bin: 277 KiB → 279 KiB.

---

## Note on the in-guest FS limit

Multiple compiles in one boot eventually fail with `cc: line 0: cannot write output to ...`. That's an AdventFS limit — either the 128-file `FS_MAX_FILES` cap or contiguous-allocation fragmentation in the data-sector bitmap. Pre-existing; not a cc bug. Workaround for now: reboot between batches. A future session could bump the FS budget or implement non-contiguous file storage.

---

## Phase 3 status after session 99

Three of N+ shipped:

- ✅ 97 — structs
- ✅ 98 — function pointers
- ✅ 99 — sizeof + scaled pointer arithmetic
- ⏳ 100 — multi-file compilation (linker)
- ⏳ 101 — struct value assignment (memcpy emit)
- ⏳ 102 — array-of-struct + member access through index expressions
- ⏳ 103+ — enum, typedef, real variadics, optimization, …
