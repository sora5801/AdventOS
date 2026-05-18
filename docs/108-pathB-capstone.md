# Session 121 — Path B Phase 4 capstone: SBV returns, static/extern, fp typedef

**Goal.** Close out the C-subset language surface in `cc` so Path B (the
self-hosting compiler) is complete enough to compile real
small-to-medium programs without falling back to workarounds. Three
features in one session:

1. **Struct-by-value returns.** `struct point f(...) { struct point p; ...; return p; }`
   — the inverse of session 106's struct-by-value *parameters*. Uses
   the standard cdecl hidden-first-arg ABI: caller pushes `&dest` as
   arg 0, callee writes through it.
2. **`static` / `extern` keywords.** `static int foo(...)` and
   `static int x;` are accepted no-op storage-class modifiers (every
   cc symbol is already TU-private). `extern int squared(int);` is a
   function prototype — registers signature info in `g_funcs` without
   emitting a body so call sites see correct param kinds and return
   type even when the definition lives later (or in another TU).
3. **`typedef RET (*NAME)(ARGS);`** — the canonical C function-pointer
   typedef syntax. Internally aliases to `LK_INT_PTR` (cc's existing
   function-pointer representation since session 98), so a
   `typedef int (*op_t)(int, int);` becomes "an int* alias" — no
   typechecking on the indirect call, but the syntax now parses.

Status: **done.** Smoke test exercises all three in one program — see
`fs/capstone.c` and `smoke_pathB.py`.

```
$ cc /capstone.c -o /cap.elf
cc: wrote /cap.elf
$ /cap.elf
make_point(3,4) = (3, 4)
shift(p,10,20)  = (13, 24)
p untouched     = (3, 4)
shift_x_only    = (103, 4)
s_module_counter = 45
squared(7)       = 49
op=s_add: op(3,5) = 8
op=s_mul: op(3,5) = 15
```

The `p untouched` line is the SBV-return invariant check: `shift` got
a *copy* of `p` (struct-by-value parameter from session 106) and built
a *fresh* struct in its hidden dest slot (Session 121), so the
caller's `p` is unaffected. Walking the same cdecl ABI in both
directions makes struct-passing fully symmetric.

---

## 1. Struct-by-value returns: the hidden-first-arg ABI

When a function is declared to return a struct, cc rewires the cdecl
layout so the callee receives a hidden pointer to the caller's return
slot at `[ebp+8]`, and the real parameters shift up by 4:

```
struct point shift(struct point in, int dx, int dy);

caller's view, immediately after `call shift`, low-to-high:
  ESP ─┬─[ &q  ]      hidden dest pointer (arg 0, pushed LAST so it's at top)
       │ [ in.x ]
       │ [ in.y ]     by-value copy of `in` (8 bytes)
       │ [ dx   ]
       │ [ dy   ]
       │ [ retaddr ]
       │ [ saved ebp ]
  EBP ─┤
       └────────────

callee's view, via [ebp + N]:
  [ebp + 8]  = &q          ← the hidden dest pointer
  [ebp + 12] = in.x        ← start of struct param (sessions 106 cum_off)
  [ebp + 16] = in.y
  [ebp + 20] = dx
  [ebp + 24] = dy
```

The two big pieces this required:

- **`cum_off` starts at 12 instead of 8** for struct-returning
  functions, so the SBV ABI shifts every real param. Session 106 had
  `cum_off = 8`; Session 121 makes it `(fn->ret_kind == LK_STRUCT) ? 12 : 8`.
- **`return STRUCT_LOCAL;` becomes a `rep movsd`** from `[ebp + r_off]`
  (the local) to `[[ebp + 8]]` (the hidden dest), followed by the
  standard epilogue. The function "returns" the dest pointer in EAX
  for consistency — some callers chain on it, others ignore it.

Here's the codegen for `return p;` inside `make_point`:

```
push esi                        ; cdecl callee-saved
push edi
mov  eax, [ebp + 8]             ; eax = hidden dest pointer
mov  edi, eax                   ; edi = dest
lea  eax, [ebp + r_off]         ; eax = &p (the local struct)
mov  esi, eax                   ; esi = src
mov  ecx, dwords                ; ecx = struct_size / 4
rep  movsd                      ; copy struct
pop  edi
pop  esi
mov  eax, [ebp + 8]             ; return the dest pointer
mov  esp, ebp                   ; standard epilogue
pop  ebp
ret
```

### Call sites: `lhs = func(...)` and `return func(...)`

A struct-returning call CAN'T appear as a bare expression — there'd be
nowhere to put the result. cc accepts exactly two shapes:

```c
struct point p = make_point(3, 4);   // not yet — see "restrictions"
struct point p;
p = make_point(3, 4);                // ✓ — assigned to a struct lvalue
...
struct point shift_x_only(struct point in, int dx) {
    return shift(in, dx, 0);         // ✓ — tail-forward into our own dest
}
```

In `gen_stmt(N_ASSIGN)`, when the RHS is a call to a struct-returning
function:

```
push the regular args (right-to-left, via push_call_args helper)
lea  eax, [ebp + off]         ; eax = &lhs (the destination)
push eax                       ; hidden dest pointer as arg 0
call func                      ; rel32 with fixup
add  esp, total_push           ; caller cleanup (cdecl)
```

In `gen_stmt(N_RETURN)`, when the current function is SBV-return AND
the RHS is a call to a same-struct-returning function, we forward OUR
hidden dest pointer to the inner call — no intermediate copy:

```
push the regular args (right-to-left)
mov  eax, [ebp + 8]            ; load OUR caller's dest pointer
push eax                        ; forward it as the inner call's dest
call inner_func
add  esp, total_push
mov  eax, [ebp + 8]             ; return same pointer (tidiness)
; standard epilogue + ret
```

This is what makes `return shift(in, dx, 0);` cheap: no copy from
`shift`'s temporary back into ours. The callee writes directly into
our caller's slot.

### Where this lives in `cc.c`

- `parse_return_type()` (new helper) — recognizes `struct T [*]` as a
  return type, sets `fn->ret_kind` + `fn->ret_meta`.
- `parse_param_list()` (refactored out of `parse_func`) — now also
  serves `parse_extern_proto`. Also accepts unnamed params (so
  `extern int squared(int);` parses).
- `parse_program()` — dispatches `struct T NAME(...) { ... }` to
  `parse_func` instead of `parse_struct_top`.
- `func_info.ret_kind / ret_meta` — symbol-table fields propagated by
  the pre-populate pass + `parse_extern_proto`.
- `push_call_args()` (new helper, factored out of `gen_call`) — used
  by `gen_call`, the SBV-call path in `N_ASSIGN`, and the
  tail-forward path in `N_RETURN`.
- `gen_func()` — sets `g_cur_ret_kind / meta`, shifts `cum_off`.
- `gen_stmt(N_RETURN)` — SBV-return memcpy + tail-forward branches.
- `gen_stmt(N_ASSIGN)` — SBV-call branch.

### Restrictions (documented limits)

- **No `struct T x = func(...);` initializer syntax** at declaration
  time. Write `struct T x; x = func(...);` instead. Adding the
  initializer is a small parser-side change (decl + assign in a
  synthetic block) but wasn't required for the smoke test.
- **No SBV calls as sub-expressions.** `f(g(...))` where `g` returns
  a struct doesn't work — only assignment and return contexts are
  supported. The current diagnostic is
  `struct-returning call must be assigned to a struct lvalue`.
- **No struct-by-value RETURN from a global as source.** The
  `rep movsd` source uses `lea [ebp + r_off]` only — extending it to
  load a global address is a one-liner but globals-as-struct-source
  is uncommon enough that we bailed.

---

## 2. `static` and `extern`

### `static`

cc compiles one translation unit at a time. Every function and global
is already private to that TU — there's no separate `.o` file system,
no `extern`-linkage cross-TU resolution. So `static` is semantically
vacuous in cc. We accept the keyword and skip it.

Why bother? Because source files written against real C frequently
use `static` to mean "module-local helper, please warn if I accidentally
make this externally visible." cc has no warnings to suppress, but
the source still has to parse. Before Session 121, `static int foo(...)`
would die with "expected 'int', 'char', 'struct', 'enum', 'typedef'…".
Now it parses, the keyword is dropped, and the declaration proceeds.

Implementation: one token (`T_STATIC`), one line in
`parse_program()`:

```c
if (tk_cur()->kind == T_STATIC) g_tk++;   /* swallow + fall through */
```

### `extern`

`extern RET NAME(PARAMS);` is a function prototype: a forward
declaration without a body. The implementation is in
`parse_extern_proto()`, which uses the same `parse_return_type` and
`parse_param_list` helpers as `parse_func`, but expects `;` instead
of `{`.

What it actually does: registers the function in `g_funcs` with the
full signature (n_params, is_variadic, param kinds/metas, ret kind/meta)
via `register_func_proto()`. No AST node is created — `gen_func`
would try to emit a body. By the time the real definition appears
later in source (or in a second `.c` file), `func_intern` finds the
existing entry by name and the definition's `gen_func` overrides
`entry_off` / `defined` / etc. The signature was already correct.

Why Session 121 introduced a helper function (`register_func_proto`)
and a forward decl at the top of the file: the parser lives in a
section above the symbol table (where `g_funcs` is defined), so the
parser can't touch the symbol table directly. The helper crosses
that boundary cleanly without requiring a wholesale reorganization
of the file.

The cdecl-orthogonal pretty bit: `extern struct point make_point(int, int);`
is also valid. The prototype carries SBV-return info, so a caller
*above* the definition sees the SBV ABI and pushes a hidden dest
pointer.

---

## 3. Function-pointer typedef syntax

Old way (session 98):

```c
int *fp;                    /* an int*, but actually holds a function VA */
fp = my_func;               /* bare name decays to function VA */
fp(args);                   /* indirect call via the variable */
```

New way (Session 121):

```c
typedef int (*op_t)(int, int);
op_t op;
op = my_func;
op(a, b);
```

Internally, `op_t` aliases to `LK_INT_PTR` — same shape as the bare
`int *fp`. cc doesn't typecheck indirect calls, so the parameter list
in the typedef is parsed for syntax but the kinds aren't recorded.
Source files written for real C parse without modification; the
runtime semantics are identical to the session-98 implementation.

The detection rule in `parse_typedef_top`: after consuming the base
return type (via `try_consume_type`), peek for the
`( * NAME ) (` token sequence. If matched, consume up to the
balanced `)` of the arg list, then `;`, and register `NAME` as a
`LK_INT_PTR` typedef:

```c
if (tk_cur()->kind == T_LPAREN
    && tk_peek(1)->kind == T_STAR
    && tk_peek(2)->kind == T_NAME
    && tk_peek(3)->kind == T_RPAREN
    && tk_peek(4)->kind == T_LPAREN) {
    /* ...consume `( * NAME ) ( args ) ;`... */
    typedef_add(fp_name, LK_INT_PTR, 0);
    return;
}
```

---

## Two collateral fixes

### CRLF in `/inittab` breaks every exec

While bringing up the smoke test, every user binary failed to exec
with `init: exec failed: <name>`. The root cause turned out to be
nothing to do with my cc changes — Git's `autocrlf` on Windows had
converted `fs/inittab` to CRLF on checkout. `init.c::tokenize`
splits on space/tab and treats `\r` as a normal character, so
`once httpd.elf\r\n` produced `argv[0] = "httpd.elf\r"` (10 bytes,
including the trailing CR). `fs_open` then walked the FS looking for
an entry literally named `httpd.elf\r`, found nothing, and returned -1.

The fix: treat `\r` as whitespace in init's tokenizer (one line of
code). Without this, the smoke test couldn't even reach the shell
prompt.

(The session was originally developed against a fork where
`FS_MAX_FILES` was still 128; that build hit a slot-exhaustion bug
when `cc /capstone.c -o /cap.elf` collided with sshd's host-key
write and three runtime `mkdir` calls. The change merges onto an
origin where session 112's FS bump to 160 already absorbed that
crunch, so the smoke test runs cleanly without trimming demo files
or pre-creating `/var`.)

---

## What "Path B complete" means

Phase 4 was the last set of remaining items the README enumerated:

- ✅ struct-by-value returns (this session)
- ✅ `static` / `extern` (this session)
- ✅ function-pointer typedef syntax (this session)
- ✅ already shipped: structs, fp, sizeof+scaled-ptr, multi-file,
  struct value-assign, array-of-struct, enum, typedef, variadics,
  SBV calls

That covers essentially every C language feature small-to-medium
programs use. Remaining items are optimizations (constant folding,
peephole, a register allocator) and a few specialized corners
(`union`, `goto`, `switch`, multi-dimensional arrays, function-like
macros). None of these block "compile a real program" anymore —
they're polish.

After Session 121, cc.bin is 302 KiB (vs 298 KiB before — +4 KiB for
the three features and their helpers). `cc.c` is ~4630 lines. The
compiler is comfortably self-aware enough that any further work can
be incremental.

---

## Files touched

- `user/cc.c` — most of the session. `~280 lines` added: SBV-return
  parser + codegen (`parse_return_type`, `parse_param_list`
  refactor, `parse_extern_proto`, `register_func_proto`, `g_cur_ret_*`,
  `push_call_args` helper, `N_RETURN` + `N_ASSIGN` SBV branches);
  static/extern keywords; function-pointer typedef syntax.
- `user/init.c` — CRLF-in-inittab fix in `tokenize`.
- `fs/capstone.c` — new smoke test exercising all three features.
- `fs/man/cc` — documentation for SBV returns, static/extern, fp
  typedef syntax.
- `smoke_pathB.py` — boots QEMU, runs `cc /capstone.c -o /cap.elf`
  then `/cap.elf`, verifies 8 expected output lines.
- `README.md` — Path B marked complete; Session 121 pointer.
