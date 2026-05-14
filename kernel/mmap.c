#include "mmap.h"
#include "task.h"
#include "fs.h"
#include "pmm.h"
#include "paging.h"
#include "signal.h"
#include "string.h"
#include "kprintf.h"

#define PAGE_4K  4096u

void *mmap_register(uint32_t fs_idx, uint32_t file_offset, uint32_t length) {
    struct task *t = task_current();
    if (length == 0) return (void *)0;

    uint32_t aligned = (length + PAGE_4K - 1u) & ~(PAGE_4K - 1u);
    if (t->mmap_brk + aligned > USER_MMAP_MAX) return (void *)0;

    int slot = -1;
    for (int i = 0; i < TASK_MMAP_MAX; i++) {
        if (!t->mmaps[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return (void *)0;

    uint32_t va = t->mmap_brk;
    t->mmap_brk += aligned;

    t->mmaps[slot].in_use      = 1;
    t->mmaps[slot].va_start    = va;
    t->mmaps[slot].va_end      = va + aligned;
    t->mmaps[slot].fs_idx      = (int)fs_idx;
    t->mmaps[slot].file_offset = file_offset;
    t->mmaps[slot].file_length = length;
    return (void *)(uintptr_t)va;
}

int mmap_unregister(uint32_t va_start, uint32_t length) {
    struct task *t = task_current();
    uint32_t aligned = (length + PAGE_4K - 1u) & ~(PAGE_4K - 1u);
    uint32_t va_end  = va_start + aligned;

    int found = 0;
    for (int i = 0; i < TASK_MMAP_MAX; i++) {
        struct task_mmap *m = &t->mmaps[i];
        if (!m->in_use) continue;
        if (m->va_start != va_start || m->va_end != va_end) continue;
        found = 1;

        /* Walk pages, free physical for any that got faulted in.
         * paging_translate returns 0 for unmapped pages. */
        for (uint32_t va = m->va_start; va < m->va_end; va += PAGE_4K) {
            uintptr_t phys = paging_translate(va);
            if (phys != 0) {
                paging_unmap(va);
                pmm_free_page((void *)phys);
            }
        }
        m->in_use = 0;
        break;
    }
    return found ? 0 : -1;
}

static struct task_mmap *find_region(struct task *t, uint32_t va) {
    for (int i = 0; i < TASK_MMAP_MAX; i++) {
        if (!t->mmaps[i].in_use) continue;
        if (va >= t->mmaps[i].va_start && va < t->mmaps[i].va_end) {
            return &t->mmaps[i];
        }
    }
    return (struct task_mmap *)0;
}

int mmap_handle_fault(struct registers *r, uint32_t cr2) {
    struct task *t = task_current();
    if (!t || !t->is_user) return -1;

    struct task_mmap *m = find_region(t, cr2);
    if (!m) return -1;

    /* User-mode fault on a mapped region with the protection-violation
     * bit set means the PTE is already present but the access wasn't
     * allowed — we map RW for now, so this shouldn't happen. Bail
     * to the panic path so it gets noticed if the model changes. */
    if (r->err_code & 1) return -1;

    uint32_t va_page = cr2 & ~(PAGE_4K - 1u);

    /* Session 71: RSS cap check before page alloc. Returning -1 from
     * the fault handler routes through isr.c's default-case panic by
     * design, but we want SIGSEGV-shaped delivery instead — the task
     * can be killed cleanly. Post SIGSEGV pending, then return 0
     * (claim we handled it). signal_check_and_deliver at the iret
     * tail picks it up; the task's signal default for SIGSEGV is to
     * terminate. The unmapped page stays unmapped — fine, the task
     * is dying. */
    if (t->max_rss_pages && t->cur_rss_pages >= t->max_rss_pages) {
        t->sig_pending |= (1u << SIGSEGV);
        return 0;
    }

    /* Allocate + zero a fresh page. Page is identity-mapped under the
     * kernel master PD so we can fill it via its physical address. */
    void *page = pmm_alloc_page();
    if (!page) return -1;
    t->cur_rss_pages++;     /* session 71 RSS count */
    for (int i = 0; i < (int)PAGE_4K / 4; i++) ((uint32_t *)page)[i] = 0;

    /* Fill the file-backed portion. The mapping covers
     * [va_start, va_end); the FILE part covers
     * [va_start, va_start + file_length). For the page at va_page
     * the file slice is the intersection. */
    uint32_t page_off  = va_page - m->va_start;
    if (page_off < m->file_length) {
        uint32_t take = m->file_length - page_off;
        if (take > PAGE_4K) take = PAGE_4K;
        int rd = fs_read(m->fs_idx,
                         m->file_offset + page_off,
                         page, take);
        if (rd < 0) {
            pmm_free_page(page);
            return -1;
        }
    }

    /* Install in the user PD with USER + WRITABLE. Then invlpg —
     * paging_map_in only invlpg's when targeting the kernel master
     * PD; we're writing the active user PD so the TLB flush is on
     * us. */
    if (paging_map_in((uint32_t *)(uintptr_t)t->cr3, va_page,
                      (uintptr_t)page,
                      PTE_USER | PTE_WRITABLE) != 0) {
        pmm_free_page(page);
        return -1;
    }
    __asm__ volatile ("invlpg (%0)" :: "r"(va_page) : "memory");
    return 0;
}
