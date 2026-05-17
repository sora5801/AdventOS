# Session 101 — cc Phase 3 part 5: struct value assignment

**Goal.** Let `copy = orig;` work when both are struct values. Up to this session, programs had to copy each field by hand. After: a single `=` triggers a `rep movsd` memcpy.

Status: **done.** Extended `fs/structs.c` with three additional asserts:

```
$ cc /structs.c -o /sx.elf
cc: wrote /sx.elf
$ sx                                     ; exit 6 (linked-list sum)
p = (3, 4)
dot(p, q) = 63
after swap: p = (4, 3)
q via r = (100, 200)
linked-list sum = 6
after copy: (42, 99)
orig is now (0, 0)
copy still   (42, 99)
```

The last three lines exercise the new path:

```c
struct point orig;
orig.x = 42; orig.y = 99;
struct point copy;
copy = orig;             /* the new feature */
orig.x = 0; orig.y = 0;
/* copy is unaffected — it's a deep copy, not an alias */
```

---

## The `rep movsd` shape

`N_ASSIGN` codegen now branches on the LHS's kind. For `LK_STRUCT`:

```c
if (k == LK_STRUCT) {
    /* Verify RHS is also a NAME of the same struct type. */
    int sz = g_structs[sidx].size;
    int dwords = sz / 4;

    push esi                    ; preserve callee-saved
    push edi
    mov  esi, &rhs              ; lea esi, [ebp + r_off]   for local
                                ; mov esi, GLOBAL_VA       for global
    mov  edi, &lhs              ; lea edi, [ebp + l_off]   for local
                                ; mov edi, GLOBAL_VA       for global
    mov  ecx, dwords            ; b9 imm32
    rep  movsd                  ; f3 a5
    pop  edi
    pop  esi
    return;
}
/* else: fall through to scalar gen_expr + e_store_local */
```

The size in dwords is `g_structs[sidx].size / 4`, which is always exact because every field reserves 4 bytes (the session 97 simplification).

ESI and EDI are callee-saved in cdecl, so we push them before the memcpy and pop after. ECX gets clobbered but the caller doesn't depend on it for cdecl.

For struct globals, the source/destination address comes from `mov esi/edi, GLOBAL_VA` with a glob_fixup — the same mechanism already used for `emit_load_global` etc.

### Why not unroll instead?

For a 2-dword struct, `rep movsd` is ~12 bytes including the register-save/restore. Unrolled `mov eax, [esi+0]; mov [edi+0], eax; mov eax, [esi+4]; mov [edi+4], eax` is ~10 bytes — slightly smaller. But for an 8-dword struct (32 bytes), the unrolled form balloons while `rep movsd` stays the same size.

The size-agnostic property of `rep movsd` is worth the 2-byte overhead. And it's simpler codegen.

---

## Restrictions

LHS must be a NAME of `LK_STRUCT` kind. RHS must be a NAME of the SAME struct type. So:

```c
struct point a, b;
a = b;                  /* OK */

struct point a;
struct other b;
a = b;                  /* error: type mismatch */

struct point *p;
struct point b;
*p = b;                 /* not supported yet (would need struct
                          deref-assign codegen path) */

struct point f() { ... }
struct point a;
a = f();                /* not supported — cc doesn't return structs */
```

The first case is the common one and is what this session enables. The others are reasonable next steps but each is a session of its own.

---

## What struct value assignment unlocks

Functions that return data by populating a pointed-to struct don't need to. Programs that hold a "current state" struct and want to snapshot it can. Linked lists where a node sometimes overwrites another node's payload in-place can use a one-line `dst->value = src->value`... well, no — that's arrow-assign for a field, not whole-struct. But `*dst_node = *src_node` (when we add that path) would work.

The most common idiom this unblocks: "save a copy, modify the original, optionally restore":

```c
struct config saved;
saved = current;       /* snapshot */
current.x = new_x;     /* mutate */
/* ... if condition fails, undo: */
current = saved;        /* restore */
```

That's not possible with field-by-field copies — too many fields to maintain.

---

## Files touched

- `user/cc.c` — `N_ASSIGN` codegen branches on `LK_STRUCT`; emits push esi/edi, lea/mov for source and dest, `mov ecx, dwords`, `rep movsd`, pop edi/esi. ~50 lines added.
- `fs/structs.c` — extended with the copy+mutate+verify sequence at the end.
- `fs/man/cc` — added the struct-assign line; removed the "no struct value assignment" restriction.

cc.bin: 279 KiB → 280 KiB.

---

## Phase 3 status after session 101

Five of N+ shipped:

- ✅ 97 — structs
- ✅ 98 — function pointers
- ✅ 99 — sizeof + scaled pointer arithmetic
- ✅ 100 — multi-file compilation
- ✅ 101 — struct value assignment
- ⏳ 102 — array-of-struct + indexed member access (`pts[i].x = ...`)
- ⏳ 103+ — enum, typedef, real variadics, optimization, struct-by-value in calls
