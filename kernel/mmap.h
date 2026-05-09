#ifndef ADVENTOS_MMAP_H
#define ADVENTOS_MMAP_H

#include "../include/types.h"
#include "isr.h"

/*
 * mmap support (session 24).
 *
 * Each user task gets a small per-task table of mmap regions
 * (struct task_mmap, in task.h). sys_mmap registers a region —
 * marking the VA range as "this VA is backed by file `fs_idx` at
 * offset `file_offset`" — but allocates NO physical pages. The
 * actual memory comes from the page fault handler the first time
 * the user touches each page in the range:
 *
 *   user reads at *p   →   #PF → mmap_handle_fault(r, cr2)
 *                                    └─ pmm_alloc_page()
 *                                    └─ zero the page
 *                                    └─ fs_read into it
 *                                    └─ paging_map_in(user_pd, page, USER|RW)
 *                                    └─ invlpg(page)
 *                                    └─ return; CPU retries the load
 *
 * The same flow handles writes — we map the page R/W and the user's
 * write hits the freshly-allocated physical page. There's no
 * write-back to the file: this is an effective MAP_PRIVATE, with
 * the file content as the lazy seed.
 */

void *mmap_register  (uint32_t fs_idx, uint32_t file_offset,
                      uint32_t length);
int   mmap_unregister(uint32_t va_start, uint32_t length);

/* Try to satisfy a page fault as a demand-load from an mmap region.
 * Returns 0 if the fault was handled (caller should iret normally),
 * -1 if cr2 is not in any of the current task's mmap regions
 * (caller should fall through to the existing panic path). */
int   mmap_handle_fault(struct registers *r, uint32_t cr2);

#endif
