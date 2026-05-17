# Session 102 — cc Phase 3 part 6: array-of-struct + indexed member access

**Goal.** Two related features that fill an awkward gap left by sessions 97 and 92:

1. **`struct TAG NAME[N];`** — a local or global array of struct values.
2. **`NAME[i].field`** and **`NAME[i].field = expr;`** — read and write members of an indexed struct.

Before this session, programs that wanted N records of struct data had to declare them as N separate variables (`struct point p0, p1, p2;`) and unroll the access loop by hand. With this session, the canonical "table of records" pattern works.

Status: **done.** Smoke test:

```
$ cc /sarr.c -o /sarr.elf
cc: wrote /sarr.elf
$ sarr                                  ; exit code 30
sum = 21
  100: alpha = 1
  200: beta = 4
  300: gamma = 9
  400: delta = 16
total value = 30
```

The `100: alpha = 1` line came from `items[0].id`, `items[0].name`, `items[0].value` reads through a `while (i < 4)` loop — all driven by a single `items[i].field` expression syntax. Three struct fields per record, one of them a `char*`.

---

## New `LK_STRUCT_ARR` kind

The `local_slot.kind` enum already had `LK_STRUCT` (value) and `LK_STRUCT_PTR` (pointer). Adding `LK_STRUCT_ARR = 7` gives the parser a way to distinguish "an array of N structs" from those:

```c
LK_STRUCT     = 5,   /* struct T x     — N=1 struct value */
LK_STRUCT_PTR = 6,   /* struct T *p    — pointer to a struct (4 bytes) */
LK_STRUCT_ARR = 7,   /* struct T x[N]  — N consecutive structs */
```

`kind_is_array(LK_STRUCT_ARR)` returns true (name decays to address), and `kind_is_pointerlike(LK_STRUCT_ARR)` returns true (indexable). `expr_ptr_elem_size` returns the struct size for either `LK_STRUCT_PTR` or `LK_STRUCT_ARR`. The existing infrastructure carried it.

---

## Parser

`parse_stmt`'s `T_STRUCT` branch grows a new arm for `[N]`:

```c
if (!is_ptr && accept(T_LBRACKET)) {
    /* int N = ... */
    expect(T_RBRACKET, ...);
    expect(T_SEMI, ...);
    struct node *n = new_node(N_STRUCT_DECL);
    n->op     = LK_STRUCT_ARR;
    n->num    = sidx;
    n->n_list = N;          /* repurposed: array length */
    return n;
}
```

`N_STRUCT_DECL` codegen reads `n->n_list` to compute total bytes: `g_structs[sidx].size * N`.

`parse_primary` extends its postfix `NAME[idx]` handler — after the closing `]`, if the next token is `.`, we make an `N_INDEX_MEMBER` instead of `N_INDEX`. Similarly, `parse_stmt`'s `NAME[idx] = ...` handler adds an `N_INDEX_MEMBER_ASSIGN` form when it sees `.field` before the `=`.

Two new AST nodes total. Their `name` is the array base, `field_name` is the field, `a` is the index expression. For assigns, `b` is the rhs.

---

## Codegen

Both `N_INDEX_MEMBER` and `N_INDEX_MEMBER_ASSIGN` share the same address-computation pattern:

```
;   base address into eax
lea  eax, [ebp + off]        ; or  mov eax, GLOBAL_VA  for globals
push eax

;   scaled index into eax
gen_expr(idx)
imul/shl by struct_size

;   pop base, add to scaled idx
pop  ebx
add  eax, ebx                ; eax = base + i * elem_size

;   add field offset
add  eax, field_offset       ; 05 imm32 (special EAX form)

;   for the read form: load
mov  eax, [eax]

;   for the assign form: save addr in ebx, pop the saved rhs, store
mov  ebx, eax
pop  eax                     ; (rhs was pushed first, before address)
mov  [ebx], eax
```

The `shl eax, 2` shortcut is used when struct size is exactly 4 (single-field structs); otherwise `imul eax, ebx` with `mov ebx, elem_size`.

All field loads/stores are dword. Byte fields aren't supported yet (see session 97 design note).

---

## What's still missing

| Feature | Why not |
|---|---|
| `arr[i]` as a bare rvalue (whole struct) | Producing a struct value in an expression context requires copying it somewhere — we don't have a "struct return register" concept. `arr[i].field` and `&arr[i]` cover the useful cases. |
| `&arr[i]` (yields struct *) | Easy to add. Not blocking anything yet. |
| `arr[i] = struct_lit` | Struct literal syntax not supported. |
| `arr[i] = other_struct` (whole-struct copy via index) | Would need N_INDEX_MEMBER-shaped codegen for assign-without-field. Same shape as session 101's struct value assign but with an indexed source/dest. |
| `pts[i][j]` (2D struct arrays) | No multi-dimensional arrays in cc. |
| Pointer to struct array | `struct T (*p)[N]` syntax isn't parsed; `struct T *p` followed by `p[i].field` works fine because of pointer arithmetic and the `LK_STRUCT_PTR` indexing path (already in cc). |

The first one — bare `arr[i]` as rvalue — is the most-requested follow-up and a natural session 103 sub-task if anyone hits it.

---

## FS budget

Session 102 added `sarr.c` (one new file). To keep the boot image under `FS_MAX_FILES = 128`, the older session-93/94/95 sample sources were removed from mkfs.py (their content is still in docs/80, 81, 82). This is the second round of mkfs.py trimming this Phase 3.

The longer-term fix is the AdventFS limit itself — bumping `FS_MAX_FILES` to 160+ requires shrinking some other kernel BSS to stay below the VGA RAM boundary, which is a separate cleanup project.

---

## Files touched

- `user/cc.c` — `LK_STRUCT_ARR` enum value; `kind_is_array` / `kind_is_pointerlike` / `expr_ptr_elem_size` updated; parser branch for `struct TAG NAME[N];`; postfix `NAME[i].field` and assignment forms; codegen for the two new AST nodes. ~100 lines added.
- `fs/sarr.c` — sample with `struct point pts[3]` and `struct entry items[4]`.
- `fs/man/cc` — updated with the new shapes.
- `mkfs.py` — added `sarr.c`; removed `globs.c`, `printf.c`, `prep.c`, `colors.h` to stay under file cap.
- README.md — pointer bump.

cc.bin: 280 KiB → 282 KiB.

---

## Phase 3 status after session 102

Six of N+ shipped:

- ✅ 97 — structs
- ✅ 98 — function pointers
- ✅ 99 — sizeof + scaled pointer arithmetic
- ✅ 100 — multi-file compilation
- ✅ 101 — struct value assignment
- ✅ 102 — array-of-struct + indexed member access
- ⏳ 103+ — enum, typedef, real variadics, optimization, struct-by-value calls

After this session, cc handles every common "what shape of C code do users write" pattern: scalars, pointers, arrays, structs (including arrays of them, value-copy, indexed members), function pointers, multi-file projects, the preprocessor, and the printf/sizeof/operator surface. The remaining gaps are quality-of-life (typedef, enum) or specialized (real variadics, optimization).
