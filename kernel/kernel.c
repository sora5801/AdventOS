/*
 * AdventOS kernel main.
 */

#include "../include/types.h"
#include "../include/io.h"

#include "vga.h"
#include "serial.h"
#include "kprintf.h"
#include "string.h"
#include "gdt.h"
#include "tss.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "memmap.h"
#include "pmm.h"
#include "paging.h"
#include "kmalloc.h"
#include "task.h"
#include "ata.h"
#include "rtc.h"
#include "fs.h"
#include "bcache.h"
#include "net.h"
#include "udp.h"
#include "dhcp.h"
#include "dns.h"
#include "tcp.h"
#include "sock.h"
#include "pipe.h"
#include "tmpfs.h"
#include "tty.h"
#include "elf.h"
#include "shell.h"

/* Two demo tasks that emit a tag to the serial port at different rates,
 * so you can see the scheduler interleaving them in real time without
 * fighting the shell's VGA cursor. */
static void demo_task_a(void) {
    for (uint32_t i = 0; ; i++) {
        serial_write("[A]");
        pit_sleep(150);
    }
}

static void demo_task_b(void) {
    for (uint32_t i = 0; ; i++) {
        serial_write("[B]");
        pit_sleep(250);
    }
}

static void banner(void) {
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs(
        "    _       _                  _    ___  ____\n"
        "   / \\   __| |_   _____ _ __ | |_ / _ \\/ ___|\n"
        "  / _ \\ / _` \\ \\ / / _ \\ '_ \\| __| | | \\___ \\\n"
        " / ___ \\ (_| |\\ V /  __/ | | | |_| |_| |___) |\n"
        "/_/   \\_\\__,_| \\_/ \\___|_| |_|\\__|\\___/|____/\n"
    );
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kputs("AdventOS v0.1 — i386 protected-mode kernel\n");
    kputs("Type 'help' for a list of commands.\n\n");
}

void kmain(uint32_t boot_drive) {
    serial_init();
    vga_init();

    kputs("[boot] serial + VGA up\n");
    kprintf("[boot] booted from drive 0x%x\n", (unsigned)boot_drive);

    kputs("[boot] installing TSS... ");
    tss_init();
    kputs("ok\n");

    kputs("[boot] installing GDT... ");
    gdt_init();
    kputs("ok (TR loaded)\n");

    kputs("[boot] installing IDT... ");
    idt_init();
    kputs("ok\n");

    kputs("[boot] remapping PIC to 0x20/0x28... ");
    pic_remap(PIC1_OFFSET, PIC2_OFFSET);
    /* Mask everything except IRQ0 (PIT) and IRQ1 (keyboard) which their
     * drivers will unmask when ready. */
    for (int i = 0; i < 16; i++) pic_set_mask((uint8_t)i);
    /* IRQ 2 is the master-side cascade for slave-PIC lines (8..15).
     * Without it unmasked, the master never sees IRQ11 (RTL8139) etc. */
    pic_clear_mask(2);
    kputs("ok\n");

    kputs("[boot] starting PIT @ 100Hz... ");
    pit_init(100);
    kputs("ok\n");

    kputs("[boot] starting keyboard... ");
    keyboard_init();
    kputs("ok\n");

    kputs("[boot] enabling serial RX IRQ... ");
    serial_install_irq();
    kputs("ok\n");

    kputs("[boot] reading BIOS E820 map... ");
    memmap_init();
    kprintf("%u entries\n", (unsigned)memmap_count());

    kputs("[boot] initializing PMM... ");
    pmm_init();
    kprintf("%u/%u pages free (%u KB)\n",
            (unsigned)pmm_free_pages(),
            (unsigned)pmm_total_pages(),
            (unsigned)(pmm_free_pages() * 4));

    kputs("[boot] reserving heap from PMM... ");
    {
        const uint32_t HEAP_PAGES = 1024;     /* 4 MiB */
        void *heap = pmm_alloc_contiguous(HEAP_PAGES);
        if (!heap) {
            kputs("FAILED — no contiguous block; halting\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        uintptr_t b = (uintptr_t)heap;
        kmalloc_init(b, b + (uintptr_t)HEAP_PAGES * PMM_PAGE_SIZE);
        kprintf("0x%x..0x%x (%u KB)\n",
                (unsigned)kmalloc_heap_start(),
                (unsigned)kmalloc_heap_end(),
                (unsigned)(kmalloc_total() >> 10));
    }

    kputs("[boot] enabling paging... ");
    paging_init();
    if (paging_is_enabled()) {
        kprintf("PD@0x%x, %u/1024 PDEs in use\n",
                (unsigned)paging_pd_addr(),
                (unsigned)paging_pd_used());
    } else {
        kputs("FAILED\n");
    }

    kputs("[boot] initializing ATA driver... ");
    ata_init();
    kputs("ok\n");

    /* Block cache must be live BEFORE fs_init — fs.c reads its
     * superblock through bcache_read on the first call. */
    kputs("[boot] initializing block cache... ");
    bcache_init();
    kprintf("ok (%d slots, %d KiB)\n",
            (int)BCACHE_NR, (int)(BCACHE_NR * BCACHE_BLKSZ / 1024));

    kputs("[boot] mounting AdventFS... ");
    fs_init();

    {
        struct rtc_time now;
        rtc_read(&now);
        kprintf("[boot] RTC: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                now.year, now.month, now.day,
                now.hour, now.min, now.sec);
    }

    kputs("[boot] initializing task system... ");
    task_init();
    kputs("ok\n");

    kputs("[boot] enabling interrupts\n");
    __asm__ volatile ("sti");

    /* NIC IRQs need to be live before init can succeed (the link-up
     * pre-fills the MAC via PIO, but later RX is IRQ-driven). */
    kputs("[boot] starting network stack\n");
    net_init();

    /* UDP transport — must come before DHCP, which uses port 68/67. */
    udp_init();

    /* Synchronous DHCP lease (or fall back to SLIRP defaults if the
     * server doesn't reply within 2s twice). After this g_my_ip etc
     * are set up. */
    if (g_net_up) {
        dhcp_acquire_lease();
    }

    dns_init();

    /* Sanity beep: short delay, prove the timer is ticking */
    pit_sleep(50);

    kputs("[boot] spawning reaper + demo tasks A, B\n");
    task_reaper_start();
    task_create(demo_task_a, "demo_a");
    task_create(demo_task_b, "demo_b");

    /* The bcache syncer needs the task system + interrupts up so its
     * pit_sleep() loop can actually be scheduled. */
    bcache_start_syncer();

    /* TCP and the socket layer must be ready before any user task
     * tries to bind/listen. The HTTP server is no longer in-kernel —
     * it's a userspace task spawned below as `httpd.elf`. */
    tcp_init();
    sock_init();
    pipe_init();
    tmpfs_init();
    tty_init();

    kputs("\n");
    banner();

    /* Helper: load + setup args + spawn a single user task. argv is a
     * brace-enclosed initializer list — pass any number of strings.
     * Example: LAUNCH("sh.elf", "sh", "selftest"); */
    #define LAUNCH(path, ...) do {                                         \
        int _fd = fs_open(path);                                           \
        if (_fd >= 0) {                                                    \
            struct elf_load_result _r;                                     \
            if (elf_load(_fd, &_r) == 0) {                                 \
                const char *_argv[] = { __VA_ARGS__ };                     \
                int _argc = (int)(sizeof(_argv) / sizeof(_argv[0]));       \
                elf_setup_args(&_r, _argc, _argv);                         \
                struct task *_t = task_create_user(_r.entry, _r.user_esp,  \
                                                   _r.cr3, _argv[0]);      \
                if (_t) kprintf("[boot] launched %s as pid %u\n",          \
                                path, (unsigned)_t->id);                   \
            }                                                              \
        }                                                                  \
    } while (0)

    /* Boot policy lives in userspace from session 22 onward: kmain
     * only spawns init.elf, which reads /etc/inittab and forks the
     * actual services (httpd, sh, ...). Tell the task layer init's
     * pid so future orphan reparenting goes there. */
    {
        int  _fd = fs_open("init.elf");
        if (_fd >= 0) {
            struct elf_load_result _r;
            if (elf_load(_fd, &_r) == 0) {
                const char *_argv[] = { "init" };
                elf_setup_args(&_r, 1, _argv);
                struct task *_t = task_create_user(_r.entry, _r.user_esp,
                                                   _r.cr3, "init");
                if (_t) {
                    task_set_init_pid(_t->id);
                    kprintf("[boot] launched init.elf as pid %u\n",
                            (unsigned)_t->id);
                }
            }
        }
    }
    kputc('\n');

    /* If the shell launched, we expect it to drive the system from
     * here. kmain becomes the idle task and just hlt's forever. */
    for (;;) __asm__ volatile ("sti; hlt");
}
