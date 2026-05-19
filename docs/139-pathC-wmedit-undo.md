# Session 153 — Path C phase 46: wmedit Ctrl-Z undo

**Goal.** Add a linear undo stack to wmedit.  Ctrl-Z pops the
last edit (with light coalescing so a forward-typing burst or
backspace burst is one step instead of one-per-character).

Status: **done.**  Smoke `smoke_wmedit_undo.py` (3/3):

```
=== checks ===
  [OK] baseline has typed text (101 green px)
  [OK] Ctrl-Z removed text (0 green px remaining)
  [OK] wmd status bar alive (832/924)
```

Boot wmd + wmedit, click in the body, type "hello" (5 chars,
coalesced into one undo entry), Ctrl-Z — the entire word
vanishes in a single step.

---

## Stack layout

```c
#define UNDO_MAX 64
#define UNDO_PIECE_MAX 40

struct undo_entry {
    int  kind;     /* 1 = insert (delete to undo), 2 = delete (re-insert) */
    int  offset;
    int  len;
    char text[UNDO_PIECE_MAX];   /* used for kind 2 only */
};
static struct undo_entry g_undo[UNDO_MAX];
static int g_undo_count;
```

`UNDO_MAX = 64` entries × ~52 bytes each = ~3.3 KB of state.
When the stack fills the oldest entry is evicted (FIFO drop).

Per-entry text capacity of 40 bytes means undoing a very large
selection-delete restores the cursor position but not the
content past 40 bytes — known limitation, documented.

---

## Coalescing rules

Two rules collapse adjacent edits into one undo step:

1. **Forward typing**: `kind == 1` (insert) at offset exactly at
   `prev->offset + prev->len`.  Extend the previous entry's
   `len`.  Net effect: "abc" types as three insert events but
   one undo entry of `{kind=1, offset=N, len=3}`.

2. **Backspace burst**: `kind == 2` (delete) with `len == 1`,
   where `offset + 1 == prev->offset`.  Prepend the new
   character to `prev->text`, decrement `prev->offset`, bump
   `prev->len`.  Net effect: 5 backspaces collapses to one
   undo entry that re-inserts all 5 bytes.

Any other transition (different kind, non-adjacent position,
text overflow) pushes a fresh entry.

---

## Undoable wrappers

Every place that mutates `g_buf` now goes through a wrapper that
records the inverse before the bare helper runs:

```c
static void buf_insert_record(char c) {
    if (g_len >= BUF_MAX - 1) return;
    undo_push(1, g_cur, 1, NULL);
    buf_insert(c);
}
static void buf_delete_record(void) {
    if (g_cur == 0) return;
    char snap = g_buf[g_cur - 1];
    undo_push(2, g_cur - 1, 1, &snap);
    buf_delete();
}
static void sel_delete_record(void) {
    if (!sel_active()) return;
    int lo = sel_lo();
    int hi = sel_hi();
    int len = hi - lo;
    int snap_len = len > UNDO_PIECE_MAX ? UNDO_PIECE_MAX : len;
    char snap[UNDO_PIECE_MAX];
    for (int i = 0; i < snap_len; i++) snap[i] = g_buf[lo + i];
    undo_push(2, lo, snap_len, snap);
    sel_delete();
}
```

The bare `buf_insert` / `buf_delete` / `sel_delete` helpers stay
untouched — `undo_pop` calls them directly so undo doesn't
re-record itself.

Call sites in the WM_EV_KEY handler swap:
- `buf_insert(c)` → `buf_insert_record(c)`
- `buf_delete()` → `buf_delete_record()`
- `sel_delete()` → `sel_delete_record()`

(Pre-edit `sel_delete()` calls inside type/paste handlers also
become `sel_delete_record()` so a "type with selection" replaces
the deletion in undo, not just the insertion.)

---

## Undo pop

```c
static void undo_pop(void) {
    if (g_undo_count == 0) return;
    struct undo_entry *e = &g_undo[--g_undo_count];
    if (e->kind == 1) {
        /* Insert was at [offset, offset+len); shift left. */
        for (int i = e->offset; i + e->len < g_len; i++)
            g_buf[i] = g_buf[i + e->len];
        g_len -= e->len;
        g_cur = e->offset;
    } else if (e->kind == 2) {
        /* Delete removed e->text; shift right + restore. */
        int len = e->len > UNDO_PIECE_MAX ? UNDO_PIECE_MAX : e->len;
        if (g_len + len < BUF_MAX) {
            for (int i = g_len + len - 1; i >= e->offset + len; i--)
                g_buf[i] = g_buf[i - len];
            for (int i = 0; i < len; i++)
                g_buf[e->offset + i] = e->text[i];
            g_len += len;
            g_cur = e->offset + len;
        }
    }
    sel_clear();
    g_dirty = 1;
}
```

Bound to **Ctrl-Z (0x1A)** in the key dispatch.

---

## What stays out of scope

- **Ctrl-Y / Shift+Ctrl-Z redo.**  Once you type after undoing,
  the popped history is lost.  Linear stack only.
- **Per-character undo.**  Coalescing means you can't undo
  "just the 'l' in hello" — the whole typing burst is one step.
- **Mid-edit checkpoints.**  No "every N seconds save a
  checkpoint" — Ctrl-Z is your only restore.
- **Cross-load persistence.**  Closing wmedit and re-opening
  loses the undo history.
- **Selection-delete > 40 bytes.**  Cursor position restores
  but the deleted text past byte 40 is gone.  Increase
  `UNDO_PIECE_MAX` if this bites; trade-off is state size.

---

## Files touched

- `user/wmedit.c`:
  - `g_undo[]` stack + `g_undo_count`
  - `undo_push` with type-forward + backspace coalescing
  - `undo_pop` reverses the most recent entry
  - `buf_insert_record` / `buf_delete_record` /
    `sel_delete_record` wrappers
  - Ctrl-Z (0x1A) dispatch in WM_EV_KEY
  - All call sites switched to the recording wrappers
- `fs/man/wmedit` — documents Ctrl-Z and the 40-byte snap cap
- `smoke_wmedit_undo.py` — new harness, 3 pixel checks
- `docs/139-pathC-wmedit-undo.md` — this file

Sizes:
- kernel.bin: 164016 (unchanged — pure userspace)
- wmedit.bin: 17288 → 21656 (+4368 B — bulk is the
  `g_undo[64]` array sitting in BSS-as-data)

---

## Path C status after session 153

- ✅ 107..152 — see prior docs
- ✅ 153 — wmedit Ctrl-Z undo with coalescing
- ⚠️  wmterm input + close — still deferred

wmedit now has the full "real editor" core: navigation,
selection, clipboard, **search** (152), **undo** (153).  Redo
would be the only obvious missing piece if you wanted to call
the editor "polished."
