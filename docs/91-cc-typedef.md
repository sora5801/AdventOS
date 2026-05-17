# Session 104 — cc Phase 3 part 8: typedef

**Goal.** Let programs alias type names. `typedef struct point Point;` followed by `Point p;` should work the same as `struct point p;`. Cleans up code that touches structs and pointer types repeatedly.

Status: **done.** Smoke test:

```
$ cc /typedefs.c -o /td.elf
cc: wrote /td.elf
$ td
add_words(7, 35) = 42
greeting = AdventOS
dot(p, q) = 63
counter = 103
```

The sample uses four kinds of typedef: scalar (`typedef int word`), pointer (`typedef char *string`), struct value (`typedef struct point Pt`), and struct pointer (`typedef struct point *PtP`). Each is exercised in a local declaration, a function parameter, a function return type, and a global.

---

## The typedef registry

```c
struct typedef_entry {
    char name[NAME_MAX];
    int  kind;     /* LK_INT / LK_INT_PTR / LK_CHAR_PTR / LK_STRUCT / LK_STRUCT_PTR */
    int  meta;     /* struct_idx for struct kinds */
};
static struct typedef_entry g_typedefs[MAX_TYPEDEFS];
```

When `parse_typedef_top` parses `typedef BASE NAME;`, it stores the underlying `kind` and `meta` (the struct index, for struct typedefs). At any later type-position, the parser checks this table when it sees a T_NAME.

---

## The hard part: parse "at a type position"

The classic typedef parsing problem in C is that you need context to know whether a name refers to a type. `(x) * y;` is ambiguous: it might be `(int)*y` if x is a typedef name, or `x * y` (multiplication) if x is a regular identifier.

cc dodges this complexity by only checking the typedef table at known type positions. There are four:

1. **Top-level decl** (`parse_program`) — at the start of each top-level item, after struct/enum/typedef keywords don't match.
2. **Local decl** (`parse_stmt` head) — before checking for `int|char|struct`.
3. **Function return type** (`parse_func` head) — before the function name.
4. **Function parameter** (`parse_func` param loop) — before the parameter name.

Each spot checks `typedef_find(tk_cur()->name)` if the current token is `T_NAME`. If it matches, the underlying type info drives the same decl-emit path as `int|char|struct`. If it doesn't match, the parser falls back to expecting `int|char|struct` and errors if neither is present.

The compromise: typedef NAMES occupy the same namespace as variable/function names, but since typedef is only consulted at type positions, there's no ambiguity at parse time. A name can't be both a typedef and a variable in cc.

---

## What's supported

```c
typedef int word;                          /* scalar alias */
typedef char *string;                      /* pointer alias */
typedef struct point Pt;                   /* struct value alias */
typedef struct point *PtP;                 /* struct pointer alias */

word counter;                              /* global with typedef */
word add(word a, word b) { return a+b; }   /* params + return */

int main() {
    word x;                                 /* local with typedef */
    Pt p; p.x = 3; p.y = 4;                /* struct value */
    PtP q; q = &p;                          /* struct pointer */
    string s = banner();                    /* char* alias */
    return 0;
}
```

---

## What's deferred

- **`typedef int arr_t[8];`** — typedef of an array. The trailing `[N]` would belong to the typedef, not the user variable. Not parsed.
- **`typedef int (*fp)(int);`** — function-pointer typedef. The declarator grammar is the C-classic mess; not parsed.
- **`typedef Foo *Bar;`** — typedef whose base is itself a typedef. Two-level resolution would be straightforward to add but isn't.
- **`typedef int *iptr; iptr a, b;`** — multiple variables in one decl. Not parsed (cc has never supported the comma form).
- **`typedef enum { A, B } Foo;`** — enum typedef. Not parsed.

Each is a tweak; none blocks common usage.

---

## Files touched

- `user/cc.c` — `T_TYPEDEF` token + keyword; `g_typedefs[64]` registry; `typedef_find` / `typedef_add`; `try_consume_type` helper; `parse_typedef_top`; typedef-NAME branches in `parse_stmt`, `parse_func` (return + params), `parse_global_decl`, and `parse_program` dispatch. ~120 lines added.
- `fs/typedefs.c` — sample.
- `fs/man/cc` — typedef docs + removed from "not implemented" list.
- `mkfs.py` — added typedefs.c.
- `README.md` — pointer bump.

cc.bin: 287 KiB → 291 KiB.

---

## Phase 3 status after session 104

Eight sub-sessions shipped. Cumulative cc features: structs (+ arrays of), function pointers, sizeof, scaled ptr arith, multi-file, struct value assign, enum, typedef. Plus all of Phases 1 and 2.

Remaining: real variadics (105), struct-by-value calls (106), optimization (107+).
