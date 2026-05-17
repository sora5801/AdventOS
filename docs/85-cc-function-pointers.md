# Session 98 — cc Phase 3 part 2: function pointers

**Goal.** Let cc programs store function addresses in variables and call through them. With this, dispatch tables, callbacks, and the standard "table of function pointers" pattern work.

Status: **done.** First-try smoke test:

```
$ cc /fnptr.c -o /fnptr.elf
cc: wrote /fnptr.elf
$ fnptr
square(7) = 49
cube(3)   = 27
neg(11)   = -11
  f(21) = 42
  f(6) = 36
table[0](5) = 25
table[1](5) = 125
table[2](5) = -5
table[3](5) = 10
```

All nine cases pass. Regression test on `structs.elf` confirms session 97's linked-list walker still works.

---

## The minimal subset

Real C has a baroque function-pointer type syntax — `int (*fp)(int, char*)`, with the parens around `*fp` to bind the pointer-ness to `fp`. Parsing that requires nontrivial lookahead in the declarator grammar.

cc skips that entirely. Function pointers are just `int *` variables — cc doesn't enforce the distinction between an int pointer and a function pointer at all. Both store a 32-bit value (an address). Calls dispatch based on the syntax, not the type.

```c
int *fp;            /* fp is just an int* — also serves as a function ptr */
fp = my_func;       /* function name decays to its address */
fp(args);           /* indirect call through fp */
```

The price of this simplification: cc can't typecheck that `fp` is actually pointing at a function with the right arity. It'll just `call eax` and hope. In practice the same is true of real C when you cast — type-checking only catches what's expressible in the type system.

---

## Three mechanisms

### 1. Function name as rvalue

`fp = my_func;` — the parser sees `my_func` as an `N_NAME` in expression context. Codegen flow:

1. `local_find(name)` — fails (not a local).
2. `global_find(name)` — fails (not a global).
3. `func_find(name)` — hits. Or, if the function is forward-declared, `func_intern(name, -1)` creates a placeholder with sentinel arg count.
4. Emit `mov eax, imm32` (`B8 ?? ?? ?? ??`); record an **address fixup** linking the imm-offset to the function index.

After all codegen + the existing fixups, a final pass walks `g_addr_fixups[]` and writes `ENTRY_VA + g_funcs[i].entry_off` into each placeholder. If the function was referenced by address but never defined, that pass dies.

Same treatment for `&my_func` — explicit address-of also lands on this path. They produce identical machine code.

### 2. Indirect call via variable

`fp(args);` — the parser sees this as an `N_CALL` with name `fp`. Codegen:

1. Is `fp` an intrinsic? No.
2. Push args right-to-left (cdecl).
3. Look up `fp` in locals + globals. **If found**: load the variable's value into eax, then `call eax` (`FF D0`).
4. **If not found**: fall through to the original behavior — assume it's a function name and emit `call rel32` with a function fixup.

This unifies the two call shapes — direct calls (`my_func(...)`) and indirect calls (`fp(...)`) — under a single AST node. The only difference is in the codegen branch, which falls out of whether the name resolves as a variable.

### 3. The `-1` sentinel in `func_intern`

`func_intern(name, n_params)` previously enforced that all references to a function agreed on arg count. The address-of-function case has no idea about arg count, so it interns with `n_params = -1`. The function-table entry stays as `-1` until the first real call or definition fills it in.

```c
static int func_intern(const char *name, int n_params) {
    int i = func_find(name);
    if (i >= 0) {
        if (g_funcs[i].n_params == -1) {
            g_funcs[i].n_params = n_params;   /* fill in */
            return i;
        }
        if (n_params >= 0 && g_funcs[i].n_params != n_params)
            die_at(0, "arg-count mismatch for", name);
        return i;
    }
    /* ...new entry, n_params can be -1... */
}
```

A function referenced ONLY by address (never called directly, never defined) keeps `n_params = -1` forever — at the end of compilation, the address-fixup pass catches "address-of undefined function" because `entry_off` is still `-1`.

---

## What this unlocks

Beyond the obvious `qsort`-style callbacks, function pointers are useful for:

- **Dispatch tables.** Stored as `int table[N]` (not `int *table[N]` — see "Why dispatch tables use int[]" below).
- **State machines.** Each state is a function; the current state is an `int *fp`.
- **Plugin-ish patterns.** A registry of named operations, with the function pointer looked up by string at runtime.
- **Inline iteration with a per-iter callback.** `apply_print(func, arg)` in the demo.

```c
int square(int n) { return n * n; }
int *fp = square;
printf("%d\n", fp(7));     /* 49 */
```

### Why dispatch tables use `int[]`, not `int*[]`

The cc parser doesn't yet support `int *name[N];` (an array-of-pointers global). The workaround: declare the table as `int table[N];` and store function VAs as ints. Since cc doesn't distinguish int from int*, the values flow through fine:

```c
int table[4];
table[0] = square;    /* RHS is a function VA; an int. */
int *f;
f = table[i];          /* int → int*; just a copy. */
f(args);               /* indirect call through f. */
```

The intermediate `f` step is necessary because cc's call form requires the base to be a NAME — `table[i](5)` would need the parser to recognize a call after an index expression. Adding that to `parse_primary` is straightforward but wasn't part of this session.

---

## The fixup order

Sessions 90–98 have built up four kinds of fixups, all resolved at end of codegen in a specific order:

1. **`g_fixups`** — `call rel32` displacements. Resolved by `resolve_fixups()` once every function has its `entry_off`.
2. **`g_addr_fixups`** — `mov eax, imm32` filled with `ENTRY_VA + func.entry_off`. **NEW in session 98.** Resolved right after `resolve_fixups()`.
3. **`g_str_fixups`** — `mov eax, imm32` filled with `string_pool_base_va + offset`. Resolved after appending the string pool to the code segment.
4. **`g_glob_fixups`** — `mov eax, imm32` or `mov eax, [imm32]` filled with `data_pool_base_va + offset`. Resolved after appending the global data pool.

The order matters: function offsets must be locked before address fixups; both must be locked before pool placements; pool placements set the segment-end VA.

---

## Restrictions (documented)

- Function-pointer type syntax `int (*fp)(int)` isn't parsed. Use `int *fp`.
- Calling a function pointer directly off an expression (`arr[i](5)`, `f()()`) isn't supported. Load into a local first.
- Array-of-pointers globals (`int *table[N]`) aren't parsed. Use `int table[N]` and treat values as addresses.
- No type-checking on argument lists for indirect calls. Pass the right number / shape of args yourself.

Each restriction is a small parser fix away. None blocks the common use cases.

---

## Files touched

- `user/cc.c` — `g_addr_fixups[128]` + `record_addr_fixup`; `func_intern` accepts `-1` sentinel; gen_call falls through to indirect-call when the name resolves as a variable; N_NAME and N_ADDR_OF fall through to function lookup; address-fixup resolution in `main()`. ~80 lines added.
- `fs/fnptr.c` — sample with named functions, callback args, and a global dispatch table.
- `fs/man/cc` — function-pointer docs.
- `mkfs.py` — added `fnptr.c`.
- README.md — pointer bump.

cc.bin: 276 KiB → 277 KiB. Tiny growth — `g_addr_fixups[128]` is ~1 KiB.

---

## Phase 3 status after session 98

Two of N+ shipped:

- ✅ 97 — structs (member access, struct pointers, linked lists)
- ✅ 98 — function pointers (indirect calls, dispatch tables)
- ⏳ 99 — scaled pointer arithmetic (`int* + 1` advances 4 bytes)
- ⏳ 100 — multi-file compilation (linker)
- ⏳ 101+ — sizeof, struct value assign, char fields, enum/typedef, …

cc has now covered most C idioms that don't require type-system depth. The remaining gaps are bigger features that compose in non-trivial ways — multi-file compilation in particular is its own substantial project (intermediate object format, relocation table, link step).
