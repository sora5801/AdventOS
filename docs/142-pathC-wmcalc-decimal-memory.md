# Session 156 — Path C phase 49: wmcalc decimal + memory keys

**Goal.** Two long-standing papercuts in wmcalc (session 139):
the `.` button was a placeholder that did nothing, and there was
no memory register.  This session adds fixed-point decimal
arithmetic and four memory buttons (M+, M-, MR, MC).

Status: **done.**  Smoke `smoke_wmcalc_dec.py` (3/3):

```
=== checks ===
  [OK] decimal arithmetic rendered (36 green px)
  [OK] memory row painted (7058 blue px)
  [OK] wmd status bar alive (842/924)
```

Sequence `1 . 5 + 2 . 2 5 =` produces `3.75` on the display.
The new bottom-row memory buttons paint in a distinctive
blue slate so the user can spot the memory cluster at a glance.

---

## Number representation

```c
struct num_t {
    int mant;   /* signed mantissa */
    int dec;    /* number of decimal places (0 = integer) */
};
```

A value `v = mant * 10^(-dec)`.  Examples:

| display    | mant   | dec |
|------------|--------|-----|
| 0          | 0      | 0   |
| 12         | 12     | 0   |
| 1.5        | 15     | 1   |
| -0.001     | -1     | 3   |
| 3.75       | 375    | 2   |

After every arithmetic operation the result is "normalised" —
trailing zeros after the decimal point get trimmed:

```c
static void num_normalize(struct num_t *n) {
    while (n->dec > 0 && n->mant != 0 && n->mant % 10 == 0) {
        n->mant /= 10;
        n->dec--;
    }
}
```

So `1.50 + 0.50 = 2.00 → 2`, not `2.00`.

---

## Arithmetic

**Add / subtract** (`num_add` with a sign parameter):

```c
int d = max(a.dec, b.dec);
num_align(&a, d);   /* scale a.mant up by 10^(d - a.dec) */
num_align(&b, d);
return (struct num_t){ a.mant ± b.mant, d };
```

**Multiply**:

```c
return (struct num_t){ a.mant * b.mant, a.dec + b.dec };
```

with `safe_mul()` checking int32 overflow first (the kernel
doesn't link libgcc, so we lack `__builtin_mul_overflow`).

**Divide** — the hardest one, because integer division loses
fractional digits.  We scale the numerator up to recover them:

```c
int extra = 0;
int scaled = a.mant;
while (extra < 4) {
    int s;
    if (safe_mul(scaled, 10, &s)) break;
    scaled = s;
    extra++;
}
return (struct num_t){ scaled / b.mant, a.dec - b.dec + extra };
```

Up to 4 extra decimal places of precision; the loop backs off
on overflow.  So `1 / 3 = 0.3333` (4 places after normalisation).

---

## Memory register

A third `struct num_t g_mem` holds the memory value.  Four
buttons:

| key | action                              |
|-----|-------------------------------------|
| M+  | `g_mem = num_add(g_mem, g_cur, +1)` |
| M-  | `g_mem = num_add(g_mem, g_cur, -1)` |
| MR  | `g_cur = g_mem; g_entering = 0`     |
| MC  | `g_mem = (struct num_t){0, 0}`      |

Plus keyboard shortcuts on the unshifted letters `m`, `n`, `r`,
`k` (chosen because `M` shifted is awkward to type during fast
calculator workflows; `r` and `k` are paired with `m`/`n`).

A small "M" indicator paints in the bottom-left of the display
panel whenever `g_mem.mant != 0` so the user knows there's a
stored value.

---

## Button grid grows to 6 rows

The original 5-row grid couldn't fit memory without crowding.
Added a 6th row at the bottom:

```
Row 0:  C    +/-   <-   /
Row 1:  7    8    9    *
Row 2:  4    5    6    -
Row 3:  1    2    3    +
Row 4:  0    .    =    AC
Row 5:  M+   M-   MR   MC      ← session 156
```

The memory row paints in `0x405068` (cool blue slate) to
visually separate it from the digit/op clusters.  Window height
grew from 320 → 368 px to make room.

---

## What stays out of scope

- **Long-double precision.**  All math is signed int32.  When
  `mant * mant` would overflow, the display goes `ERR`.  No
  arbitrary-precision; no scientific notation.
- **Memory chains.**  No `M+=` button; combine M+ with the
  sign toggle for subtraction-into-memory if you don't want to
  use M-.
- **Operator precedence / parentheses.**  Still two-register
  left-to-right.  `2 + 3 * 4 =` gives `20`, not `14`.
- **History / scroll.**  The display shows the current entry
  only; previous results vanish into the accumulator.

---

## Files touched

- `user/wmcalc.c` — rewritten:
  - `struct num_t { int mant; int dec; }`
  - `safe_mul`, `num_align`, `num_normalize`, `num_add`,
    `num_mul`, `num_div`
  - `press_dot`, `press_mem`
  - 24-entry `g_btns[]` (was 20), 6-row grid
  - `print_num` replaces `print_decimal`
  - Memory-row distinctive blue background
  - "M" indicator on the display when memory is non-zero
- `fs/man/wmcalc` — full rewrite documenting decimals + memory
- `smoke_wmcalc_dec.py` — new harness, 3 pixel checks
- `docs/142-pathC-wmcalc-decimal-memory.md` — this file

Sizes:
- kernel.bin: 164016 (unchanged — pure userspace)
- wmcalc.bin: 6088 → 8028 (+1940 B for fixed-point arith +
  memory + 4 more buttons + indicator paint)

---

## Path C status after session 156

- ✅ 107..155 — see prior docs
- ✅ 156 — wmcalc decimal arithmetic + memory keys
- ⚠️  wmterm input + close — still deferred

wmcalc is now a usable pocket calculator — fixed-point math
with M+/M-/MR/MC memory, fits within the int32 budget the
freestanding userspace gives us.
