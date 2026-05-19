#ifndef ADVENTOS_WM_H
#define ADVENTOS_WM_H

/*
 * wm.h — kernel-side window manager registry (session 112).
 *
 * Tracks which task is "the WM" (one at a time, set by SYS_WM_BIND),
 * plus a small fixed table of client windows.  Each window owns a
 * chunk of physical pages that are mapped into both the client's PD
 * (at sys_wm_create.pixels_va) and the WM's PD (at the wmd_va we
 * return in the next SYS_WM_POLL).  All accessors are called from
 * the syscall handler with no preemption; no locks needed.
 *
 * Lifecycle:
 *   - client SYS_WM_CREATE → wm_create_window: allocate pages,
 *     map both sides, append an op=1 message to the WM's queue.
 *   - WM SYS_WM_POLL       → wm_pop_message:   pop next queued msg.
 *   - client SYS_WM_DESTROY OR task_exit_current → wm_destroy_window:
 *     queue op=2 (destroy) for the WM, then unmap+free the pages.
 *
 * Cleanup: when the FB-owning WM task exits, wm_release_wm() is
 * called so the WM-bound slot is freed; remaining client windows
 * stay registered but unreachable until a new WM binds (in which
 * case the new WM will see them as a re-flood of op=1 messages).
 */

#include "../include/types.h"
#include "syscall.h"

struct task;

/* Hard caps.
 *
 * These are tight in session 112: each LIVE entry holds 64 bytes of
 * static state plus a kmalloc'd page-list. The kernel .bss already
 * brushes the VGA-RAM boundary at 0xA0000, so we keep the static
 * tables minimal and lean on kmalloc for the variable-sized parts. */
#define WM_MAX_WINDOWS        4
#define WM_MAX_PAGES_PER_WIN  256          /* 256 * 4096 = 1 MiB cap */
#define WM_MSG_QUEUE_DEPTH    8
/* Session 113 — per-window input event queue depth. Big enough for
 * a few hundred ms of 60-Hz mouse-move events if the client polls
 * lazily. */
#define WM_EVENT_QUEUE_DEPTH  32
#define WM_SURFACE_VA_BASE    0x60000000u  /* both client and WM share base */

/* Try to bind `t` as the WM.  Returns 0 on success, -1 if another
 * task is already bound. */
int  wm_bind(struct task *t);

/* SYS_WM_CREATE backend. Allocates pages and maps them into the
 * caller AND into wmd at independent VAs. Returns 0 on success,
 * -1 on any failure (no partial leak — the helper rolls back). */
int  wm_create_window(struct task *client, struct sys_wm_create *args);

/* Session 113 — input routing.  Wmd-side push (caller must be the
 * bound WM); client-side poll (caller must own the window). */
int  wm_push_event(struct task *caller, uint32_t window_id,
                   const struct sys_wm_event *ev);
int  wm_poll_event(struct task *caller, uint32_t window_id,
                   struct sys_wm_event *out);

/* SYS_WM_DESTROY backend. Returns 0 on success, -1 if the window
 * isn't owned by `caller` or doesn't exist. */
int  wm_destroy_window(struct task *caller, uint32_t window_id);

/* Drains one message from the WM queue into `out`. Returns 1 if a
 * message was returned, 0 if the queue is empty, -1 if `caller`
 * isn't the bound WM. */
int  wm_pop_message(struct task *caller, struct sys_wm_msg *out);

/* task_exit calls this on every exiting task so any windows it
 * still owns get torn down and a destroy message queued for wmd. */
void wm_on_task_exit(struct task *t);

/* Session 135 — Alt+Tab routing.  USB-HID calls wm_post_alttab()
 * when the user presses Alt+Tab; wmd polls wm_poll_alttab() once
 * per frame.  Decoupled from the main keyboard ring so the shell
 * (or any other reader) can't intercept the sentinel byte. */
void wm_post_alttab(void);
int  wm_poll_alttab(struct task *caller);

/* Session 143 — toast-notification ring.  Apps push short status
 * messages (<= 63 chars after truncation); wmd drains one per
 * call.  push returns 0 / -1; pop returns the byte length (0 if
 * the ring is empty). */
int  wm_notify_push(const char *text, int len);
int  wm_notify_pop (char *buf, int cap);

#endif
