/*
 * clipboard.c — kernel-side single-slot clipboard (session 136).
 *
 * One byte buffer, kmalloc'd on first set.  Locking: every entry
 * point is called from the syscall handler with preemption
 * disabled (single-CPU build), so no spinlock.  Once SMP-graphics
 * becomes a thing, wrap with a small mutex.
 */

#include "clipboard.h"
#include "kmalloc.h"
#include "string.h"

static char    *g_clip;
static int      g_clip_cap;     /* capacity of the kmalloc'd buf */
static int      g_clip_len;     /* bytes currently stored */

int clipboard_set(const void *src, int len) {
    if (len < 0 || len > CLIPBOARD_MAX) return -1;
    if (len == 0) {
        /* Drop to empty without freeing — keeps the buffer warm
         * for the common copy-then-immediately-paste pattern. */
        g_clip_len = 0;
        return 0;
    }
    if (!g_clip || len > g_clip_cap) {
        if (g_clip) kfree(g_clip);
        g_clip = (char *)kmalloc((size_t)len);
        if (!g_clip) { g_clip_cap = 0; g_clip_len = 0; return -1; }
        g_clip_cap = len;
    }
    memcpy(g_clip, src, (size_t)len);
    g_clip_len = len;
    return 0;
}

int clipboard_get(void *dst, int cap) {
    if (cap < 0) return -1;
    if (g_clip_len == 0 || !g_clip) return 0;
    int n = g_clip_len;
    if (n > cap) n = cap;
    if (n > 0) memcpy(dst, g_clip, (size_t)n);
    return g_clip_len;          /* full length, not n — caller spots truncation */
}

int clipboard_len(void) {
    return g_clip_len;
}
