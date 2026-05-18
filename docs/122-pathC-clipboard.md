# Session 136 — Path C phase 29: clipboard

**Goal.** Cross-client copy/paste — the first piece of inter-WM
communication.

Status: **done.** Smoke test (`smoke_wmclip.py`, 4/4 pass):

```
   A: text white px = 48
   B: text white px = 48

=== pixel checks ===
  [OK] A: 'hi' rendered (48 white)
  [OK] B: paste re-populated buffer (48 white)
  [OK] B: 'chars=...' footer text (151 gray)
  [OK] wmd status bar (879/924)
```

Verified flow: type "hi" into wmtype → Ctrl+C → backspace × 2
(clears the buffer) → Ctrl+V → "hi" reappears.  Same window
demonstrates the round-trip via the global clipboard.  Two
separate wmtype windows would work the same way; the kernel
clipboard is a single global slot.

Regression: `smoke_wmtype` still passes — Ctrl+C/V intercepts
sit above the normal printable-key path so plain typing is
unaffected.

---

## Three new pieces

### 1. Kernel-side single-slot buffer (`kernel/clipboard.[ch]`)

```c
#define CLIPBOARD_MAX 4096
int  clipboard_set(const void *src, int len);
int  clipboard_get(void *dst, int cap);
int  clipboard_len(void);
```

One `g_clip` byte buffer, kmalloc'd lazily on the first write.
Subsequent writes reuse the same buffer if the new payload
fits, kfree+kmalloc otherwise.  `len == 0` clears (without
freeing — the common "copy then immediately paste" case
re-uses the warm slot).

`clipboard_get` returns the **total stored length**, not the
bytes-copied count, so callers can detect truncation by
comparing against `cap`.

Locking: single global counter accessed only from the syscall
handler with preemption disabled.  No spinlock today; an SMP
graphics future will want one.

### 2. Two syscalls (102 / 103) + libuser wrappers

Slots 102/103 because 101 was taken by Path D's `SYS_RENAME`.

```c
int sys_clipboard_set(const void *buf, int len);
int sys_clipboard_get(void *buf, int cap);
```

No auth — any task can read or write.  Same model as the X11
PRIMARY/CLIPBOARD selections; the WM doesn't gate access, the
*convention* is that apps only write on explicit user gesture.

### 3. libwm convenience wrappers

```c
int wm_clipboard_set(const void *buf, int len) {
    return sys_clipboard_set(buf, len);
}
int wm_clipboard_get(void *buf, int cap) {
    return sys_clipboard_get(buf, cap);
}
```

Client code that already links libwm doesn't need to drag in
libuser just for clipboard.

---

## wmtype binding

wmtype intercepts two control codes that arrive on
`WM_EV_KEY`:

```c
if (k == 0x03) {                     /* Ctrl+C — copy */
    wm_clipboard_set(buf, len);
} else if (k == 0x16) {              /* Ctrl+V — paste */
    char pb[BUF_MAX];
    int pn = wm_clipboard_get(pb, (int)sizeof(pb));
    if (pn > 0) {
        if (pn > BUF_MAX - len - 1) pn = BUF_MAX - len - 1;
        for (int i = 0; i < pn; i++) buf[len++] = pb[i];
    }
}
```

The kernel USB-HID layer translates Ctrl+letter into the
classic ASCII control codes (`c - 'a' + 1`), so Ctrl+C arrives
as `0x03` and Ctrl+V as `0x16` — no separate keycode-with-
modifier-flags wiring needed for this case.

Other clients are free to NOT implement Ctrl+C/V — `wmterm`
deliberately doesn't, because Ctrl+C is the terminal interrupt
signal that goes through to the shell.  Each client decides
its own copy/paste UI.

---

## Why this is small

The clipboard pattern is well-trodden: one buffer, two ops,
last-writer-wins.  We're not chasing the X11 selection model
(which has multiple selection atoms, request/notify protocols,
content-type negotiation, MIME) — that's three orders of
magnitude more code than this OS warrants.  Plain bytes,
single slot.  When something needs MIME-type negotiation,
that's a future session.

Future extensions someone might land later:
- **Copy event back to the source.**  Today only the source
  knows it set the clipboard; no other client gets notified.
  An advisory `WM_EV_CLIPBOARD_CHANGED` broadcast would let
  clients (e.g. a clipboard manager app) react.
- **Multiple kinds.**  Today's slot is byte-typed.  A
  type-tagged version would let one app put a PNG image, a
  text fallback, and an alternate format all at once.
- **History.**  Many desktops keep the last N clipboard
  contents.  Trivial extension of the single-slot layout.

None of those are needed for what session 136 set out to do.

---

## Files touched

- `kernel/clipboard.h`, `kernel/clipboard.c` — new (45 lines
  combined): single-slot byte buffer + 3 entry points
- `kernel/syscall.h` — `SYS_CLIPBOARD_SET` (102) +
  `SYS_CLIPBOARD_GET` (103)
- `kernel/syscall.c` — dispatch cases + `#include "clipboard.h"`
- `user/libuser.h`, `user/libuser.c` — mirror constants +
  `sys_clipboard_set` / `sys_clipboard_get` inline-asm wrappers
- `libwm/libwm.h`, `libwm/libwm.c` — `wm_clipboard_set` /
  `wm_clipboard_get` thin forwarders
- `user/wmtype.c` — Ctrl+C copies the current buffer, Ctrl+V
  pastes clipboard contents at the cursor
- `smoke_wmclip.py` — new harness, 4 pixel checks
- `docs/122-pathC-clipboard.md` — this file

kernel.bin: 135344 → 143536 (+8 KiB net; most is the
unrelated rendering-corner work that landed on main between
sessions 135 and 136, plus ~250 B for the clipboard
implementation itself).
wmtype.bin: 5200 → 5632 (+432 B for the Ctrl+C/V handling).

---

## Path C status after session 136

- ✅ 107..135 — see prior docs
- ✅ 136 — clipboard

Now any two clients can exchange a chunk of bytes.  The
foundation for richer inter-process gestures (drag-and-drop,
selection-tracking, clipboard managers) is in place.
