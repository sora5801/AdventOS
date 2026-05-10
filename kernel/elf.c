#include "elf.h"
#include "fs.h"
#include "paging.h"
#include "pmm.h"
#include "kprintf.h"
#include "string.h"
#include "dyld.h"

#define USER_STACK_VA   0x40100000u
#define USER_MIN_VA     0x40000000u   /* refuse to map kernel-area VAs */

static int validate_ehdr(const struct elf32_ehdr *h) {
    if (h->ident[0] != 0x7F || h->ident[1] != 'E' ||
        h->ident[2] != 'L'  || h->ident[3] != 'F')      return -1;
    if (h->ident[4] != ELFCLASS32)                       return -2;
    if (h->ident[5] != ELFDATA2LSB)                      return -3;
    if (h->machine  != EM_386)                           return -4;
    if (h->type     != ET_EXEC)                          return -5;
    if (h->phnum    == 0)                                return -6;
    if (h->phentsize < sizeof(struct elf32_phdr))        return -7;
    return 0;
}

static uint32_t align_down(uint32_t v, uint32_t a) { return v & ~(a - 1); }
static uint32_t align_up  (uint32_t v, uint32_t a) { return (v + (a - 1)) & ~(a - 1); }

int elf_load(int fs_idx, struct elf_load_result *out) {
    /* 1. Pull and validate the ELF header. */
    struct elf32_ehdr eh;
    if (fs_read(fs_idx, 0, &eh, sizeof(eh)) != (int)sizeof(eh)) {
        kputs("elf: short read of ehdr\n");
        return -100;
    }
    int v = validate_ehdr(&eh);
    if (v != 0) {
        kprintf("elf: invalid ELF (validation code %d)\n", v);
        return v;
    }

    /* 2. Build the user address space. */
    uint32_t *user_pd = paging_create_user_pd();
    if (!user_pd) return -200;

    /* 3. Walk program headers, load each PT_LOAD segment. */
    for (uint16_t i = 0; i < eh.phnum; i++) {
        struct elf32_phdr ph;
        uint32_t ph_off = eh.phoff + (uint32_t)i * eh.phentsize;
        if (fs_read(fs_idx, ph_off, &ph, sizeof(ph)) != (int)sizeof(ph)) {
            kputs("elf: short read of phdr\n");
            paging_destroy_user_pd(user_pd);
            return -101;
        }
        if (ph.type != PT_LOAD) continue;
        if (ph.memsz == 0)      continue;

        if (ph.vaddr < USER_MIN_VA) {
            kprintf("elf: PT_LOAD vaddr 0x%x below USER_MIN_VA 0x%x\n",
                    (unsigned)ph.vaddr, (unsigned)USER_MIN_VA);
            paging_destroy_user_pd(user_pd);
            return -102;
        }
        if (ph.filesz > ph.memsz) {
            kputs("elf: filesz > memsz\n");
            paging_destroy_user_pd(user_pd);
            return -103;
        }

        /* Span of pages this segment touches. */
        uint32_t va_start = align_down(ph.vaddr, PAGE_SIZE);
        uint32_t va_end   = align_up  (ph.vaddr + ph.memsz, PAGE_SIZE);

        uint32_t pte_flags = PTE_USER;
        if (ph.flags & PF_W) pte_flags |= PTE_WRITABLE;

        for (uint32_t va = va_start; va < va_end; va += PAGE_SIZE) {
            void *page = pmm_alloc_page();
            if (!page) {
                kputs("elf: out of physical pages\n");
                paging_destroy_user_pd(user_pd);
                return -104;
            }

            /* Zero everything first; bytes the file doesn't cover stay
             * zero (this is how ELF .bss works). */
            memset(page, 0, PAGE_SIZE);

            if (paging_map_in(user_pd, va, (uintptr_t)page, pte_flags) != 0) {
                pmm_free_page(page);
                paging_destroy_user_pd(user_pd);
                return -105;
            }

            /* Does this page overlap the file-backed [vaddr, vaddr+filesz)? */
            uint32_t file_start = ph.vaddr;
            uint32_t file_end   = ph.vaddr + ph.filesz;
            if (va < file_end && va + PAGE_SIZE > file_start) {
                uint32_t copy_va_start = (va > file_start) ? va : file_start;
                uint32_t copy_va_end   = (va + PAGE_SIZE < file_end)
                                       ? va + PAGE_SIZE : file_end;
                uint32_t copy_len = copy_va_end - copy_va_start;
                uint32_t file_off = ph.offset + (copy_va_start - ph.vaddr);
                uint32_t page_off = copy_va_start - va;

                /* The page is identity-mapped under the kernel master
                 * PD, so we can write to its physical address directly. */
                uint8_t *dst = (uint8_t *)page + page_off;
                int rd = fs_read(fs_idx, file_off, dst, copy_len);
                if (rd != (int)copy_len) {
                    kputs("elf: short read of segment data\n");
                    paging_destroy_user_pd(user_pd);
                    return -106;
                }
            }
        }
    }

    /* 3b. Map libc.bin into the new PD at LIBC_VA. The dyld layer
     *     copies bytes from a kernel-side cache; each process gets
     *     its own private physical pages so libc's .data (malloc
     *     state, etc.) is naturally per-process. If libc.bin isn't
     *     in the FS, dyld_map_libc returns 0 silently — programs
     *     that don't use libc trampolines still work. */
    if (dyld_map_libc(user_pd) != 0) {
        kputs("elf: libc map failed\n");
        paging_destroy_user_pd(user_pd);
        return -107;
    }

    /* 4. Allocate a multi-page user stack. Bumped from 1 page to 4
     * pages (16 KiB) in session 36 — TLS handshake code allocates
     * 4 KiB record buffers plus call-chain frames; a single-page
     * stack overflows into unmapped memory at 0x400FF000 just below
     * USER_STACK_VA. The previous "1 page is enough for shells and
     * coreutils" assumption stops holding once we add crypto. The
     * stack is allocated as four contiguous pages (NOT contiguous
     * physical pages — they're separately pmm_alloc'd, but VA is
     * contiguous from USER_STACK_VA - 3*PAGE_SIZE up to USER_STACK_VA
     * + PAGE_SIZE). */
    #define USER_STACK_PAGES  4
    #define USER_STACK_BYTES  (USER_STACK_PAGES * PAGE_SIZE)
    /* Stack lives in [USER_STACK_VA - 3*PAGE_SIZE, USER_STACK_VA + PAGE_SIZE);
     * user_esp starts at the high end (USER_STACK_VA + PAGE_SIZE) and
     * grows down. */
    uint32_t stack_va_lo = USER_STACK_VA + PAGE_SIZE - USER_STACK_BYTES;
    void *first_stack_page = 0;
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        void *p = pmm_alloc_page();
        if (!p) {
            paging_destroy_user_pd(user_pd);
            return -300;
        }
        memset(p, 0, PAGE_SIZE);
        uint32_t va = stack_va_lo + (uint32_t)i * PAGE_SIZE;
        if (paging_map_in(user_pd, va, (uintptr_t)p,
                          PTE_USER | PTE_WRITABLE) != 0) {
            pmm_free_page(p);
            paging_destroy_user_pd(user_pd);
            return -301;
        }
        if (i == USER_STACK_PAGES - 1) first_stack_page = p;  /* top page (used by elf_setup_args) */
    }

    out->entry      = eh.entry;
    out->cr3        = (uint32_t)(uintptr_t)user_pd;
    out->user_esp   = USER_STACK_VA + PAGE_SIZE;
    out->stack_phys = (uint32_t)(uintptr_t)first_stack_page;
    out->stack_size = PAGE_SIZE;     /* elf_setup_args only uses top page */
    return 0;
}

void elf_setup_args(struct elf_load_result *r,
                    int argc, const char *const *argv) {
    uint8_t *kbase = (uint8_t *)(uintptr_t)r->stack_phys;
    uint32_t cur_off = r->stack_size;
    uint32_t cur_va  = USER_STACK_VA + r->stack_size;

    uint32_t str_va[16];
    if (argc > 16) argc = 16;

    /* 1. Strings, in reverse so argv[0]'s string ends up lowest. */
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = 0;
        while (argv[i][len]) len++;
        len++;                              /* + NUL */
        cur_off -= (uint32_t)len;
        cur_va  -= (uint32_t)len;
        for (size_t j = 0; j < len; j++) kbase[cur_off + j] = (uint8_t)argv[i][j];
        str_va[i] = cur_va;
    }

    /* 2. Align down to 4 bytes for the pointer table. */
    cur_off &= ~3u;
    cur_va  &= ~3u;

    /* 3. argv terminator (NULL). */
    cur_off -= 4;
    cur_va  -= 4;
    *(uint32_t *)(kbase + cur_off) = 0;

    /* 4. argv[argc-1] ... argv[0]. */
    for (int i = argc - 1; i >= 0; i--) {
        cur_off -= 4;
        cur_va  -= 4;
        *(uint32_t *)(kbase + cur_off) = str_va[i];
    }

    /* 5. argc. */
    cur_off -= 4;
    cur_va  -= 4;
    *(uint32_t *)(kbase + cur_off) = (uint32_t)argc;

    r->user_esp = cur_va;
}
