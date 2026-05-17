# Session 93 — cc Phase 2 part 3: global variables

**Goal.** Add file-scope `int` / `char` / pointer / array declarations and let user code read and write them. After this, the only "missing piece" between cc's output and a real-program-shaped binary is variadic functions (for `printf`) and the preprocessor.

Status: **done.** First-try smoke test:

```
$ cc /globs.c -o /globs.elf
cc: wrote /globs.elf
$ globs                                ; exit code 3
100        ; counter = 100   (init)
-7         ; neg_init = -7
0          ; hits            (uninitialized → zero)
101 102 103  ; counter after 3 bump()s
3          ; hits             (also incremented in bump)
Hi, glob   ; global char[16] modified byte-by-byte
108        ; local + global mixed expression
```

No regressions: chars.elf, strs.elf, hello.elf all still produce identical output to their previous sessions.

---

## The shape

Globals follow the same pattern as session 91's string pool: a data area is appended to the PT_LOAD segment, and the compiler tracks fixups for every code reference. After all codegen is done — and after the string pool has been laid down — the global pool gets appended and patched.

```
[0..83]      ELF header + phdr        ; filesz=memsz=code+strings+data
[84..C-1]    g_code                    ; machine code
[C..S-1]     g_str_pool                ; string literals, NUL-terminated
[S..]        g_data_pool               ; globals, padded to 4 bytes each
```

`C` and `S` are determined at finalization. Each `g_globals[i]` records an `offset` within `g_data_pool`; its final VA is `ENTRY_VA + S + offset`.

Initialized globals get their bytes written into `g_data_pool` at parse time; uninitialized globals are left as the zero they already are (the pool is BSS-init'd to all zeros).

---

## Lookup and codegen

The name-resolution order is `local_find` → `global_find`. Every site that previously assumed a NAME was a local — `N_NAME`, `N_ADDR_OF`, `N_DEREF`, `N_INDEX`, `N_ASSIGN`, `N_DEREF_ASSIGN`, `N_INDEX_ASSIGN` — now has a "local? yes; else: global? yes; else: undefined" cascade.

For globals, the address VA isn't known until finalization. So the emitter writes an `imm32` placeholder and records a `glob_fixup{ code_off, glob_idx }`. The final patch pass replaces the placeholder with `data_pool_base_va + g_globals[gi].offset`.

The compact i386 absolute-address move forms are nice here — `mov eax, [imm32]` is just `A1` + 4 bytes (5 bytes total), and `mov [imm32], eax` is `A3` + 4 bytes:

```c
static int e_mov_eax_at_abs(void) {
    emit_b(0xa1);
    int off = g_code_len;
    emit_d(0);
    return off;     /* file offset of the imm32 to patch */
}
```

Same shape for `mov [imm32], al` (byte store) and the `0f b6 05 imm32` movzx form (byte load with zero-extend), in case a future session adds scalar `char` globals. For now scalar `char` is treated as int-sized so we don't need the byte forms at file scope.

---

## Parser: function vs global disambiguation

Top-level declarations both start with `int|char [*] NAME`. After the name we look at the next token: `(` → function, `;`/`=`/`[` → global.

```c
int main()           /* function */
int counter = 100;   /* global */
char buf[16];        /* global array */
int *handles;        /* global pointer */
```

This is the first time the parser actually does multi-token lookahead in a structural sense — earlier sessions only peeked one ahead for things like `NAME = expr`. Here we peek past the optional `*` and the name to look at the next significant token. Implementation is straightforward with `tk_peek(d)`.

Initializers for globals are restricted to integer literals (positive or negative) for now. String-literal global init would need ordering — strings finalize before globals so a global initialized to a string pointer would have to record its own pending fixup. A later session.

---

## The fixup waltz

There are now three kinds of fixups in cc:

1. **Function call fixups** (`g_fixups`) — rel32 displacements between a `call` and its target function. Resolved by `resolve_fixups()` once every function has its `entry_off`.
2. **String literal fixups** (`g_str_fixups`) — absolute imm32 holding the VA of a string. Resolved after the string pool is appended.
3. **Global variable fixups** (`g_glob_fixups`) — absolute imm32 holding the VA of a global. Resolved after the data pool is appended.

The order matters: function fixups first (they don't depend on pool placement), then strings, then globals. Each pass extends `g_code_len` and the next pass uses the new value as its base VA. Conceptually:

```
A: resolve_fixups()                    ; patch all call disps
B: append strings to g_code            ; g_code_len → S
   patch every str_fixup with VA(s_i)
C: append globals to g_code            ; g_code_len → S + pool_size
   patch every glob_fixup with VA(g_i)
D: write_elf(g_code_len)               ; emit the whole image
```

Globals can reference each other only at compile time via their integer initializer, never at runtime via "initialize global p to &q" — that would require an extra runtime relocation, and we don't bother.

---

## What this unlocks

The interesting programs you can finally write:

```c
/* A reference counter or program counter that persists across calls */
int call_count;
int next_id() {
    call_count = call_count + 1;
    return call_count;
}

/* A scratch buffer the whole program shares without passing pointers */
char message[64];
void format_greeting(char *name) {
    /* ... write to message ... */
}
int main() {
    format_greeting("world");
    puts(message);
}

/* A configuration table */
int verbose = 0;
int max_depth = 8;
```

Real programs read configuration at startup, accumulate state in module globals, and write output to shared buffers. Sessions 91/92 covered the data shapes; session 93 makes them addressable from anywhere in the program.

---

## What's deferred to later

- **String initializers for globals** (`char *prog_name = "globs";`). Needs string-pool finalization to happen BEFORE the parser knows the string's address. Doable; just hasn't been wired.

- **Array literal initializers** (`int primes[] = {2, 3, 5, 7};`). Needs a brace-init parser path. Out of scope for "the minimum to read/write globals."

- **Const-correctness / `static` storage class.** Real C has both. Neither matters yet — every cc global is effectively `static` (single-file program, no linkage to outside).

- **A real `.data` section in the ELF.** We still emit one PT_LOAD covering code + strings + globals, all RWX. A pedantic ELF would have separate `.text`/`.rodata`/`.data`/`.bss` segments with correct permission bits. Doesn't matter for AdventOS where the kernel maps every PT_LOAD as RWX anyway.

---

## Files touched

- `user/cc.c` — `g_globals[]` table, `g_data_pool[]`, `g_glob_fixups[]`; `global_declare` / `global_find`; absolute-address emit helpers (`e_mov_eax_at_abs`, `e_mov_at_abs_eax`, `e_movzx_eax_at_abs_b`, `e_mov_at_abs_al`, `e_mov_eax_imm_for_fixup`); `parse_global_decl`; top-level function-vs-global disambiguation in `parse_program`; local-then-global cascade in every name-handling codegen case; data-pool append + fixup patching after string-pool. Forward decls added to keep the file linear. ~200 lines added.
- `fs/globs.c` — new sample with `counter`, `hits`, `neg_init`, `buf[16]`, a `bump()` function, and mixed local-plus-global expression.
- `fs/man/cc` — globals docs.
- `mkfs.py` — added `globs.c`.

cc.bin: 185 KiB → 202 KiB. The growth is mostly `DATA_POOL_MAX=8192` + `g_globals[128]` + `g_glob_fixups[256]`.

---

## Phase 2 status after session 93

Three of five sub-sessions shipped:

- ✅ 91 — string literals + puts
- ✅ 92 — char/pointers/arrays
- ✅ 93 — globals
- ⏳ 94 — printf (variadic + format strings)
- ⏳ 95 — preprocessor
- ⏳ 96 — compound operators + ++/-- + ternary

After 94 we can write programs that print formatted output, which is the last piece needed to do anything "useful" with cc. After 95 we can write multi-file projects with shared headers. 96 is a polish pass.

At that point we can compare cc's reach against a real `tcc` port and decide whether the next step is "keep extending cc" or "port tcc."
