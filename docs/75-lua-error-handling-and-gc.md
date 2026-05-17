# Session 88 — Path D continues: error handling, closures, GC

**Goal.** Session 87 shipped a working Lua-syntax interpreter but called out five deferred features as "next session." This session closes four of them — leaving multi-return values (the one that requires the most invasive refactor) as the remaining piece.

Status: **done.**

| Feature | Status |
|---|---|
| `pcall` + `error` (REPL error recovery) | ✅ |
| Closures with upvalues (capture-by-value) | ✅ |
| `string.find` / `string.byte` / `string.char` | ✅ |
| Mark-sweep GC | ✅ |
| Multi-return values + generic `for k,v in pairs(t)` | ❌ (deferred to session 89) |

Net diff: ~340 lines added to `user/lua.c`. Build size 27.7 KB → 31.1 KB (+12%). Updated `fs/hello.lua` to exercise every new feature; updated the man page.

---

## Part 1 — `pcall` + `error` (REPL error recovery)

### The problem

Session 87's REPL was single-process: any runtime error called `die()` which called `sys_exit(1)`. Type one bad statement at the prompt, the whole interpreter exits. Not viable for interactive use.

### The fix: error frames + `__builtin_setjmp`

A global linked list of `struct err_frame`:

```c
#define ERR_MSG_MAX 240
struct err_frame {
    void              *jmp[5];
    char               msg[ERR_MSG_MAX];
    int                msg_len;
    struct err_frame  *prev;
};
static struct err_frame *g_err_stack = 0;
```

`__builtin_setjmp` on i386 saves EBP, EBX, ESI, EDI, ESP, and the return address into a 5-pointer buffer. Compiles inline; no libgcc dependency. (`setjmp.h`'s `setjmp` would also save the signal mask, which we don't have anyway, AND requires linking against libc-style support that AdventOS doesn't ship.)

The new `die()`:

```c
static void die(const char *fmt, ...) {
    char buf[ERR_MSG_MAX];
    /* format the message via va_args ... */

    if (g_err_stack) {
        /* Pack the message into the topmost frame and longjmp. */
        struct err_frame *f = g_err_stack;
        /* copy buf into f->msg */
        __builtin_longjmp(f->jmp, 1);
    }
    /* No pcall on the stack — fatal. */
    sys_write(2, "lua: ", 5);
    sys_write(2, buf, n);
    sys_exit(1);
}
```

### Two entry points for protection

**The REPL** wraps every line:

```c
struct err_frame frame;
frame.prev = g_err_stack;
g_err_stack = &frame;
if (__builtin_setjmp(frame.jmp) == 0) {
    run_source(line, n);
} else {
    sys_write(2, "lua: ", 5);
    sys_write(2, frame.msg, frame.msg_len);
    sys_write(2, "\n", 1);
}
g_err_stack = frame.prev;
```

A bad statement prints the error and returns to the prompt instead of exiting.

**`pcall(f, args...)`** does the same dance around a function call:

```c
static struct value bi_pcall(int argc, struct value *argv) {
    struct err_frame frame;
    frame.prev = g_err_stack;
    g_err_stack = &frame;
    if (__builtin_setjmp(frame.jmp) == 0) {
        /* call the function with args ... */
        g_err_stack = frame.prev;
        return result;
    }
    /* Error path. */
    g_err_stack = frame.prev;
    g_last_error = make_string(frame.msg, frame.msg_len);
    return v_bool(0);
}
```

Real Lua's `pcall` returns `(true, value)` on success or `(false, errmsg)` on failure — multi-return values. This subset doesn't have multi-return yet, so the compromise is: `pcall` returns the result on success or `false` on failure, and the message can be read via `last_error()`. Documented; session 89 cleans this up when multi-return lands.

### `error(msg)`

```c
static struct value bi_error(int argc, struct value *argv) {
    if (argc < 1) die("error called");
    struct string *s = to_str(argv[0]);
    die("%s", s->data);
    return v_nil();     /* unreachable */
}
```

`error(...)` becomes a regular call into `die(...)` which longjumps to the topmost `pcall` (or exits if there is none). Works correctly because `die` is the only place that touches `g_err_stack`.

### Known limit

Heap allocations made between the `setjmp` and a `longjmp` *leak* — the values are still in the tracking lists, but if no root references them after the longjmp they'll be reclaimed by the next mark-sweep pass (see Part 4). So in practice, `pcall`-caught errors leak briefly until the next collection.

---

## Part 2 — Closures with upvalues (capture-by-value)

### The problem

Pre-session-88, a nested function could only see globals + its own locals. The Lua factory pattern didn't work:

```lua
local function make_greeter(name)
    return function() return "hello " .. name end
end
local g = make_greeter("world")
print(g())     -- ERROR: 'name' was a local of make_greeter, not visible
```

### Two options

Real Lua does **capture-by-reference**: every captured local lives in a heap-allocated "upvalue box" that's shared between the enclosing function's local slot and the inner function's closure. A write to the outer local is visible from inside the inner function — and conversely, a write from inside the inner is visible from outside.

This is the right behavior but requires ~200 lines of plumbing: upvalue boxes, the parser identifies which locals are captured, every local-slot read/write goes through the (possibly-boxed) indirection.

The cheaper alternative is **capture-by-value**: at the moment the inner function is created, snapshot every visible outer local into the function's own array. Writes from inside the inner go to its own locals, not back to the outer's box. Writes from outside aren't visible to the inner. This is "Lua 4-style," and it covers every closure idiom where the captured outer locals don't mutate after the inner function is created — which is the common factory-pattern case.

This session uses capture-by-value. Documented difference from real Lua. Session 89 or later can upgrade to capture-by-reference if the factory-pattern-with-mutation case starts mattering.

### Implementation

`struct func` gains an upvalue array:

```c
struct func {
    ...
    struct string **upval_names;
    struct value   *upvals;
    int             n_upvals;
};
```

`N_FUNC` eval snapshots the enclosing env's locals:

```c
case N_FUNC: {
    struct func *f = malloc(sizeof(*f));
    ...
    f->n_upvals = e->n_locals;
    f->upval_names = malloc(sizeof(struct string *) * f->n_upvals);
    f->upvals      = malloc(sizeof(struct value) * f->n_upvals);
    for (int i = 0; i < f->n_upvals; i++) {
        f->upval_names[i] = e->locals[i].name;
        f->upvals[i]      = e->locals[i].value;
    }
    return v_fn(f);
}
```

`struct env` gains a `host_fn` pointer set by `eval_call` to the function whose body the env is running:

```c
struct env {
    ...
    struct func      *host_fn;
};
```

`var_get` consults the host_fn's upvals after locals miss but before falling through to globals:

```c
static struct value var_get(struct env *e, struct string *name) {
    int i = local_find(e, name);
    if (i >= 0) return e->locals[i].value;
    if (e->host_fn) {
        for (int j = 0; j < e->host_fn->n_upvals; j++) {
            struct string *un = e->host_fn->upval_names[j];
            if (un->len != name->len) continue;
            /* byte-compare names ... */
            if (eq) return e->host_fn->upvals[j];
        }
    }
    return table_get(e->globals, v_strz(name->data));
}
```

`var_set` deliberately does NOT write through to upvalues. A write to a name shadowing an upvalue creates/updates a global (matching Lua's "implicit global" assignment) instead of mutating the closure. That's the capture-by-value behavior.

### Result

The factory pattern works:

```lua
local function make_greeter(name)
    return function() return "hello " .. name end
end
local g1 = make_greeter("world")
local g2 = make_greeter("AdventOS")
print(g1())     -- hello world
print(g2())     -- hello AdventOS
```

A stateful closure-counter doesn't quite work because writes to the captured local don't update the outer:

```lua
local function counter()
    local n = 0
    return function() n = n + 1; return n end   -- writes to global n, not outer
end
```

Documented. Real Lua needs the by-reference upgrade. Filed.

---

## Part 3 — `string.find` / `string.byte` / `string.char`

Three small built-ins that fill an obvious gap. `string.find` does plain substring search:

```c
static struct value bi_string_find(int argc, struct value *argv) {
    if (argc < 2 || argv[0].kind != V_STR || argv[1].kind != V_STR) return v_nil();
    struct string *s = argv[0].as.s;
    struct string *p = argv[1].as.s;
    int start = (argc >= 3) ? to_num(argv[2], "string.find init") : 1;
    /* ... naive O(n*m) substring search ... */
}
```

Lua's real `string.find` interprets the second argument as a pattern (with `%a` `%d` `[set]` `+` `?` `*` etc.) unless the fourth argument is `true`. This subset only does plain substring. Pattern support is a separate feature with its own ~300 LOC; deferred.

`string.byte(s, [i])` reads a single byte value. `string.char(b1, b2, ...)` builds a string from byte values. Useful primitives for low-level string work without patterns.

---

## Part 4 — Mark-sweep GC

### The problem

Session 87 leaked every allocation. Long-running REPL sessions accumulated memory. Not catastrophic — the user-heap is large — but unacceptable for "real scripting language."

### Design

Three tracking lists, one per heap-allocated value kind:

```c
struct string { struct string *gc_next; int gc_mark; int len; char data[1]; };
struct table  { struct table  *gc_next; int gc_mark; int count; int cap; struct tk_pair *kv; };
struct func   { struct func   *gc_next; int gc_mark; ... };

static struct string *g_all_strings = 0;
static struct table  *g_all_tables  = 0;
static struct func   *g_all_funcs   = 0;
```

Each allocator function (`make_string`, `table_new`, the `N_FUNC` eval case) pushes the new object onto its list.

### Roots

Reachability is computed from:
- `g_globals` (the global namespace table)
- Every env in `g_env_top → ... → NULL` (the active call chain)
- `g_last_error` (the most recent pcall error message)

`struct env` got a `prev` field linking to its caller's env, and `make_env`/`free_env` maintain the chain.

### Mark phase

Recursive: `mark_value` → `mark_table` (recurse on every key+value) or `mark_func` (recurse on every upvalue) → `mark_value` again. Strings are leaves — just set `gc_mark = 1`.

```c
static void mark_table(struct table *t) {
    if (!t || t->gc_mark) return;
    t->gc_mark = 1;
    for (int i = 0; i < t->count; i++) {
        mark_value(t->kv[i].k);
        mark_value(t->kv[i].v);
    }
}
```

The `if (gc_mark) return` guard handles cycles — a table referencing itself doesn't loop forever.

### Sweep phase

Walk each tracking list. For each entry: if marked, clear the mark and continue. If unmarked, unlink and free.

```c
struct string **sp = &g_all_strings;
while (*sp) {
    if ((*sp)->gc_mark) { (*sp)->gc_mark = 0; sp = &(*sp)->gc_next; }
    else { struct string *dead = *sp; *sp = dead->gc_next; free(dead); }
}
```

Tables also free their `kv` array on death; funcs free `upvals` and `upval_names`.

### When to collect

Counter + geometric threshold:

```c
static int g_alloc_count = 0;
static int g_gc_threshold = 256;

static void gc_maybe_collect(void) {
    if (g_alloc_count >= g_gc_threshold) gc_collect();
}

/* In gc_collect(), after sweep: */
g_alloc_count = 0;
g_gc_threshold *= 2;
if (g_gc_threshold > 8192) g_gc_threshold = 8192;
```

Threshold doubles after each collection up to a cap of 8192. So early in a program, GC runs frequently (every 256 allocs); once steady-state is reached, runs every 8192. The cap prevents pathologically long runs.

`gc_maybe_collect()` is called between statements in `eval_block`. That's the safe point — no temp `struct value`s are live on the C stack, so no surprise frees.

### Manual trigger

`collectgarbage()` is wired as a global; force a collection from Lua code.

### What gets collected

Everything in the three tracking lists EXCEPT what's reachable from the roots. That includes the AST? No — the AST is allocated outside the tracker (the parser uses raw `malloc`); it stays for the lifetime of the program. Built-in function pointers (`V_BUILTIN`) aren't tracked either — they're code addresses, not heap-allocated.

The kv arrays inside tables are freed as part of sweeping their owning table. Upval arrays inside funcs the same.

### Validation

The updated `hello.lua` includes a stress loop:

```lua
for i = 1, 500 do
    local junk = { "a", "b", "c", "d" }
    junk[5] = string.rep("x", 16)
end
collectgarbage()
print("gc survived 500 alloc cycles")
```

500 iterations × ~5 allocations each = 2500 allocations. With the geometric threshold starting at 256, the GC runs ~5 times during this loop. Survival means no use-after-free, no double-free, and the program reaches the print.

---

## What's still deferred — session 89

**Multi-return values + generic-for.** The piece that didn't fit this session. It's a different shape of change — touches every place that returns a value (eval_expr signature widens to `struct values { int n; struct value v[N]; }`), the parser for `return`, `local`, and `=` assignment (now LHS-list and RHS-list), and a whole new `for k, v in iter(t) do` loop body. With it, `pairs(t)` and `ipairs(t)` would Just Work the way Lua users expect, and `pcall` could return `(false, errmsg)` instead of the current `false`-plus-`last_error()` workaround.

Estimated effort: ~250-400 lines of changes. Worth its own session for the testing.

After session 89, Path D is effectively complete:
- ✅ pcall + error
- ✅ Closures (capture-by-value)
- ✅ string.find / byte / char
- ✅ GC
- 🚧 Multi-return + generic-for (session 89)

The features that would still be "missing" from real Lua at that point: metatables, coroutines, capture-by-reference closures, full string patterns, math library. Those are each multi-session projects in their own right and most aren't on the daily-scripting critical path.

---

## Files touched

```
user/lua.c                      +340 lines:
                                  - struct err_frame + g_err_stack
                                  - reworked die() with __builtin_longjmp
                                  - bi_pcall, bi_error, bi_last_error
                                  - bi_string_find, bi_string_byte, bi_string_char
                                  - bi_collectgarbage
                                  - struct func: upval_names, upvals, n_upvals
                                  - N_FUNC eval captures upvals
                                  - struct env: host_fn, prev
                                  - var_get checks host_fn->upvals
                                  - mark/sweep + tracking lists in
                                    struct string/table/func
                                  - gc_maybe_collect() in eval_block
                                  - REPL: setjmp wrap around run_source
fs/hello.lua                    Updated to exercise every new feature
fs/man/lua                      IMPLEMENTED / NOT IMPLEMENTED rewritten
docs/75-lua-error-handling-and-gc.md   NEW — this file
README.md                       latest-session pointer
```

Build size: `lua.bin` 27724 → 31148 bytes (+3424, +12%). Kernel image unchanged. No new syscalls.

---

## Smoke test

Boot and run:

```sh
advent$ lua hello.lua
```

Expected output covers every section labeled in the script. If `gc survived 500 alloc cycles` prints at the end, the GC is reachable-mark/sweep correct. If `pcall caught: false msg: intentional failure` shows, error recovery works. If the two greeters print correctly, closure capture works.

For interactive use:

```sh
advent$ lua
> error("oops")
lua: oops
> print(2+2)
4
> pcall(function() error("nope") end)
false
> last_error()
nope
>
```

The REPL keeps running after the first error — the entire point of this session.
