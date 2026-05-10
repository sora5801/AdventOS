#include "dyld.h"
#include "fs.h"
#include "pmm.h"
#include "paging.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "string.h"

/* Absolute path so fs_open doesn't need a valid task_current()->cwd_dir.
 * dyld_init runs before task_init — pre-init task_current() returns
 * g_tasks[0] whose cwd_dir is uninitialized garbage. */
#define LIBC_PATH    "/libc.bin"
#define LIBC_MAGIC   0x434C4441u
#define PAGE_4K      4096u

static uint8_t  *g_image;
static uint32_t  g_size;
static uint32_t  g_pages;
static int       g_loaded;
static uint32_t  g_version;
static uint32_t  g_export_count;

void dyld_init(void) {
    if (g_loaded) return;

    int fd = fs_open(LIBC_PATH);
    if (fd < 0) {
        kprintf("dyld: %s not in FS — userspace will run without libc\n",
                LIBC_PATH);
        return;
    }

    uint32_t size = fs_size(fd);
    if (size == 0 || size > (1u << 20)) {
        kprintf("dyld: %s has implausible size %u\n", LIBC_PATH, (unsigned)size);
        return;
    }

    /* Cache the entire image in kernel heap. ~6 KiB today. */
    g_image = kmalloc(size);
    if (!g_image) {
        kprintf("dyld: kmalloc(%u) failed\n", (unsigned)size);
        return;
    }
    int rd = fs_read(fd, 0, g_image, (int)size);
    if (rd != (int)size) {
        kprintf("dyld: short read on %s (%d/%u)\n",
                LIBC_PATH, rd, (unsigned)size);
        return;
    }

    /* Header sanity check: first 4 bytes are LIBC_MAGIC ('ADLC'). */
    uint32_t magic = *(uint32_t *)g_image;
    if (magic != LIBC_MAGIC) {
        kprintf("dyld: %s bad magic (got 0x%x, expected 0x%x)\n",
                LIBC_PATH, (unsigned)magic, (unsigned)LIBC_MAGIC);
        return;
    }
    g_version      = *(uint32_t *)(g_image + 4);
    g_export_count = *(uint32_t *)(g_image + 8);

    g_size  = size;
    g_pages = (size + PAGE_4K - 1u) / PAGE_4K;
    g_loaded = 1;
    kprintf("dyld: cached libc.bin v%u (%u bytes, %u pages, %u exports)\n",
            (unsigned)g_version, (unsigned)g_size,
            (unsigned)g_pages, (unsigned)g_export_count);
}

int dyld_map_libc(uint32_t *user_pd) {
    if (!g_loaded) return 0;     /* harmless skip if no libc */

    /* For each page of libc.bin: alloc a fresh physical page, copy
     * the contents in, install a USER+WRITABLE mapping at
     * LIBC_VA + i*4096 in the supplied PD.
     *
     * On any failure we don't unwind — the caller will eventually
     * tear down the user_pd via paging_destroy_user_pd which frees
     * everything reachable from it. */
    for (uint32_t i = 0; i < g_pages; i++) {
        void *page = pmm_alloc_page();
        if (!page) return -1;

        uint32_t off = i * PAGE_4K;
        uint32_t copy = PAGE_4K;
        if (off + copy > g_size) copy = g_size - off;

        memcpy(page, g_image + off, copy);
        if (copy < PAGE_4K) {
            memset((uint8_t *)page + copy, 0, PAGE_4K - copy);
        }

        uint32_t va = LIBC_VA + off;
        if (paging_map_in(user_pd, va, (uintptr_t)page,
                          PTE_USER | PTE_WRITABLE) != 0) {
            pmm_free_page(page);
            return -1;
        }
    }
    return 0;
}

uint32_t dyld_libc_size(void)   { return g_size; }
int      dyld_libc_loaded(void) { return g_loaded; }
