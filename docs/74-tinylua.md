# Session 87 — Path D begins: a Lua-syntax interpreter from scratch

**Goal.** AdventOS gets a scripting language at the shell prompt. Path A (sessions 83-86) finished the daily-use Unix surface; Path D adds the layer above — something you can use to compose tasks too complex for shell pipelines but too small to write a C binary for.

Status: **done.** `lua` is in the userland — a single-file `user/lua.c` tree-walking interpreter for a useful subset of Lua syntax. ~1100 lines, ~28 KB compiled, runs scripts and has a REPL.

This is **not** a port of upstream Lua. It's a fresh implementation that recognizes the same surface syntax. See "Why subset, not port" below.

---

## Why subset, not port

Real Lua is ~14 KLOC of high-quality portable C, and the upstream sources are deliberately designed to be embeddable in plain-C hosts. So the natural plan is: vendor the sources, write a small porting layer, ship. Why isn't this session that?

Three blockers:

**1. AdventOS user programs build without an FPU.** `build.sh` adds `-mgeneral-regs-only -mno-mmx -mno-sse` to `USER_CFLAGS` because the kernel doesn't save FPU state across IRQ tails. Lua numbers are doubles. Without FPU support, GCC emits soft-float calls — `__adddf3`, `__muldf3`, `__divdf3`, `__floatsidf`, etc. — which live in libgcc, which AdventOS doesn't link.

Confirming the diagnosis: an earlier attempt at this session compiled a draft of `user/lua.c` with `double` numbers and got `undefined reference to '__adddf3'` 30+ times at link. The two fixes — make the kernel save FPU state across context switches (big work for an unclear daily-use benefit), or ship libgcc soft-float (hundreds of KB of code and a runtime ABI commitment) — both vastly exceed the scope of "give the OS a scripting language."

**2. `setjmp`/`longjmp` aren't in the libuser surface.** Lua leans heavily on these for `pcall` and protected error recovery. Implementing them on i386 isn't hard (~30 lines of inline asm save/restore), but every place Lua uses them assumes the surrounding ABI conventions and stack layout — getting that right against a libc we don't fully control is a session's worth of debugging by itself.

**3. The libc surface Lua expects is large.** stdio.h (FILE*, fprintf, fopen/fclose, fread/fwrite, fgetc, ungetc), stdlib.h (atof/strtod, abort), string.h beyond what we ship, math.h (sin/cos/sqrt/pow), errno, locale, ctype, time. AdventOS's libuser covers maybe a third of this. Writing the rest as wrappers is multi-session work even before you get Lua to *compile*.

The lower-friction path: **write a fresh interpreter that recognizes the Lua surface syntax for the common cases**. No FPU dependency, no setjmp dependency, no libc surface beyond what AdventOS already has. The result is ~1100 lines, builds in seconds, and runs the 80% of scripts a system administrator actually writes.

---

## What's implemented

| Layer | Coverage |
|---|---|
| Types | nil, boolean, number (int32), string, table, function |
| Literals | numbers, "double-" and 'single-' quoted strings with `\n \t \\ \" \'` escapes, `true`, `false`, `nil` |
| Operators | binops `+ - * / % ^ == ~= < > <= >= .. and or`, unops `not - #` |
| Statements | assignment, local (with optional init), `if` / `elseif` / `else`, `while`, numeric `for`, function declaration, function literal expr, return, break, expression-as-statement |
| Tables | constructors `{1, 2, 3}` and `{x=1, y=2}` and `{[k]=v}`; index via `t[k]` and `t.k`; assignment to indexed slots; `#t` length operator |
| Functions | top-level (`function name() ... end`), local (`local function name() ... end`), anonymous (`function() ... end`), dotted decl (`function a.b() ... end`), recursion |
| Control flow | `break`, `return [expr]`, short-circuit `and` / `or` |
| Built-ins | `print`, `type`, `tostring`, `tonumber`, `ipairs` (limited — single-return so the `for k,v in ipairs(t) do` form doesn't work, but explicit-iter does), `string.len/sub/upper/lower/rep`, `table.insert/concat`, `io.read/write`, `os.time/exit` |
| REPL | yes — one statement at a time, single-process so errors abort |
| File mode | `lua FILE.lua` |
| `-e` flag | `lua -e "STMT"` |

A representative session:

```
advent$ lua hello.lua
hello from advent lua
sum 1..10 =     55
10! =   3628800
table length:   4
  1: apple
  2: banana
  3: cherry
  4: date
joined: apple, banana, cherry, date
ADVENTOS!
length: 8
first 6: Advent
negative
zero
small
large
```

(That's the output of `fs/hello.lua`, shipped in the image.)

---

## What's NOT implemented (deferred)

| Feature | Why |
|---|---|
| Metatables / `__index` / `__newindex` etc. | Recursive metamethod dispatch is a non-trivial addition; the daily-use scripts in a system OS rarely need it. |
| Coroutines | Stack switching. Useful for cooperative concurrency, but the OS itself has real preemptive multitasking — that's the recommended path. |
| `pcall` / `xpcall` / `error` | Needs `setjmp`/`longjmp` or a manual error-unwind protocol. Runtime errors currently abort the program; the REPL doesn't recover. |
| Multiple return values | The whole eval pipeline assumes scalar return values; adding tuples touches every assignment + call site. |
| Closures with upvalues | A nested function in the AST has access to globals and to its own locals/params — but cannot capture an outer function's locals. Adding that requires upvalue resolution + a closure capture data structure. |
| Generic `for` (`for k,v in pairs(t)`) | Requires multi-return. Numeric `for i=a,b,c` works. |
| `require` / packages | Single-file scripting only; no module system. |
| String patterns / `gmatch` | `string.find` itself isn't shipped — only `len/sub/upper/lower/rep`. Patterns would need an ad-hoc regex-like matcher. |
| `math.*` (sin, cos, sqrt, pow, etc.) | Trig and transcendentals need floats. See the "Why subset" section. |
| Floating-point numbers | Same FPU reason. `1.5` is a parse error; `1/3 == 0`. |
| Garbage collection | Allocations leak. Programs are short; cleanup happens at process exit. A mark-sweep pass could be added (~150 lines) but isn't urgent. |
| Multi-assignment (`a, b = c, d`) | Eval pipeline limitation as above. |

The full Lua reference manual is 100+ pages; this subset fits in a single source file because every "non-essential" feature is gone.

---

## Implementation tour

Everything is in `user/lua.c`. ~1100 lines, no other files touched.

### Lexer (`lex_all`)

Scans the source byte-by-byte, producing a flat array of `struct tok`. Tokens are: numbers, strings (with `\n \t \\ \" \'` escapes), identifiers (vs keywords — looked up via a small `if`-chain), operators (one or two chars: `=`, `==`, `<`, `<=`, `..`, etc.), punctuation. Line numbers preserved for error messages.

Comments are `--` to end of line. Long strings (`[[ ]]`) and long comments (`--[[ ]]`) aren't supported.

### Parser (`parse_block` and friends)

Recursive descent. The interesting piece is `parse_expr_prec(min_prec)`, which does precedence climbing for binary operators:

```c
int p = binop_prec(t);
if (p == 0 || p < min_prec) break;
tk_advance();
int next_min = binop_right_assoc(t) ? p : p + 1;
struct node *right = parse_expr_prec(next_min);
```

This handles `1 + 2 * 3` correctly (`*` binds tighter than `+`), `a == b and c` correctly (`and` is lower than `==`), and `a .. b .. c` as right-associative.

Suffixes (`. .x`, `[expr]`, `(args)`) are parsed in a separate loop after the primary expression, which makes `f().b[1]` parse as a chain on the same node. Table constructors handle three forms: `{a, b, c}` (auto-indexed), `{x = 1}` (named field, sugar for `["x"] = 1`), and `{[k] = v}` (explicit key).

### AST

A single `struct node` with a `kind` tag and a union-like blob of fields used per-kind. Not type-safe but the kind tags + the per-arm code makes it clear what's valid where.

### Eval (`eval_expr`, `eval_block`)

Tree-walking. Each `eval_expr` returns a `struct value`. Each `eval_block` returns a `struct ret` carrying `(flow, value)` — flow is `FL_NORMAL`, `FL_BREAK`, or `FL_RETURN`. The block stops early on `FL_BREAK` / `FL_RETURN`, the calling loop / function consumes them.

Scoping is two-tier:
- Locals live in a stack array `struct env::locals[LOCALS_MAX]` that grows on `local NAME` declarations and on function parameter binding, and shrinks at block end.
- Globals live in a single `struct table` shared across all calls. Lookup walks locals top-down then falls through to globals.

This is dramatically simpler than real Lua's nested closure/upvalue scheme but covers all the cases where a script doesn't capture outer locals from inside a function.

### Tables

Single flat array of `(key, value)` pairs with linear lookup. Replace-or-insert on set; key removal on `set(..., nil)`. No separate hash/array part like real Lua — this scales O(n) but n is tiny for any script that fits in user RAM.

Length operator (`#t`) walks `1, 2, 3, ...` and stops at the first nil. Same semantics as real Lua for tables with no array-part holes.

### Numbers are int32 — see the deep design note in `user/lua.c`'s header

Real Lua's number is a double. AdventOS user code can't use floats without either kernel FPU support or libgcc soft-float. Neither was worth the scope, so the interpreter is int32-only. Consequences: `1/3 == 0`, `2^31` overflows silently, `1.5` is a parse error. Documented limit; revisit when/if FPU support lands.

### Built-ins

Just function pointers stored in the globals table at startup (`install_stdlib`). The few built-ins:

```c
print(...)                   write args separated by tab, then newline
type(v)                      "nil"|"boolean"|"number"|"string"|"table"|"function"
tostring(v)                  string conversion
tonumber(v)                  number parse, or nil on failure
ipairs(t)                    returns the iter function (multi-return limit)

string.len(s)                length
string.sub(s, i, [j])        substring (Lua-style 1-indexed, neg counts from end)
string.upper(s) / .lower(s)  case
string.rep(s, n)             repeat n times

table.insert(t, [pos], v)    append or insert at pos
table.concat(t, [sep])       join array part with separator

io.read()                    read a line from stdin
io.write(...)                like print but no newline, no separator

os.time()                    Unix epoch
os.exit([code])              terminate the interpreter
```

That's all of them. ~15 functions, ~200 lines.

### REPL

```c
for (;;) {
    out_str("> ");
    int n = sys_read_line(line, sizeof(line));
    ...
    run_source(line, n);
}
```

Single-process. If `die()` fires inside `run_source`, the whole interpreter exits — there's no recovery. A fork-per-line shape would recover gracefully but adds ~50ms per command and complicates the I/O setup. Documented limitation. The actual fix is `setjmp`/`longjmp` + a `pcall` built-in; that's session 88-ish if anyone wants it.

### Error handling

`die(fmt, ...)` does a minimal varargs format into a static buffer, writes to stderr, and calls `sys_exit(1)`. No setjmp, no recovery. Errors are loud (the message gets to stderr in full) but unrecoverable.

---

## Testing

`fs/hello.lua` is shipped as a smoke-test script. It exercises:

- arithmetic + numeric `for`
- `local function` + recursion
- table constructor + `#t` + `table.insert` + `table.concat`
- string concatenation + `string.upper` / `string.len` / `string.sub`
- `if`/`elseif`/`else` chain
- multiple `print` invocations to stdout

Boot, then at the shell:

```
advent$ lua hello.lua
```

The expected output is included in the "What's implemented" section above. If any line of output doesn't match, that's the bug to chase.

---

## Files touched

```
user/lua.c                  NEW (~1100 LOC) — the whole interpreter
build.sh                    `lua` added to USER_PROGS
mkfs.py                     'lua.elf' to USER_PROGRAMS; man page + hello.lua to DATA_FILES
fs/man/lua                  NEW — man page
fs/hello.lua                NEW — sample script
docs/74-tinylua.md          NEW — this file
README.md                   latest-session pointer updated
```

Build size: `lua.bin` is 27724 bytes. No new syscalls. Kernel image unchanged.

---

## What's next on Path D

If the interpreter gets daily use and the gaps become annoying, the obvious next moves:

- **Closures with upvalues.** Lets `function counter() local n=0; return function() n=n+1; return n end end` work. Non-trivial — requires upvalue resolution in the parser/compiler.
- **`pcall` + `error`.** Needs `setjmp`/`longjmp` (~30 lines of i386 asm for the buffer save/restore, plus a `volatile` discipline at every catch point).
- **Multi-return values.** Touches assignment, function calls, and the AST. Enables `for k,v in pairs(t) do`.
- **String patterns / `string.find` / `gmatch`.** Lua's pattern syntax is simpler than POSIX regex; ~300-500 lines for a full implementation.
- **Mark-sweep GC.** ~150 lines added to the existing struct value layout (no individual headers — values know their kind, so the sweep walks the globals table + every local-stack frame and marks reachable strings/tables/functions).

After those, the major non-floating-point Lua features are all covered. The float gap remains the fundamental wall — solving it means teaching the kernel to save FPU state across context switches, which is its own multi-session project.

---

## Beyond Path D

Path A (Usable Unix) wrapped at session 86. Path D started here. The remaining candidate paths in the README:

- **Path B — Self-hosting (port `tcc`).** With Lua-style scripting now in the OS, scripting tasks have a real home; the next step up is making the OS able to *compile its own programs*. Big symbolic milestone.
- **Path C — Graphics (window manager on VBE).** Visual payoff, lots of input-event routing.
- **Path E — Drivers (virtio, AC97 consumer, more USB device classes).** Pure systems work.
