# Session 128 — cc language corners (11 features in one branch)

After session 121 declared the Path B language surface "complete" and
session 125 added optimization passes, the remaining list in the
README was a grab-bag of corners — features that aren't blockers for
typical programs but every now and then someone needs them. This
session ships eleven of them in one branch, one commit each.

## What landed

| # | Feature | New token / node | Smoke check |
|---|---------|------------------|-------------|
| 1 | Comma operator (`a, b`) | `N_COMMA`, `parse_comma_expr` | `comma = 30` |
| 2 | Bitwise / shift compound assigns (`&=`, `|=`, `^=`, `<<=`, `>>=`) | 5 new tokens, reuses session-96 rewrite | `b_xor_eq = 4` etc. |
| 3 | `do { ... } while (cond);` | `T_DO`, `N_DO_WHILE` | `do_while_sum = 15` |
| 4 | `break` / `continue` | `T_BREAK`/`T_CONTINUE`, loop-context stack | `break_continue = 19` |
| 5 | `union` | `T_UNION` + `is_union` flag on struct_info | `union_as_str = ok` |
| 6 | `sizeof NAME` / `sizeof(NAME)` | `N_SIZEOF_NAME` (codegen-time fold) | `sizeof_u = 4` |
| 7 | `goto` + labels | `T_GOTO`, `N_LABEL`/`N_GOTO`, per-function fixup table | `goto_sum = 6` |
| 8 | Assignment-as-expression | parse_expr accepts `NAME = rhs` | `assign_expr_b = 7` |
| 9 | `switch` / `case` / `default` | 3 tokens + 3 nodes, cascading-compare dispatch | `switch_total = 122` |
| 10 | 2D arrays (`int a[N][M]`, `a[i][j]`) | `N_INDEX2`/`N_INDEX2_ASSIGN`, `dim2` on local_slot | `array2d_sum = 21` |
| 11 | `static` LOCAL variables | Parse-time mangle + body-AST rename pass | `static_local_3 = 103` |

Smoke target: [`fs/corners.c`](../fs/corners.c) runs through every
feature in order and prints 22 anchor lines, all checked by
[`smoke_corners.py`](../smoke_corners.py).

## The interesting bits

### Comma operator — separator vs operator disambiguation

`parse_expr` is called both at expression positions AND inside
argument lists (`f(a, b, c)`), where comma is the *separator*, not
the operator. Folding comma-absorption into `parse_expr` would
break call-arg parsing. The fix: keep `parse_expr` strict and add a
sibling `parse_comma_expr` that wraps it in a `, parse_expr` loop.
It's called only at contexts where comma is unambiguously the
operator — `(expr)` inside parens, and the top of an
expression-statement.

### break / continue — loop context stack with switch-skip

Each enclosing while/do-while/switch pushes a `loop_ctx`. break
appends its forward-jmp placeholder to `break_jmps[]`; continue
appends to `cont_jmps[]` (forward) or emits a direct backward jmp
when the cont target is known up-front (while's `top` is captured
before the body).

Switch frames set `is_switch=1`. `continue` walks back past switch
frames via `loop_top_for_continue()` — matches C semantics where
`continue` inside `switch (...) { while (...) { ...; continue; ... }}`
belongs to the `while`, not the `switch`.

### Union — reused struct machinery

A union is just a struct_info with `is_union=1`, all field offsets
stamped at 0, and size = max(field sizes). Since every cc field is
4 bytes, union size is always 4. All existing struct codegen sites
(member access, member assign, field-typed function args/returns)
work without modification — they use the field_info.offset
directly, which is correct for both struct and union.

### sizeof NAME — codegen-time fold

`sizeof(TYPE)` has folded to N_NUM at parse time since session 99.
But `sizeof NAME` can't: the local/global symbol tables aren't
populated until `gen_func` runs. The fix: a new `N_SIZEOF_NAME` AST
node whose codegen looks up the name's kind and emits
`mov eax, computed_size`. Typedef-NAMEs (which are in the
parse-time `g_typedefs[]` table) still fold at parse time —
fastest path.

### goto — single resolution pass at function end

Forward and backward gotos both go through a per-function fixup
list (`g_gotos[]`). `N_LABEL` records its `code_off = g_code_len`;
`N_GOTO` emits a `jmp rel32` placeholder. At the end of
`gen_func`, every fixup is resolved against the label table —
undefined labels die with the original source line number. Uniform
handling instead of trying to special-case backward gotos was
simpler and only adds a single pointer-table walk per function.

### Assignment-as-expression — right-associative parse-tail

`parse_expr` gains a tail check: if the parsed LHS is a plain
`N_NAME` and the next token is `T_ASSIGN`, consume `= rhs` (right-
recursive `parse_expr` for the RHS — gives natural right-associat-
ivity for `a = b = c`). `gen_expr` learns a case `N_ASSIGN` that
delegates to `gen_stmt`'s existing codegen, which already leaves
the stored value in EAX on the scalar path.

LHS is intentionally limited to plain `N_NAME`. `*p = x`,
`a[i] = x`, field stores etc. stay statement-only — extending to
those would require duplicating each store-codegen path into
`gen_expr`, and the value-in-EAX guarantee gets fiddly for some.

### Switch — pre-scan + cascading compare

```
gen_expr(switch_value)             ; eax = value
cmp eax, V1; je case_V1_target     ; emitted for each non-default case
cmp eax, V2; je case_V2_target
...
jmp default_or_end

case_V1_target:
   body of case V1
case_V2_target:
   body of case V2
default_target:
   body of default
end:
```

Pre-scan walks the body's top-level statements once to find all
N_CASE / N_DEFAULT positions. The dispatch chain emits with
forward-jmp placeholders that get patched as each case's body
position is reached during body codegen. Fall-through works
naturally because no jumps are emitted at case boundaries — the
user writes `break;` (uses the loop_ctx's break_jmps[]) to end a
case explicitly.

Restrictions: case/default labels must be top-level in the switch
body (no nested cases inside if/while inside the switch), case
values are integer literals only.

### 2D arrays — flat layout, `(i*M + j) * elem` addressing

`int a[N][M]` is a flat `N * M * 4`-byte block on the stack. Access
`a[i][j]` computes the address as `&a[0][0] + (i * M + j) * 4`.
The inner dimension M is stored on a new `dim2` field of
`local_slot` at decl time; codegen looks it up there.

Two new AST nodes — `N_INDEX2` for reads, `N_INDEX2_ASSIGN` for
writes. Parse_primary's `NAME[...]` looks for a second `[` after
the first `]` and produces N_INDEX2 if found. Same in parse_stmt's
assignment path.

Local only. Two dimensions max. Element types int/char only. Real
multi-dim support (3D+, globals, struct elements) would require
threading dim info through `g_globals` and the struct registry.

### static LOCAL — parse-time mangle + AST rename pass

The trickiest one to design well. C says `static int x;` inside a
function gives x persistent storage across calls, distinct from
the same-named static in another function.

Approach:

1. At parse time, `static int x [= N];` mangles to
   `_sl_<funcname>_<x>` and registers a backing global via
   `global_declare`. The initializer (if any) writes into the data
   pool immediately.
2. A rename entry `(x → _sl_<fn>_<x>)` is appended to a per-
   function rename table.
3. The parser returns an `N_NOP` placeholder — no AST node
   describing the static decl is needed because the global is
   already in place.
4. After `parse_block` returns the function body, `parse_func`
   walks the body AST and rewrites every `name` field that
   matches a rename entry. After the walk, the body looks like
   ordinary global access — the codegen doesn't need to know
   that a static-local ever existed.

`g_n_renames` resets at the start of each `parse_func`. Two
functions with `static int counter` produce two distinct mangled
globals.

Restrictions: int/char (with optional `*`) only; integer literal
initializer; mangled names share NAME_MAX (24) with all other
names so long fn+var combinations clamp.

## Cumulative size

`fs/corners.c` exercises all 11 features in one program. cc
compiles it cleanly:

  - cor.elf: 3809 bytes
  - Boots, runs, all 22 expected output lines match.

cc.bin itself grew 302308 → 319684 bytes (+17 KiB) for ~1500
lines of new compiler code. Most of the bytes come from switch
(generic dispatch logic + the loop_ctx integration) and goto
(label/goto fixup tables + the function-end resolution loop).

## Files touched

- `user/cc.c` — ~1500 lines net additions across 11 commits.
- `fs/corners.c` — new smoke target, grows commit by commit.
- `smoke_corners.py` — new smoke harness, mirrors smoke_pathB.py.
- `mkfs.py` — corners.c wired into the FS.
- `docs/115-pathB-language-corners.md` — this file.
- `README.md` — current-session pointer.
