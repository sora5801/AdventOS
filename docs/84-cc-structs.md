# Session 97 — cc Phase 3 part 1: structs

**Goal.** Add `struct` to cc. The Phase 2 compiler couldn't represent organized data; programs had to use parallel arrays. With this session, cc compiles linked lists, polymorphic-ish dispatch via vtables-of-fields, and the rest of the struct-shaped C idioms.

Status: **done.** First-try smoke test:

```
$ cc /structs.c -o /structs.elf
cc: wrote /structs.elf
$ structs                                    ; exit code 6
p = (3, 4)
dot(p, q) = 63
after swap: p = (4, 3)
q via r = (100, 200)
linked-list sum = 6
```

`fs/structs.c` defines two struct types, declares struct values and pointers, passes struct pointers to functions, mutates fields through pointers, and walks a self-referential linked list. Every line of stdout matches the expected output. No regressions to sessions 90–96.

---

## What changed

### Lexer
- `struct` keyword → `T_STRUCT`
- `.` → `T_DOT`
- `->` → `T_ARROW`

### Type system
Two new `LK_*` kinds:
```c
LK_STRUCT     = 5,   /* struct T x  — value-typed struct */
LK_STRUCT_PTR = 6,   /* struct T *p — pointer to struct */
```

Both `local_slot` and `global_info` gain a `meta` field that, for these two kinds, holds the index into `g_structs[]`. The meta field is otherwise zero. (`local_slot.meta` is also set for params via the existing param-binding path.)

### Struct registry
```c
struct field_info {
    char name[NAME_MAX];
    int  offset;     /* byte offset within the struct */
    int  size;       /* 4 — every field is dword-sized for now */
    int  kind;       /* LK_* of the field */
    int  meta;       /* for LK_STRUCT_PTR fields: target struct_idx */
};
struct struct_info {
    char name[NAME_MAX];
    struct field_info fields[MAX_FIELDS];
    int  n_fields;
    int  size;       /* total bytes (== n_fields * 4) */
    int  defined;    /* 0 = forward-decl-only */
};
static struct struct_info g_structs[MAX_STRUCTS];
```

`MAX_STRUCTS = 32`, `MAX_FIELDS = 16`. Both bumpable but tight enough that BSS doesn't explode.

### Parser
- **Top-level** `struct TAG { fields };` definitions, plus `struct TAG NAME;` / `struct TAG *NAME;` global declarations. Disambiguated by looking for `{` after `struct TAG`.
- **Local** `struct TAG NAME;` / `struct TAG *NAME;` declarations in `parse_stmt`.
- **Member access** in `parse_primary` — `NAME.field` and `NAME->field` as postfix operators after a name. Restriction: the base is always a NAME (same restriction as `a[i]`).
- **Member assignment** in `parse_stmt` — `NAME.field = expr;` and `NAME->field = expr;` get their own statement forms (`N_MEMBER_ASSIGN`, `N_ARROW_ASSIGN`).
- **Function params** now accept `struct T *p` (struct value params are deliberately not supported — too much trouble for too little benefit).

### Codegen
All struct ops reduce to "compute the base address, add the field offset, load or store via that address." The base address is:

- For `LK_STRUCT` local: `lea eax, [ebp + off]`
- For `LK_STRUCT` global: `mov eax, GLOBAL_VA` (via the existing fixup)
- For `LK_STRUCT_PTR` local: `mov eax, [ebp + off]`  (load the pointer value)
- For `LK_STRUCT_PTR` global: `mov eax, [GLOBAL_VA]`

Then `add eax, field_offset` (`05 imm32` — the special EAX-only form), then load or store. All field loads/stores are dword for now; byte-sized fields (a real `char` in a struct) aren't supported yet.

The codepath is delightfully short — about 60 lines for the four cases (rvalue dot, rvalue arrow, dot-assign, arrow-assign), all sharing the same address-computation pattern.

---

## Forward references

Linked-list-style structs need forward references — `struct node { int val; struct node *next; };` mentions `node` before its definition is complete. The parser handles this by:

1. When parsing a struct field of type `struct OTHER *`, if `OTHER` isn't in the registry yet, allocate a forward slot (`defined=0`).
2. The slot's `size` is 0 until the real definition comes through.
3. When the definition is parsed, fill in the existing slot rather than allocating a new one.
4. `defined=1` after the body is parsed.

This handles `struct node *next` referring to its own struct's pointer type. It also handles mutual recursion (`struct A` has `struct B *`, `struct B` has `struct A *`) as long as the field is a POINTER — sizing only requires knowing that pointers are 4 bytes, not the size of the pointee.

Restriction: struct-typed fields MUST be pointers. A field like `struct point center;` (a struct value inside a struct) isn't supported because the parent struct's size becomes dependent on whether the child is fully defined — fixable with a two-pass scheme later.

---

## What the demo exercises

```c
struct point { int x; int y; };
struct node  { int val; struct node *next; };

int dot(struct point *a, struct point *b) {
    return a->x * b->x + a->y * b->y;
}
int swap_xy(struct point *p) {
    int t;
    t = p->x;
    p->x = p->y;
    p->y = t;
    return 0;
}

int main() {
    struct point p;
    p.x = 3; p.y = 4;                   /* . assign */
    printf("p = (%d, %d)\n", p.x, p.y); /* . read */
    /* ... */
    struct point *r;
    r = &q;                              /* &struct */
    r->x = 100;  r->y = 200;             /* -> assign */
    /* ... */
    /* Linked list of three nodes a -> b -> c -> 0 */
    struct node a, b, c;
    a.val = 1; a.next = &b;
    b.val = 2; b.next = &c;
    c.val = 3; c.next = 0;
    struct node *cur;
    int total = 0;
    cur = &a;
    while (cur != 0) {
        total += cur->val;     /* -> read in while-cond */
        cur = cur->next;        /* -> read assigned to ptr */
    }
    /* ... */
}
```

Everything that the older C textbooks teach as "structs are how you represent data" works. The linked-list traversal in particular is a meaningful demonstration — it exercises `&local_struct`, struct-pointer assignment to a local, traversal via `cur = cur->next`, and the `cur != 0` (NULL check) idiom.

---

## What's deferred

| Feature | Why not |
|---|---|
| Struct value passing (`int f(struct point p)`) | The cdecl ABI for structs is fiddly. Pointer params are universally supported and idiomatic enough. |
| Struct value returning (`struct point f()`) | Same. The MS-style "hidden first arg pointer" or "split into eax/edx" both add complexity. |
| Struct value assignment (`a = b;` where both are structs) | Needs a memcpy emit. Easy to add — wasn't motivated yet. |
| Nested struct values (`struct outer { struct inner inner; ... };`) | Two-pass struct sizing. Restrict to pointer fields for now. |
| Byte-sized fields | Need per-field offset alignment + byte load/store paths. All fields are dword for now. |
| `sizeof(struct T)` | Easy to add — just an integer constant lookup. |
| Arrays of struct | `struct point pts[8];` — needs element-size scaling on the array side. |
| `typedef` | We don't have typedef anyway. `struct point` is the only way to spell the type. |

Each is a clear next-step. The minimum useful struct support is in; bigger improvements are session-sized.

---

## Files touched

- `user/cc.c` — token additions, LK_STRUCT/LK_STRUCT_PTR, struct registry (~150 lines), local_declare_struct/local_meta/global_declare_struct, parse_struct_top, struct-decl parser in parse_stmt, struct member parsing in parse_primary, member-assign parser in parse_stmt, codegen for N_STRUCT_DECL/N_MEMBER/N_ARROW/N_MEMBER_ASSIGN/N_ARROW_ASSIGN, struct params in parse_func, meta propagation in gen_func's param-bind. ~300 lines added.
- `fs/structs.c` — sample (struct point + linked-list traversal).
- `fs/man/cc` — updated.
- `mkfs.py` — added structs.c.
- README.md — pointer bump.

cc.bin: 249 KiB → 276 KiB. The growth is `g_structs[32]` × `(NAME_MAX + MAX_FIELDS × sizeof(field_info) + 3 ints)` ≈ 23 KiB, plus a few KB of new codegen.

---

## Path B Phase 3 status

Session 97 of 6+ shipped. Remaining Phase 3 targets, in plausible order:

- **98** — function pointers (callbacks, dispatch tables).
- **99** — scaled pointer arithmetic (`int *p; p+1` should advance 4 bytes, not 1).
- **100** — multi-file compilation (separate .c files + a linker step).
- **101** — sizeof + struct value assignment.
- **102** — char fields and 1-byte struct field offsets.
- **103+** — `enum`, `typedef`, real variadic functions, optimization, etc.

Each is a session in itself. The post-Phase-3 question — "keep extending cc or port tcc" — gets harder to answer the more sessions go in. Today the answer is "cc is fine; keep going."
