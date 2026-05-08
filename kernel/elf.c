#include "elf.h"
#include "fs.h"
#include "paging.h"
#include "pmm.h"
#include "kprintf.h"
#include "string.h"

#define USER_STACK_VA   0x40100000u
#define USER_MIN_VA     0x40000000u   /* refuse to map kernel-area VAs */
#define PAGE_SIZE       4096u

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

    /* 4. Allocate a one-page user stack. */
    void *stack_page = pmm_alloc_page();
    if (!stack_page) {
        paging_destroy_user_pd(user_pd);
        return -300;
    }
    memset(stack_page, 0, PAGE_SIZE);
    if (paging_map_in(user_pd, USER_STACK_VA, (uintptr_t)stack_page,
                      PTE_USER | PTE_WRITABLE) != 0) {
        pmm_free_page(stack_page);
        paging_destroy_user_pd(user_pd);
        return -301;
    }

    out->entry    = eh.entry;
    out->cr3      = (uint32_t)(uintptr_t)user_pd;
    out->user_esp = USER_STACK_VA + PAGE_SIZE;
    return 0;
}
