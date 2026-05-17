# Session 103 — cc Phase 3 part 7: enums

**Goal.** Add `enum` so programs can use named integer constants instead of stringing together `#define` macros or hard-coding numbers in conditionals. Small feature; high ergonomic payoff.

Status: **done.** Smoke test:

```
$ cc /enums.c -o /enums.elf
cc: wrote /enums.elf
$ enums                                ; exit code 16 (ERR_TIMEOUT)
RED=0 GREEN=1 BLUE=2
WHITE=7 OFF_WHITE=8
RGB combined = 3
ok
busy
denied
timeout
unknown
```

All cases match: auto-increment from 0 (`RED=0, GREEN=1, BLUE=2`), explicit value resumes auto-increment (`WHITE=7, OFF_WHITE=8`), bitwise OR over enum constants gives the expected 3, and a `describe(int)` function pattern-matches against anonymous-enum constants from another scope.

---

## What enums look like in cc

Two forms:

```c
enum color {
    RED,                /* 0 */
    GREEN,              /* 1 */
    BLUE,               /* 2 */
    WHITE = 7,          /* 7 — explicit */
    OFF_WHITE           /* 8 — auto-increment resumes */
};

enum {
    ERR_OK = 0,
    ERR_BUSY,
    ERR_DENIED,
    ERR_TIMEOUT = 16
};
```

Both work the same way: the enumerators get added to a global `g_enum_consts[]` table with their values. The tag (if any) is parsed but ignored — cc doesn't distinguish enum types from ints, so `enum color x;` would just be `int x;` semantically. Real C does the same thing — enum is sugar for int.

---

## Lookup happens in `parse_primary`

When the parser sees an identifier in expression context, it checks for an enum match BEFORE creating an `N_NAME` node:

```c
if (t->kind == T_NAME) {
    if (tk_peek(1)->kind != T_LPAREN) {
        int ei = enum_find(t->name);
        if (ei >= 0) {
            struct node *n = new_node(N_NUM);
            n->num = g_enum_consts[ei].value;
            g_tk++;
            return n;
        }
    }
    /* ...fall through to N_NAME / call parsing as before... */
}
```

This means enum constants resolve to `N_NUM` at parse time — by codegen, they're indistinguishable from numeric literals. No runtime cost, no separate constant section.

The `tk_peek(1) != T_LPAREN` guard is paranoid: it makes sure that `foo()` is treated as a function call even if `foo` is also an enum constant. In practice the namespaces don't overlap (enum constants and function names are in the same global table), but the guard makes the intent explicit.

---

## Three useful idioms unlocked

**Flags / bitmasks.** Define each flag as a power-of-2 enum, then OR them together:

```c
enum { F_BOLD = 1, F_ITALIC = 2, F_UNDERLINE = 4 };
int style = F_BOLD | F_UNDERLINE;
```

**Error codes.** Replace bare `return -1;` with named codes:

```c
enum { ERR_OK = 0, ERR_NOMEM, ERR_BUSY, ERR_TIMEOUT = 16 };
int op() {
    if (out_of_mem()) return ERR_NOMEM;
    if (busy())       return ERR_BUSY;
    return ERR_OK;
}
```

**State machines.** Each state is a named constant; the current-state variable is an int:

```c
enum { STATE_IDLE, STATE_WAIT, STATE_RUNNING, STATE_DONE };
int state = STATE_IDLE;
while (state != STATE_DONE) {
    if (state == STATE_IDLE && trigger()) state = STATE_WAIT;
    /* ... */
}
```

All three were doable before via `#define`, but enums:

- Auto-increment so adding a new value in the middle doesn't renumber the rest.
- Stay in the namespace as constants, not text substitutions — debuggers, future symbol tables, etc. would see the names.
- Group logically related constants together visually.

---

## Compared to `#define`

| | enum | #define |
|---|---|---|
| Value type | always int | text substitution; any tokens |
| Auto-increment | yes | manual |
| Scope | global (cc doesn't have nested enums) | global |
| Lookup cost | O(N) at parse time | O(N) at preprocess time |
| Debugger visibility | constants persist as identifiers in cc | gone after preprocess |

For the kinds of programs cc compiles, both work; enum is just cleaner.

---

## What's still missing

- **enum as a type with a tag.** `enum color x;` parses (the tag is consumed by the parser) but the local is treated as a plain int. Real C would let you write `enum color c; c = RED;` and the compiler might warn if you assigned a non-RED-family value. cc has no warnings.
- **anonymous enum used as type.** `enum { X, Y } var;` — declaration-time creation + use. Real C accepts this; cc requires the enum and the variable to be separate declarations.
- **Forward enum declarations.** `enum tag;` without a body. Not parsed.

Each is a parser extension. None blocks the common uses.

---

## Files touched

- `user/cc.c` — `T_ENUM` token + `enum` keyword; `g_enum_consts[128]` registry; `enum_find` / `enum_add`; `parse_enum_top` for top-level definitions; lookup in `parse_primary` for identifier-as-constant. ~80 lines added.
- `fs/enums.c` — sample exercising tagged + anonymous, auto-increment + explicit values, conditionals + bitwise.
- `fs/man/cc` — added enum docs; removed enum from "deliberately not implemented" list.
- `mkfs.py` — added `enums.c`.
- `README.md` — pointer bump.

cc.bin: 282 KiB → 287 KiB.

---

## Phase 3 status after session 103

Seven sub-sessions shipped:

- ✅ 97 — structs
- ✅ 98 — function pointers
- ✅ 99 — sizeof + scaled pointer arithmetic
- ✅ 100 — multi-file compilation
- ✅ 101 — struct value assignment
- ✅ 102 — array-of-struct + indexed member access
- ✅ 103 — enum
- ⏳ 104+ — typedef, real variadics, struct-by-value calls, optimization, register allocator

The remaining items are all "fits one more feature into a session" candidates. cc covers the C surface area that small-to-medium real-world programs use. The post-Phase-3 question — keep extending cc, or port tcc — remains unanswered, but cc has earned itself a long enough runway that the question can wait.
