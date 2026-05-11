# Session 61 — Real text input: text-field widget + Calc + Notepad

**Goal:** the session-57 window manager could draw, drag, focus, and dispatch click events but had no text-input primitive — every "app" was either static text (Hello, Clock) or pure pointer manipulation (Paint, Tasks). Session 61 fills the gap with a reusable text-field widget and two apps that exercise it:

- **Calculator** — single-line expression input, click-or-type any digit/operator/paren, hit Enter (or click `=`) to evaluate. Recursive-descent integer parser with precedence-correct `* /` then `+ -`.

- **Notepad** — multi-line scratchpad with word-wrap rendering and a Save button that writes the current buffer to `/notepad.txt`. Re-reading the file is the t44 selftest's "ground truth" that the typed bytes survived the input → buffer → disk round trip.

The text-field widget itself is the load-bearing piece — both apps embed one, and a future Terminal / IRC client / search bar would too without duplicating the rendering, key dispatch, or scroll logic.

`[t44]` selftest, 5/5 PASS:

```
[t44] GUI text input: Calc evaluator + Notepad save-to-disk
  PASS  Calc: keyboard injected '12+34' + ENTER, eval = 46
  PASS  Notepad: Save click wrote 12-byte buffer to /notepad.txt
  PASS  /notepad.txt exists after Save click
  PASS  file size matches the 12-char buffer length
  PASS  file content == 'hi from gui!' (typed via text field)
```

Selftest total: **134 PASS, 0 FAIL.** Same RSA / TLS / SSH / debugger / NTP suites as before, plus the new t44 set.

---

## 1. The text-field widget

Single struct, all primitives non-allocating, buffer is caller-owned:

```c
#define TF_MAX 1024

struct text_field {
    char *buf;          /* not owned; caller supplies storage */
    int   cap;          /* sizeof(buf) — buf[cap-1] is the NUL slot */
    int   len;          /* current string length, not counting NUL */
};

static void tf_init     (struct text_field *, char *buf, int cap);
static void tf_clear    (struct text_field *);
static void tf_append   (struct text_field *, char c);
static void tf_backspace(struct text_field *);
static int  tf_handle_key(struct text_field *, int key);
static void tf_draw     (const struct text_field *,
                         int x, int y, int w, int h,
                         int focused, int frame);
```

### Why caller-owned storage

A "the widget owns its own buffer" design would force either (a) one fixed size for every field everywhere, or (b) a malloc on widget creation. We took option (c): the field holds a `char *` + `cap`, and apps wire in their own backing.

That lets Calc point a 64-byte buffer at the same widget code that Notepad points a 1024-byte buffer at, with zero overhead. It also means widgets can be embedded in `static` storage for predictable boot behavior.

### `tf_handle_key` — what counts as a printable

```c
if (key == '\n' || key == '\r')      return 1;       /* submit */
if (key == 0x08 || key == 0x7F) { tf_backspace(...); return 0; }
if (key >= 0x20 && key < 0x7F) { tf_append(...);     return 0; }
return 0;
```

Backspace gets both 0x08 (Ctrl-H, raw-mode emit) and 0x7F (DEL, what some terminals send) — keeps the field's behavior consistent regardless of which keyboard convention the underlying TTY uses. Enter accepts both LF and CR for the same cross-keyboard reason.

The "printable" gate is the ASCII-7-bit range. Multi-byte UTF-8 would need a separate path (parse on multi-byte boundaries); for an i386 hobby kernel we keep it ASCII.

The return value matters: it's `1` if the key was ENTER. Callers decide what "submit" means — Calc calls `calc_evaluate()`, Notepad just appends the newline as a literal character because for a multi-line editor ENTER is "new paragraph," not "I'm done editing."

### Rendering: focus + scroll + blinking cursor

```c
fill_rect(x, y, w, h, 0xFFFFFF);
rect_outline(x, y, w, h, focused ? 0x4080E0 : 0x808080);
if (focused) rect_outline(x+1, y+1, w-2, h-2, ...);   /* double thick */
```

A double border is a clearer focus cue than a single thicker line — it survives any backing color the host theme might pick.

The text scroll-on-overflow keeps the rightmost characters visible:

```c
int char_cap = (w - 8) / FONT_W;
int start = (tf->len + 1 > char_cap) ? tf->len + 1 - char_cap : 0;
for (int i = start; i < tf->len; i++) draw_glyph(...);
```

Without this, a long expression in Calc would render off the right edge with the cursor invisible. With it, the cursor (the blinking `_`) stays anchored at the right margin and the older text scrolls out of view.

Cursor blinks every 15 frames (`frame / 15 & 1`) ≈ 2 Hz at 60 fps — the standard cursor cadence Mac and Windows use. The cursor only draws when focused, so unfocused fields don't strobe in unison and the user's eye snaps to "the field with the live cursor."

---

## 2. The Calculator app

```
┌─────────────────────────┐
│ Calc                  ✕ │  ← title bar (focused = blue)
├─────────────────────────┤
│  ┌─────────────────────┐│
│  │ 12+34_              ││  ← expression text-field (focused)
│  └─────────────────────┘│
│  ┌─────────────────────┐│
│  │ = 46                ││  ← result line
│  └─────────────────────┘│
│  ┌──┐ ┌──┐ ┌──┐ ┌──┐    │
│  │7 │ │8 │ │9 │ │/ │    │
│  └──┘ └──┘ └──┘ └──┘    │
│  ... 4x4 button grid ...│
│  ┌─────────────────────┐│
│  │         =           ││  ← evaluates on click
│  └─────────────────────┘│
└─────────────────────────┘
```

### Evaluator

Classic recursive-descent integer expression parser:

```
expr   := term  (('+' | '-') term)*
term   := factor (('*' | '/') factor)*
factor := number | '-' factor | '(' expr ')'
```

Precedence falls out of the grammar: `expr` handles `+ -` at one level, `term` handles `* /` at a tighter level, so `1+2*3` correctly evaluates to `7` not `9`. Parens recurse into `expr` for arbitrary nesting.

Errors:

- Division by zero → `cp.err = 1`, on-screen "ERROR"
- Unparsed trailing bytes (e.g. `12*` with no second factor) → `cp.err = 1`
- Empty buffer → no eval, no result
- Otherwise → `g_calc_result = v` and the result line shows `= <n>`

Every evaluation also prints `calc: '<expr>' = <result>` (or `= ERROR`) to stdout — that's the t44 selftest's keyboard-input-actually-reached-the-widget witness, captured via a pipe.

### Click vs key paths

Both end up at the same `tf_append` / `calc_evaluate`:

- **Click on a button**: `calc_click` reads the per-cell label from `g_calc_btn[r][c]`, calls `tf_append`. The "C" button calls `tf_clear`; the "=" bar calls `calc_evaluate`.
- **Keystroke**: `calc_key` calls `tf_handle_key`. If that returns 1 (ENTER), evaluate. The `=` ASCII character (when typed) also triggers `calc_evaluate` — a keyboard shortcut for the same op.

The two paths exercise different parts of the WM:

- Click dispatch goes through `handle_mouse` → `win_hit_test` → `w->click(...)` (down-edge only — see §4 below).
- Key dispatch goes through `sys_kbd_poll` → `handle_key` → `win_focused_idx` → `w->key(...)`.

For the selftest both paths run: scripted mouse click focuses Calc's window, then `tty_inject("12+34\n")` fires through the key path.

---

## 3. The Notepad app

A multi-line text area + footer with a green Save button and a character counter.

```
┌─────────────────────────────────────┐
│ Notepad                           ✕ │
├─────────────────────────────────────┤
│ hi from gui!_                       │
│                                     │
│  ...                                │
│                                     │
├─────────────────────────────────────┤
│ [Save]  (12 ch)              SAVED! │  ← transient flash, ~1 s
└─────────────────────────────────────┘
```

### Line-wrap rendering

The buffer is a flat `char[1024]`. The draw path walks it left-to-right, advancing column count, and emits a "newline" both on explicit `\n` AND on hitting the right margin (soft wrap):

```c
for (int i = 0; i < tf->len; i++) {
    char c = tf->buf[i];
    if (c == '\n') { row++; col = 0; continue; }
    if (col >= char_cap) { row++; col = 0; }
    if (row >= max_rows) break;
    draw_glyph(bx + 4 + col * FONT_W, by + 2 + row * FONT_H, c, fg);
    col++;
}
```

Beyond `max_rows` the text scrolls off the bottom — no smart "follow the cursor" scrolling, just visible-from-top with a hard cap. That's fine for a few-screenful scratchpad; a real editor would track a viewport offset.

### Save: `sys_fs_write` to `/notepad.txt`

```c
int rc = sys_fs_write("/notepad.txt", buf, len);
printf("notepad: saved %d bytes to /notepad.txt (rc=%d)\n", len, rc);
g_notepad_saved_msg_frames = 60;        /* show "SAVED" for 1 s */
```

The transient "SAVED!" overlay is a one-shot frame counter — decremented in the draw path, drawn while > 0, hidden once it ticks down. No timers, no callbacks; the existing 60-fps redraw loop handles the animation by free.

The t44 selftest validates the save by reading `/notepad.txt` back after `gui.elf` exits. That's the round-trip ground truth — the per-frame printf could in principle be lying, but the on-disk bytes can't be.

---

## 4. The held-click bug + per-window `wants_drag` flag

The very first test run of session 61 caught a real bug. The selftest scripted a Save click at frame 165 and a release at frame 175 — 10 frames held. The output read:

```
notepad: saved 12 bytes to /notepad.txt (rc=0)
notepad: saved 12 bytes to /notepad.txt (rc=0)
notepad: saved 12 bytes to /notepad.txt (rc=0)
...
```

Ten times. Because `handle_mouse` dispatched the click handler **on every frame the button was held**, not just on the down-edge — Paint needs that behavior so dragging the mouse paints a continuous stroke, but a Save button does NOT.

Fix: a per-window `wants_drag` flag (default 0, "button style"). The "click while held" dispatch only fires for windows that opt in:

```c
} else if ((btns & 0x1) && g_drag_idx < 0) {
    int idx = win_focused_idx();
    if (idx >= 0) {
        struct window *w = &g_wins[idx];
        if (w->click && w->wants_drag && my > w->y + TITLE_H) {
            /* ... dispatch ... */
        }
    }
}
```

Paint sets `wants_drag = 1` after `spawn_window`. Calc and Notepad leave it 0 — their click handlers only see the down-edge, which is what a button-style widget needs.

This is the kind of bug that lives latent until someone wires up a stateful UI primitive. It's exactly why feature-driven session work pulls bugs into the open.

---

## 5. Driving keyboard input from the selftest

The WM polls `sys_kbd_poll` each frame. To test text input without a physical keyboard, we use the existing `tty_inject` syscall (session 8) to push bytes directly into the kernel keyboard ring — `sys_kbd_poll` reads from the same ring, so injected bytes look indistinguishable from real keystrokes.

The script table gains a `const char *keys` field:

```c
struct script_step {
    int at_frame;
    int x, y, btns;
    const char *keys;
};

static struct script_step g_script[] = {
    /* frame  x    y   btns  keys                  */
    {  80,   100, 230,  1,   0  },                   /* click Calc */
    {  95,   100, 230,  0,   "12+34" },              /* type expression */
    { 105,   100, 230,  0,   "\n" },                 /* ENTER = evaluate */
    ...
```

`script_apply` tracks the highest applied step in `g_last_script_idx` so each row's keystrokes fire exactly once — otherwise the naive "find latest step with at_frame ≤ frame" approach would re-inject the same string sixty times a second.

```c
if (last != g_last_script_idx) {
    for (int i = g_last_script_idx + 1; i <= last; i++) {
        const char *keys = g_script[i].keys;
        if (keys) tty_inject(keys, strlen(keys));
    }
    g_last_script_idx = last;
}
```

The same per-step "fire once" idiom is what real input systems use to convert continuous state to discrete events.

---

## 6. The selftest

```
[t44] GUI text input: Calc evaluator + Notepad save-to-disk
  PASS  Calc: keyboard injected '12+34' + ENTER, eval = 46
  PASS  Notepad: Save click wrote 12-byte buffer to /notepad.txt
  PASS  /notepad.txt exists after Save click
  PASS  file size matches the 12-char buffer length
  PASS  file content == 'hi from gui!' (typed via text field)
```

The choreography:

```
fork (gui.elf selftest) → pipe-capture stdout
  ... gui.elf runs its 200-frame scripted timeline ...
  ... Calc keys + ENTER → "calc: '12+34' = 46\n"
  ... Notepad keys + Save click → "notepad: saved 12 bytes to /notepad.txt\n"
  exit 0
wait()
grep captured for the two app-witness strings → 2 assertions
sys_open("/notepad.txt"); sys_read(...)  → 3 more assertions
  (exists, size == 12, content == "hi from gui!")
```

The on-disk check is the test's anchor — the captured printfs could in principle reflect optimized-out code paths or hand-rolled instrumentation lying about state. A different process reading actual bytes off the filesystem is irrefutable: those bytes can only get there if the entire chain worked, from keyboard ring → `sys_kbd_poll` → focused window dispatch → `notepad_key` → `tf_handle_key` → buffer state → `sys_fs_write`.

---

## 7. Touched files

- `user/gui.c` — text-field widget primitives; Calc + Notepad apps; per-window `wants_drag` flag; keystroke injection in script_apply; two new spawn_window calls; bumped selftest max_frames 80 → 200.
- `user/sh.c` — `[t44]` selftest.

No kernel changes. The WM is entirely userspace — the only system surfaces it needs (framebuffer mmap, keyboard ring poll, mouse state, tty_inject for tests, fs_write for notepad save) were already plumbed by previous sessions. That's a nice observation: getting from a CLI hobby OS to an interactive GUI text editor required adding zero new syscalls in this session.

## 8. Out of scope

- **Mid-string cursor movement (← / →)** — would require buffering escape sequences out of the keyboard ring (the WM currently treats a lone `0x1B` as ESC = quit) and tracking an insertion-point separate from `len`.
- **Selection + clipboard** — needs a "marked range" model and at minimum a kernel-side clipboard.
- **Mouse-driven cursor placement** — clicking inside the text field could position the cursor; would need a hit-test in `tf_draw`.
- **Save As / Open** — Notepad currently has one fixed save target. A picker dialog would need its own window + a tiny file-listing widget.

Most realistic next session: arrow-key cursor movement + click-to-position. Both touch the same "real interactive editing" frontier and unlock a real Terminal app down the line.
