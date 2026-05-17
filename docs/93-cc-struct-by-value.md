# Session 106 — cc Phase 3 part 10: struct-by-value function args

**Goal.** Let users pass struct values directly to functions: `int f(struct point p)` where `p` is a true copy, not a pointer. Previously cc required `struct point *p` everywhere, forcing users to write `&` at every call site.

Status: **done.** Smoke test:

```
$ cc /sbv.c -o /sbv.elf
cc: wrote /sbv.elf
$ sbv                              ; exit 0
magnitude_sq(3,4) = 25
reverse_p result  = 34
pp untouched      = (3, 4)
sum_triple        = 600
between(1, pp, 2) = 10
```

The fourth and fifth lines are the most interesting: `reverse_p` swaps `p.x` and `p.y` *inside the callee*, but the caller's `pp` stays `(3, 4)` — proving it's a real value copy, not aliasing. And `between(1, pp, 2)` exercises the cumulative-offset math when a struct sits between int args.

---

## Three pieces moving at once

Struct-by-value calls touched more of cc than any earlier Phase 3 session because they require coordinated changes:

1. **Per-function param info, pre-populated.** `gen_call` at any source location needs to know each param's kind, regardless of whether the function is defined yet. So `g_funcs[]` gains `param_kinds[MAX_PARAMS_PER_FUNC=8]` and `param_metas[]`, and a new pass after `parse_program` walks `prog->list` to populate them.

2. **Cumulative ebp offsets in the callee.** Previously each param sat at `[ebp + 8 + i*4]`. With struct-by-value, params can be 8, 12, 16 bytes (or more), so the callee binds locals using a running `cum_off` that bumps by `(struct.size + 3) & ~3` per struct param.

3. **Struct-push at the caller.** Instead of `gen_expr + push_eax`, struct args emit a multi-step copy: `sub esp, size; push esi/edi; lea esi/edi; mov ecx, dwords; rep movsd; pop edi/esi`. The cleanup at the call's end uses cumulative `total_push` rather than `argc * 4`.

Each piece is straightforward in isolation, but they have to agree on the cdecl layout for the call to work.

---

## The cumulative-offset diagram

For `int between(int before, struct point p, int after)` where `struct point` is 8 bytes:

```
caller's view of args, low-addr to high-addr:
  ESP ── [after]        4 bytes (pushed first, ends up at LOW addr)
         [p.y]          \
         [p.x]           > struct point p (8 bytes total, pushed second)
         [before]        4 bytes (pushed third)
         [return addr]   4 bytes — set by `call`
         [saved ebp]     4 bytes — set by callee's `push ebp`
  EBP ── ↑

callee's view via [ebp + N]:
  [ebp + 0]  = saved ebp
  [ebp + 4]  = return addr
  [ebp + 8]  = before
  [ebp + 12] = p.x        — struct starts here
  [ebp + 16] = p.y        — second field of struct
  [ebp + 20] = after
```

Inside `between`, the parameter bindings are:
- `before`: kind=LK_INT, ebp_off=8, size=4 → next cum_off=12
- `p`: kind=LK_STRUCT, ebp_off=12, size=8 → next cum_off=20
- `after`: kind=LK_INT, ebp_off=20, size=4 → next cum_off=24

`p.x` resolves to `[ebp + 12 + 0]`, `p.y` to `[ebp + 12 + 4]`. The existing `N_MEMBER` codegen handles this transparently — it lea's `[ebp + p_off]` for a LK_STRUCT local then adds the field offset, regardless of whether the local is a regular stack slot or a function parameter.

---

## The caller-side struct-push

The interesting machine code at the call site:

```
sub  esp, sz                    ; 81 ec sz_imm32 — reserve space
push esi                        ; 56
push edi                        ; 57
lea  edi, [esp + 8]             ; 8d 7c 24 08 — dst (above our 2 pushes)
lea  eax, [ebp + arg_off]       ; lea source addr into eax (existing helper)
mov  esi, eax                   ; 89 c6
mov  ecx, dwords                ; b9 imm32
rep  movsd                      ; f3 a5
pop  edi                        ; 5f
pop  esi                        ; 5e
```

`esi` and `edi` are pushed-then-popped because they're cdecl callee-saved — if the surrounding compiler-generated code (or another call's pre-push setup) was holding values in them, we must restore.

The `lea edi, [esp + 8]` accounts for the two `push` instructions between `sub esp, sz` and the lea. After `sub esp, sz`, ESP points at where the struct goes. After two pushes (esi+edi), ESP is 8 lower, so `[ESP + 8]` gets back to the struct destination.

---

## The pre-populate trick

Why pre-populate `g_funcs[]` from the AST before codegen? Because at a call site, we need to know:

- How many params the callee expects (existing — for the argc check)
- Whether the callee is variadic (session 105 — to skip the argc check)
- **What kind each param is** (session 106 — to decide between `push eax` and the struct-push sequence)

Before session 106, both 1 and 2 were filled in by `gen_func` when the callee's definition was reached. Calls before the definition saw stale info but the argc/variadic check tolerated it.

With session 106, calls BEFORE the definition would have all-zeros for `param_kinds`, which would treat struct args as ints — wrong codegen.

The fix: do a pre-pass over the AST after parsing, where every `N_FUNC_DECL` gets its entry created and filled. By the time gen_call runs anywhere, every called function's param info is correct.

This also fixes an old wart: cc previously could miscompile programs where a forward call's argc differed from the eventual definition's. Now `g_funcs[idx].n_params` is set authoritatively at parse-end.

---

## Restrictions

- **Struct-by-value RETURNS are still not supported.** Returning a struct by value in cdecl requires either:
  - The caller passes a hidden first arg that's a pointer to the destination, and the callee writes the struct through that pointer
  - Or for small structs (≤8 bytes), pack into EAX:EDX
  
  Neither is implemented yet. Returns must use out-pointer args.

- **Struct-by-value args must be NAMEs of local LK_STRUCT.** `f(*p)` where `p` is a struct* doesn't work yet (would need dereference-and-copy codegen at the call site).

- **Global struct-by-value args.** I bailed (`die_at`) when the arg is a global struct. The fix would be to emit `mov esi, GLOBAL_VA` instead of `lea esi, [ebp + off]` — a small addition for the next session.

- **MAX_PARAMS_PER_FUNC = 8.** Beyond 8 params, the function's param info isn't fully tracked. Cap is bumpable.

---

## Files touched

- `user/cc.c` — `MAX_PARAMS_PER_FUNC=8` + arrays in func_info; pre-populate pass in `main()`; struct-by-value branch in `parse_func` (accept no `*`); cumulative-offset binding in `gen_func`'s param loop; struct-push in `gen_call`; `total_push` accumulator for cleanup. ~120 lines added.
- `fs/sbv.c` — sample exercising single-struct call, multi-field struct, struct-with-mutation-doesn't-leak, mixed int+struct+int.
- `fs/man/cc` — struct-by-value docs.
- `mkfs.py` — added sbv.c; removed ops.c / fnptr.c to stay under file cap.
- `README.md` — pointer bump.
- DELETED: `fs/ops.c` (session 96 demo), `fs/fnptr.c` (session 98 demo). Their content lives in docs/83 + docs/85.

cc.bin: 293 KiB → 298 KiB.

---

## Phase 3 status after session 106

Ten sub-sessions shipped:

- ✅ 97 — structs
- ✅ 98 — function pointers
- ✅ 99 — sizeof + scaled pointer arithmetic
- ✅ 100 — multi-file compilation
- ✅ 101 — struct value assignment
- ✅ 102 — array-of-struct + indexed member access
- ✅ 103 — enum
- ✅ 104 — typedef
- ✅ 105 — variadic functions
- ✅ 106 — struct-by-value calls
- ⏳ 107+ — struct-by-value returns, optimization, register allocator, etc.

After this session, cc handles essentially the entire C language surface that small-to-medium programs need. The remaining items are optimizations (register allocator, peephole, constant folding) and a few specialized corners (struct-by-value returns, function-pointer typedefs, `static`, `extern`). None of them are required for "compile a real program" anymore.

The post-Phase-3 decision — keep extending cc, port tcc, or pivot to another path — gets easier to answer now: cc has earned a real runway. Any feature work from here is incremental polish, not a blocker.
