#ifndef ADVENTOS_CLIPBOARD_H
#define ADVENTOS_CLIPBOARD_H

/*
 * clipboard.h — kernel-side clipboard (session 136).
 *
 * Single global byte buffer.  Any task can write (clipboard_set)
 * or read (clipboard_get) — there's no per-client clipboard yet,
 * matching the desktop convention that "copy" globally replaces
 * whatever was there before.  Backing store is one kmalloc on
 * first write; subsequent writes reuse it if the new payload
 * fits, kfree+kmalloc otherwise.
 */

#include "../include/types.h"

/* Hard cap.  4 KiB is plenty for typical copy/paste and keeps
 * the per-call copy_from_user / copy_to_user bounded. */
#define CLIPBOARD_MAX 4096

/* Replace clipboard contents with `len` bytes from `src`.  Returns
 * 0 on success, -1 on out-of-memory or len > CLIPBOARD_MAX.
 * len = 0 clears the clipboard. */
int  clipboard_set(const void *src, int len);

/* Copy up to `cap` bytes of the clipboard into `dst`.  Returns
 * the total stored length (NOT bytes copied — caller can detect
 * truncation by comparing).  Returns 0 if clipboard is empty. */
int  clipboard_get(void *dst, int cap);

/* Current stored length without copying anything.  Used by
 * tests that want to assert "clipboard has something" without
 * the allocation noise. */
int  clipboard_len(void);

#endif
