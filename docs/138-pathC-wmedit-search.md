# Session 152 — Path C phase 45: wmedit Ctrl-F incremental search

**Goal.** Add an inline find feature to wmedit.  Press Ctrl-F →
the footer turns mustard-yellow with a "Find: <pattern>_" prompt.
Type to refine; matches in the visible body highlight in mustard
yellow; the cursor auto-jumps to the first match.  Enter walks
forward through matches (wrapping).  Esc closes the search bar.

Status: **done.**  Smoke `smoke_wmedit_search.py` (3/3):

```
=== checks ===
  [OK] search bar mustard footer (4126 px)
  [OK] match highlights in body (490 yellow px)
  [OK] wmd status bar alive (832/924)
```

Boot wmd + wmedit, type "abcdef abcdef abcdef", hit Ctrl-F, type
"abc" — three matches highlight in yellow, the footer turns
mustard with the live pattern, the cursor jumps to the first
match.

---

## State

```c
#define SEARCH_PAT_MAX 48
static int  g_search_mode;
static char g_search_pat[SEARCH_PAT_MAX];
static int  g_search_len;

static int find_match(int from) {
    if (g_search_len <= 0 || g_search_len > g_len) return -1;
    int last = g_len - g_search_len;
    /* Forward scan from `from`, then wrap. */
    for (int i = from; i <= last; i++) {
        int ok = 1;
        for (int j = 0; j < g_search_len; j++) {
            if (g_buf[i + j] != g_search_pat[j]) { ok = 0; break; }
        }
        if (ok) return i;
    }
    for (int i = 0; i < from && i <= last; i++) {
        /* same body */
    }
    return -1;
}
```

Linear-scan substring match (no KMP / Boyer-Moore — the 8 KiB
buffer cap makes the simple loop adequate).

---

## Key handling

The Ctrl-F entry point sits at the **top** of the WM_EV_KEY
handler so it takes priority over the existing edit keys:

```c
if (g_search_mode) {
    if (k == 27) { g_search_mode = 0; break; }      /* Esc — close */
    if (k == '\r' || k == '\n') {                    /* Enter — next */
        int m = find_match(g_cur + 1);
        if (m >= 0) g_cur = m;
        break;
    }
    if (k == 0x08 || k == 0x7F) {                   /* Backspace */
        if (g_search_len > 0) g_search_len--;
        g_search_pat[g_search_len] = 0;
        int m = find_match(g_cur);
        if (m >= 0) g_cur = m;
        break;
    }
    if (k >= 0x20 && k <= 0x7E
        && g_search_len < SEARCH_PAT_MAX - 1) {
        g_search_pat[g_search_len++] = (char)k;
        g_search_pat[g_search_len] = 0;
        int m = find_match(g_cur);
        if (m >= 0) g_cur = m;
        break;
    }
    break;                                          /* swallow anything else */
}
/* ...normal mode dispatch below... */
if (k == 0x06) {                                    /* Ctrl-F — open */
    g_search_mode = 1;
    g_search_len = 0;
    g_search_pat[0] = 0;
    sel_clear();
    break;
}
```

Every printable keystroke re-runs `find_match(g_cur)` so the
cursor stays glued to the leading edge of the current pattern.
That's the "incremental" part — feedback on every byte.

---

## Painting

Two paint changes:

1. **Match highlight** in the body — precompute up to 64 visible
   matches before painting, then check each rendered cell:
   ```c
   for (int m = 0; m < n_matches; m++) {
       if (byte >= matches[m]
           && byte < matches[m] + g_search_len) {
           wm_fill_rect(&win, GRID_X + col * CELL_W, y,
                        CELL_W, LINE_H, 0x807030u);
           break;
       }
   }
   ```
   Yellow (0x807030) sits above selection blue in z-order
   (selection paints first, match overrides).

2. **Mustard search bar** in the footer when `g_search_mode`:
   - bg 0x403820 (dark mustard) instead of the usual 0x202830
   - "Find: " label in white
   - live pattern in light yellow (0xFFE080)
   - blinking '_' caret at end of pattern
   - "Esc:close  Enter:next" hint on the right

```c
if (g_search_mode) {
    wm_fill_rect(&win, 0, WIN_H - FOOTER_H, WIN_W, FOOTER_H,
                 0x403820u);
    int fx = draw_str(&sctx, 6, fy, "Find: ", 0xFFFFFFu);
    fx = draw_str(&sctx, fx, fy, g_search_pat, 0xFFE080u);
    if (((caret_phase / 12) & 1) == 0) {
        draw_str(&sctx, fx, fy, "_", 0xFFFFFFu);
    }
    draw_str(&sctx, WIN_W - 200, fy,
             "Esc:close  Enter:next", 0xC0C0A0u);
}
```

---

## What stays out of scope

- **Backward search.**  No Shift+Enter "previous match" yet.
  Forward-with-wrap covers most use cases.
- **Regex.**  Literal substring only.
- **Case-insensitive toggle.**  Byte-exact match.
- **Replace.**  No Ctrl-H / Find-and-replace flow.  The Cut +
  Type pattern (Ctrl-X then type the replacement) works as a
  manual substitute.
- **Persistent highlights across modes.**  Highlights vanish when
  Esc closes the search bar — there's no "keep highlights but
  resume editing."  The cursor position is preserved though.

---

## Files touched

- `user/wmedit.c`:
  - search state (`g_search_mode`, `g_search_pat`,
    `g_search_len`)
  - `find_match(from)` linear-scan matcher
  - Ctrl-F open + search-mode key dispatch at top of
    WM_EV_KEY
  - match-precompute + yellow highlight in body paint
  - mustard search bar replacing the footer status while in
    search mode
- `fs/man/wmedit` — documents Ctrl-F + search-mode keys
- `smoke_wmedit_search.py` — new harness, 3 pixel checks
- `docs/138-pathC-wmedit-search.md` — this file

Sizes:
- kernel.bin: 164016 (unchanged — pure userspace)
- wmedit.bin: 16012 → 17288 (+1276 B for search state +
  matcher + paint additions)

---

## Path C status after session 152

- ✅ 107..151 — see prior docs
- ✅ 152 — wmedit Ctrl-F incremental search
- ⚠️  wmterm input + close — still deferred

wmedit now has 4 of the 5 "real editor" features: cursor
navigation, selection, clipboard, **search**.  Undo (Ctrl-Z)
is the last big one missing.
