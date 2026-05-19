# Session 154 — Path C phase 47: wmedit Ctrl-Y redo

**Goal.** Round out session 153's undo with redo support.  Ctrl-Y
re-applies the most recently undone edit; fresh edits clear the
redo stack (standard editor convention).

Status: **done.**  Smoke `smoke_wmedit_redo.py` (4/4):

```
=== checks ===
  [OK] baseline text rendered (101 px)
  [OK] undo cleared text (0 px remaining)
  [OK] redo restored text (101 px, baseline was 101)
  [OK] wmd status bar alive (832/924)
```

Typed "hello" → 101 green text pixels.  Ctrl-Z → 0.  Ctrl-Y →
101 again, **exactly** the baseline count.

---

## Two-stack model

```c
static struct undo_entry g_undo[UNDO_MAX];
static int g_undo_count;
static struct undo_entry g_redo[UNDO_MAX];
static int g_redo_count;
```

Both stacks hold the same `struct undo_entry` format from
session 153 (kind / offset / len / text[40]).  Difference is
direction: an entry on the *undo* stack describes an action
already performed (pop reverses it); on the *redo* stack it
describes an action that was just reversed (pop re-performs it).

The mechanic is symmetric:

```c
static void undo_pop(void) {
    if (g_undo_count == 0) return;
    struct undo_entry e = g_undo[--g_undo_count];
    struct undo_entry inv = apply_inverse(&e);
    stack_push(g_redo, &g_redo_count, &inv);
    sel_clear();
    g_dirty = 1;
}

static void redo_pop(void) {
    if (g_redo_count == 0) return;
    struct undo_entry e = g_redo[--g_redo_count];
    struct undo_entry inv = apply_inverse(&e);
    stack_push(g_undo, &g_undo_count, &inv);
    sel_clear();
    g_dirty = 1;
}
```

`apply_inverse` reverses an entry's action on `g_buf` and returns
a new entry describing the inverse (so the caller can stash it on
the opposite stack).  Both `undo_pop` and `redo_pop` use the same
helper — only the stack direction differs.

---

## apply_inverse

```c
static struct undo_entry apply_inverse(const struct undo_entry *e) {
    struct undo_entry inv = *e;
    if (e->kind == 1) {
        /* Recorded insert: capture bytes from g_buf (in case
         * entry.text is stale), then delete. */
        int n = e->len > UNDO_PIECE_MAX ? UNDO_PIECE_MAX : e->len;
        for (int i = 0; i < n; i++) inv.text[i] = g_buf[e->offset + i];
        for (int i = e->offset; i + e->len < g_len; i++)
            g_buf[i] = g_buf[i + e->len];
        g_len -= e->len;
        g_cur = e->offset;
        inv.kind = 2;
    } else if (e->kind == 2) {
        /* Recorded delete: re-insert text. */
        int len = e->len > UNDO_PIECE_MAX ? UNDO_PIECE_MAX : e->len;
        if (g_len + len < BUF_MAX) {
            for (int i = g_len + len - 1; i >= e->offset + len; i--)
                g_buf[i] = g_buf[i - len];
            for (int i = 0; i < len; i++)
                g_buf[e->offset + i] = e->text[i];
            g_len += len;
            g_cur = e->offset + len;
        }
        inv.kind = 1;
    }
    return inv;
}
```

Kind toggles on every traversal: an `insert` entry, when undone,
produces a `delete` entry for the redo stack; redoing that
`delete` (which restores the insertion) produces another `insert`
entry back on the undo stack.  Symmetric round-trip.

The defensive `inv.text[i] = g_buf[...]` capture in the kind=1
branch matters because `buf_insert_record` now stores the
inserted byte too, but old entries from a previous wmedit run
(if loaded somehow) might not.

---

## Fresh-edit redo invalidation

In `undo_push` — the entry point for *every* user-initiated edit
via the `_record` wrappers — we clear the redo stack first:

```c
static void undo_push(int kind, int offset, int len, const char *text) {
    /* Session 154 — any fresh edit invalidates the redo history. */
    g_redo_count = 0;
    /* ...coalesce-or-push as before... */
}
```

That's the "type something after undoing, and redo is gone"
behaviour every other editor implements.  `undo_pop` and
`redo_pop` themselves don't go through `undo_push` (they call
`stack_push` directly), so they don't trip this clear.

---

## What stays out of scope

- **Cross-coalesce redo.**  A redo of a coalesced typing burst
  re-applies the whole burst in one Ctrl-Y, same as Ctrl-Z
  undid it.  But splitting a coalesced step into individual
  characters isn't supported.
- **Branching undo.**  Linear stack only — no per-character
  alternate-history navigation à la Vim's `g-`.
- **Persistent history.**  Closing wmedit loses both stacks.

---

## Files touched

- `user/wmedit.c`:
  - `g_redo[]` + `g_redo_count`
  - generic `stack_push` helper
  - `undo_push` clears redo at entry (only via the
    user-edit path)
  - `apply_inverse` factored out so undo_pop and redo_pop
    share one implementation
  - `redo_pop` mirror of `undo_pop`
  - Ctrl-Y (0x19) dispatch in WM_EV_KEY
  - `buf_insert_record` now passes the inserted byte so redo
    can re-insert without re-reading g_buf later
- `fs/man/wmedit` — Ctrl-Y listed + limitations updated
- `smoke_wmedit_redo.py` — new harness, 4 pixel checks
- `docs/140-pathC-wmedit-redo.md` — this file

Sizes:
- kernel.bin: 164016 (unchanged — pure userspace)
- wmedit.bin: 21656 → 25960 (+4304 B; bulk is the second
  `g_redo[64]` array sitting in BSS-as-data)

---

## Path C status after session 154

- ✅ 107..153 — see prior docs
- ✅ 154 — wmedit Ctrl-Y redo (round-trip with session 153
  undo)
- ⚠️  wmterm input + close — still deferred

wmedit's edit core is now feature-complete by the standards of
a 1990s-era text editor: cursor navigation, selection,
clipboard cut/copy/paste, **search**, **undo/redo**.  The only
common feature still missing is multi-file (file picker /
tabs), which is genuinely structural rather than additive.
