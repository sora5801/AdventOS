/*
 * wm.c — kernel-side WM registry (session 112 / Path C phase 6).
 *
 * Hosts the bound-WM pointer, a small dynamic table of client
 * windows, and a state-per-slot machine the WM drains via SYS_WM_POLL.
 *
 * Sharing model
 *
 *   client task  ──── owns(client_va) ───►   ┌──────────────────┐
 *                                            │ N physical pages │
 *   WM task     ──── owns(wmd_va)    ───►    └──────────────────┘
 *
 * Each window record remembers both VAs and the per-page physical
 * frame addresses, so we can unmap from either PD on the way out.
 *
 * Lifecycle (state column shows the per-slot state machine)
 *
 *   slot = EMPTY
 *     client SYS_WM_CREATE  → alloc pages, dual-map, slot = OPEN_PENDING
 *     WM SYS_WM_POLL        → return op=1, slot = LIVE
 *     client SYS_WM_DESTROY → unmap client side, slot = DESTROY_PENDING
 *     WM SYS_WM_POLL        → return op=2, unmap WM side, free pages,
 *                             slot = EMPTY
 *
 * task_exit short-circuits: if the dying task is the WM, walk all
 * slots and pull the WM-side mapping out of its PD without freeing
 * the pages.  If the dying task is a client, mark its slots
 * DESTROY_PENDING so the WM finds them on its next poll.
 *
 * The static .bss footprint is just two pointers (g_wm_owner + g_state)
 * to keep the kernel image under the VGA-RAM ceiling at 0xA0000.
 * Everything else lives in kmalloc-allocated storage created on
 * first SYS_WM_BIND.
 *
 * Locking: all entry points run from the syscall handler with
 * preemption disabled.  No locks today.  SMP-graphics later will
 * want a spinlock around g_state->windows[].
 */

#include "wm.h"
#include "pmm.h"
#include "paging.h"
#include "task.h"
#include "kmalloc.h"
#include "kprintf.h"

/* Slot states. */
#define WM_SLOT_EMPTY            0
#define WM_SLOT_OPEN_PENDING     1   /* freshly created, WM not yet told  */
#define WM_SLOT_LIVE             2   /* WM has it in its window list      */
#define WM_SLOT_DESTROY_PENDING  3   /* client gone, WM not yet told      */

struct wm_window {
    uint32_t  state;
    uint32_t  id;
    uint32_t  owner_pid;
    uint32_t  client_va;
    uint32_t  wmd_va;
    uint32_t  w, h;
    char      title[32];
    uint32_t  n_pages;
    uint32_t *pages;     /* kmalloc'd at create */

    /* Session 113 — per-window event ring.  Allocated lazily on the
     * first wm_push_event so windows that never see input cost zero
     * extra heap. */
    struct sys_wm_event *events;
    uint32_t             ev_head;   /* read */
    uint32_t             ev_tail;   /* write */
    uint32_t             ev_size;
};

struct wm_state {
    struct wm_window windows[WM_MAX_WINDOWS];
    uint32_t         next_id;
};

/* Hot path globals — tiny to keep .bss footprint low. */
static struct task    *g_wm_owner;
static struct wm_state *g_state;

/* Session 135 — Alt+Tab counter.  Bumped by usb_hid when the user
 * presses Alt+Tab; decremented by wmd's per-frame poll.  Capped at
 * a small max so a stuck-keyboard never overflows. */
static volatile unsigned int g_alttab_pending;

/* Session 147 — workspace switch request.  usb_hid intercepts
 * Alt+1..4 and posts the workspace index here; wmd polls once
 * per frame.  -1 = no pending switch; set by post, cleared by
 * poll.  No queue — only the most-recent request matters. */
static volatile int g_workspace_pending = -1;

/* Session 143 — toast-notification ring.  Apps post short status
 * text (e.g. "saved /tmp/foo (123 B)") via SYS_WM_NOTIFY; wmd
 * drains via SYS_WM_POLL_NOTIFY each frame and pops up a toast.
 * Cap is small — toasts are ephemeral and lossy is fine. */
#define WM_NOTIFY_MAX        8
#define WM_NOTIFY_TEXT_MAX  64
struct wm_notify_slot {
    char text[WM_NOTIFY_TEXT_MAX];
    int  len;
};
static struct wm_notify_slot g_notify_ring[WM_NOTIFY_MAX];
static int g_notify_head;   /* next write */
static int g_notify_tail;   /* next read */

static int  ensure_state(void) {
    if (g_state) return 0;
    g_state = (struct wm_state *)kmalloc(sizeof(*g_state));
    if (!g_state) return -1;
    /* Zero everything explicitly: kmalloc doesn't promise zeroing,
     * and the slot state machine starts with WM_SLOT_EMPTY = 0. */
    unsigned char *p = (unsigned char *)g_state;
    for (uint32_t i = 0; i < sizeof(*g_state); i++) p[i] = 0;
    g_state->next_id = 1;
    return 0;
}

static struct wm_window *find_slot_by_id(uint32_t id) {
    if (!g_state) return 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        struct wm_window *w = &g_state->windows[i];
        if (w->state != WM_SLOT_EMPTY && w->id == id) return w;
    }
    return 0;
}

static struct wm_window *find_empty_slot(void) {
    if (!g_state) return 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (g_state->windows[i].state == WM_SLOT_EMPTY) {
            return &g_state->windows[i];
        }
    }
    return 0;
}

/* Manually clear a PTE in an arbitrary PD without freeing the
 * physical page.  paging.h doesn't expose this — but we built the
 * mapping ourselves with paging_map_in so we know the structure. */
static void unmap_from_pd_keep_phys(uint32_t *pd, uintptr_t va) {
    if (!pd) return;
    uint32_t pd_i = (uint32_t)(va >> 22);
    uint32_t pt_i = (uint32_t)((va >> 12) & 0x3FFu);
    if (!(pd[pd_i] & PTE_PRESENT)) return;
    uint32_t *pt = (uint32_t *)(uintptr_t)(pd[pd_i] & ~0xFFFu);
    pt[pt_i] = 0;
}

static void free_slot_pages(struct wm_window *w) {
    if (!w->pages) return;
    for (uint32_t i = 0; i < w->n_pages; i++) {
        if (w->pages[i]) pmm_free_page((void *)(uintptr_t)w->pages[i]);
    }
    kfree(w->pages);
    w->pages = 0;
    w->n_pages = 0;
}

static void free_slot_events(struct wm_window *w) {
    if (w->events) { kfree(w->events); w->events = 0; }
    w->ev_head = w->ev_tail = w->ev_size = 0;
}

/* ---- public API ---------------------------------------------- */

int wm_bind(struct task *t) {
    if (g_wm_owner) return -1;
    if (ensure_state() < 0) return -1;
    g_wm_owner = t;
    /* Window table itself survives a WM rebind, but the wmd-side
     * mapping in the prior WM's PD is gone (its PD got torn down).
     * For simplicity, drop any surviving LIVE windows — clients
     * will need to re-create after a WM restart. */
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        struct wm_window *w = &g_state->windows[i];
        if (w->state != WM_SLOT_EMPTY) {
            free_slot_pages(w);
            free_slot_events(w);
            w->state = WM_SLOT_EMPTY;
        }
    }
    return 0;
}

int wm_create_window(struct task *client, struct sys_wm_create *args) {
    if (!client || !args) return -1;
    if (!g_wm_owner)      return -1;     /* no WM bound */
    if (!g_state)         return -1;
    if (args->w == 0 || args->h == 0) return -1;
    if (args->w > 1024 || args->h > 1024) return -1;

    uint64_t bytes = (uint64_t)args->w * (uint64_t)args->h * 4ull;
    if (bytes > (uint64_t)WM_MAX_PAGES_PER_WIN * PAGE_SIZE) return -1;
    uint32_t n_pages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    if (n_pages == 0 || n_pages > WM_MAX_PAGES_PER_WIN) return -1;

    struct wm_window *slot = find_empty_slot();
    if (!slot) return -1;

    slot->pages = (uint32_t *)kmalloc(sizeof(uint32_t) * n_pages);
    if (!slot->pages) return -1;
    for (uint32_t i = 0; i < n_pages; i++) slot->pages[i] = 0;

    /* Session 122 — per-task VA bump allocator.  next_wm_va starts
     * at 0 (sentinel) and the first allocation sets it to the base.
     * Each subsequent SYS_WM_CREATE in the same task advances by
     * WM_MAX_PAGES_PER_WIN*PAGE_SIZE = 1 MiB so two windows in the
     * same client never share VA space.  WM side still uses a per-
     * slot stable VA range. */
    if (client->next_wm_va == 0) client->next_wm_va = WM_SURFACE_VA_BASE;
    uint32_t client_va = client->next_wm_va;
    int slot_idx = (int)(slot - g_state->windows);
    uint32_t wmd_va = WM_SURFACE_VA_BASE
           + (uint32_t)slot_idx * WM_MAX_PAGES_PER_WIN * PAGE_SIZE;

    uint32_t *client_pd = (uint32_t *)(uintptr_t)client->cr3;
    uint32_t *wmd_pd    = (uint32_t *)(uintptr_t)g_wm_owner->cr3;
    uint32_t  flags     = PTE_USER | PTE_WRITABLE;
    for (uint32_t i = 0; i < n_pages; i++) {
        void *p = pmm_alloc_page();
        if (!p) goto rollback;
        slot->pages[i] = (uint32_t)(uintptr_t)p;
        unsigned char *zp = (unsigned char *)p;
        for (int k = 0; k < PAGE_SIZE; k++) zp[k] = 0;
        if (paging_map_in(client_pd, client_va + i * PAGE_SIZE,
                          (uintptr_t)p, flags) != 0) goto rollback;
        if (paging_map_in(wmd_pd,    wmd_va    + i * PAGE_SIZE,
                          (uintptr_t)p, flags) != 0) goto rollback;
    }

    slot->id        = g_state->next_id++;
    slot->owner_pid = client->id;
    slot->client_va = client_va;
    slot->wmd_va    = wmd_va;
    slot->w         = args->w;
    slot->h         = args->h;
    slot->n_pages   = n_pages;
    for (int i = 0; i < 31; i++) slot->title[i] = args->title[i];
    slot->title[31] = 0;
    /* Event queue starts empty/unallocated — wm_push_event lazy-
     * allocates on first use (session 113). */
    slot->events    = 0;
    slot->ev_head   = 0;
    slot->ev_tail   = 0;
    slot->ev_size   = 0;
    slot->state     = WM_SLOT_OPEN_PENDING;

    args->id        = slot->id;
    args->pixels_va = client_va;
    /* Session 122 — advance the per-task bump only on successful
     * commit so a failed allocation can be retried at the same VA. */
    client->next_wm_va = client_va
                       + WM_MAX_PAGES_PER_WIN * PAGE_SIZE;
    return 0;

rollback:
    /* Unmap from both PDs and free any pages we allocated. */
    for (uint32_t j = 0; j < n_pages && slot->pages[j]; j++) {
        unmap_from_pd_keep_phys(client_pd, client_va + j * PAGE_SIZE);
        unmap_from_pd_keep_phys(wmd_pd,    wmd_va    + j * PAGE_SIZE);
        pmm_free_page((void *)(uintptr_t)slot->pages[j]);
        slot->pages[j] = 0;
    }
    kfree(slot->pages);
    slot->pages = 0;
    slot->n_pages = 0;
    return -1;
}

int wm_destroy_window(struct task *caller, uint32_t window_id) {
    struct wm_window *w = find_slot_by_id(window_id);
    if (!w) return -1;
    if (!caller || w->owner_pid != caller->id) return -1;
    if (w->state != WM_SLOT_LIVE && w->state != WM_SLOT_OPEN_PENDING) return -1;

    /* Unmap from client PD (caller's current PD). */
    uint32_t *client_pd = (uint32_t *)(uintptr_t)caller->cr3;
    for (uint32_t i = 0; i < w->n_pages; i++) {
        unmap_from_pd_keep_phys(client_pd,
            w->client_va + (uintptr_t)i * PAGE_SIZE);
    }
    w->state = WM_SLOT_DESTROY_PENDING;
    return 0;
}

int wm_pop_message(struct task *caller, struct sys_wm_msg *out) {
    if (g_wm_owner != caller) return -1;
    if (!g_state) return 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        struct wm_window *w = &g_state->windows[i];
        if (w->state == WM_SLOT_OPEN_PENDING) {
            out->op        = 1;
            out->id        = w->id;
            out->owner_pid = w->owner_pid;
            out->w         = w->w;
            out->h         = w->h;
            out->wmd_va    = w->wmd_va;
            for (int j = 0; j < 32; j++) out->title[j] = w->title[j];
            w->state = WM_SLOT_LIVE;
            return 1;
        }
        if (w->state == WM_SLOT_DESTROY_PENDING) {
            out->op        = 2;
            out->id        = w->id;
            out->owner_pid = w->owner_pid;
            out->w = out->h = out->wmd_va = 0;
            for (int j = 0; j < 32; j++) out->title[j] = 0;
            /* Free the WM side and the pages now. */
            uint32_t *wmd_pd = (uint32_t *)(uintptr_t)g_wm_owner->cr3;
            for (uint32_t p = 0; p < w->n_pages; p++) {
                unmap_from_pd_keep_phys(wmd_pd,
                    w->wmd_va + (uintptr_t)p * PAGE_SIZE);
            }
            free_slot_pages(w);
            free_slot_events(w);
            w->state = WM_SLOT_EMPTY;
            return 1;
        }
    }
    return 0;
}

/* ---- Session 113: input routing ------------------------------ */

int wm_push_event(struct task *caller, uint32_t window_id,
                  const struct sys_wm_event *ev) {
    if (!ev || !caller) return -1;
    if (g_wm_owner != caller) return -1;     /* WM-only */
    if (!g_state) return -1;
    struct wm_window *w = find_slot_by_id(window_id);
    if (!w) return -1;
    if (w->state != WM_SLOT_LIVE && w->state != WM_SLOT_OPEN_PENDING) return -1;

    /* Lazy-allocate the event ring on first push.  Saves heap for
     * windows that never receive input. */
    if (!w->events) {
        w->events = (struct sys_wm_event *)
            kmalloc(sizeof(struct sys_wm_event) * WM_EVENT_QUEUE_DEPTH);
        if (!w->events) return -1;
        w->ev_head = w->ev_tail = w->ev_size = 0;
    }

    /* Drop oldest on overflow.  Mouse-move events are the noisy
     * case; a slow client missing the tail isn't fatal — better
     * than the WM blocking on a wedged client. */
    if (w->ev_size >= WM_EVENT_QUEUE_DEPTH) {
        w->ev_head = (w->ev_head + 1u) % WM_EVENT_QUEUE_DEPTH;
        w->ev_size--;
    }
    w->events[w->ev_tail] = *ev;
    w->ev_tail = (w->ev_tail + 1u) % WM_EVENT_QUEUE_DEPTH;
    w->ev_size++;
    return 0;
}

int wm_poll_event(struct task *caller, uint32_t window_id,
                  struct sys_wm_event *out) {
    if (!out || !caller) return -1;
    if (!g_state) return 0;
    struct wm_window *w = find_slot_by_id(window_id);
    if (!w) return -1;
    if (w->owner_pid != caller->id) return -1;
    if (!w->events || w->ev_size == 0) return 0;

    *out = w->events[w->ev_head];
    w->ev_head = (w->ev_head + 1u) % WM_EVENT_QUEUE_DEPTH;
    w->ev_size--;
    return 1;
}

/* Session 135 — usb_hid pushes here on Alt+Tab; wmd pulls once
 * per frame.  No buffering — just a counter, so wmd doesn't see
 * a stale press the very next frame after release. */
void wm_post_alttab(void) {
    if (g_alttab_pending < 8) g_alttab_pending++;
}

int wm_poll_alttab(struct task *caller) {
    if (g_wm_owner != caller) return 0;
    if (g_alttab_pending == 0) return 0;
    g_alttab_pending--;
    return 1;
}

/* Session 147 — workspace switch channel.  Same one-slot pattern
 * as alttab but carries a value 0..NUM_WORKSPACES-1 (= 0..3). */
void wm_post_workspace(int n) {
    if (n >= 0 && n < 4) g_workspace_pending = n;
}

int wm_poll_workspace(struct task *caller) {
    if (g_wm_owner != caller) return -1;
    int n = g_workspace_pending;
    g_workspace_pending = -1;
    return n;
}

/* Session 143 — notification ring entry points. */
int wm_notify_push(const char *text, int len) {
    if (!text || len <= 0) return -1;
    if (len > WM_NOTIFY_TEXT_MAX - 1) len = WM_NOTIFY_TEXT_MAX - 1;
    int next = (g_notify_head + 1) % WM_NOTIFY_MAX;
    if (next == g_notify_tail) return -1;   /* ring full; drop */
    struct wm_notify_slot *s = &g_notify_ring[g_notify_head];
    for (int i = 0; i < len; i++) s->text[i] = text[i];
    s->text[len] = 0;
    s->len = len;
    g_notify_head = next;
    return 0;
}

int wm_notify_pop(char *buf, int cap) {
    if (g_notify_tail == g_notify_head) return 0;     /* empty */
    if (cap <= 0) return 0;
    struct wm_notify_slot *s = &g_notify_ring[g_notify_tail];
    int len = s->len;
    if (len > cap - 1) len = cap - 1;
    for (int i = 0; i < len; i++) buf[i] = s->text[i];
    buf[len] = 0;
    g_notify_tail = (g_notify_tail + 1) % WM_NOTIFY_MAX;
    return len;
}

void wm_on_task_exit(struct task *t) {
    if (!g_state) {
        if (g_wm_owner == t) g_wm_owner = 0;
        return;
    }
    if (g_wm_owner == t) {
        /* WM exiting: pull our mappings out of the dying WM's PD so
         * paging_destroy_user_pd doesn't free shared pages still
         * referenced by clients. */
        g_wm_owner = 0;
        uint32_t *wm_pd = (uint32_t *)(uintptr_t)t->cr3;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            struct wm_window *w = &g_state->windows[i];
            if (w->state == WM_SLOT_EMPTY) continue;
            for (uint32_t p = 0; p < w->n_pages; p++) {
                unmap_from_pd_keep_phys(wm_pd,
                    w->wmd_va + (uintptr_t)p * PAGE_SIZE);
            }
            if (w->state == WM_SLOT_DESTROY_PENDING) {
                free_slot_pages(w);
                w->state = WM_SLOT_EMPTY;
            }
        }
        return;
    }
    /* Client exiting: mark its windows DESTROY_PENDING. Don't touch
     * the client's PD — it's being destroyed by paging_destroy_user_pd
     * right after we return. */
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        struct wm_window *w = &g_state->windows[i];
        if ((w->state == WM_SLOT_LIVE || w->state == WM_SLOT_OPEN_PENDING)
            && w->owner_pid == t->id) {
            w->state = WM_SLOT_DESTROY_PENDING;
        }
    }
}
