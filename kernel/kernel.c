/*
 * AdventOS kernel main.
 */

#include "../include/types.h"
#include "../include/io.h"

#include "vga.h"
#include "serial.h"
#include "kprintf.h"
#include "smp_trace.h"
#include "string.h"
#include "gdt.h"
#include "tss.h"
#include "idt.h"
#include "isr.h"
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
#include "vfs.h"
#include "procfs.h"
#include "smp.h"
#include "net.h"
#include "udp.h"
#include "dhcp.h"
#include "dns.h"
#include "tcp.h"
#include "sock.h"
#include "pipe.h"
#include "pty.h"
#include "tmpfs.h"
#include "tty.h"
#include "elf.h"
#include "shell.h"
#include "vbe.h"
#include "fbcon.h"
#include "dyld.h"
#include "ac97.h"
#include "usb_core.h"
#include "usb_cdc_acm.h"
#include "blkdev.h"
#include "virtio_blk.h"
#include "virtio_net.h"
#include "virtio_scsi.h"
#include "ahci.h"
#include "virtio_rng.h"
#include "virtio_console.h"
#include "virtio_balloon.h"
#include "virtio_9p.h"

/* Session 38 gate: 1 = user tasks free to run on any CPU; 0 =
 * pinned to BSP. The BKL machinery + race-fixed task creation are
 * always in effect; this flag only controls cpu_pin. Off by
 * default until we add cross-CPU TLB shootdowns; see docs/38. */
int g_ap_runs_user = 0;

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

    isr_print_build_marker();

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

    /* Session 109 — Path C: PS/2 mouse. Mouse bytes arrive on the
     * same i8042 data port as keyboard; the keyboard's drain loops
     * route AUX bytes here via mouse_process_byte. */
    kputs("[boot] starting mouse... ");
    extern void mouse_init(void);
    mouse_init();
    kputs("ok\n");

    kputs("[boot] enabling serial RX IRQ... ");
    serial_install_irq();
    kputs("ok\n");

    kputs("[boot] reading BIOS E820 map... ");
    memmap_init();
    kprintf("%u entries\n", (unsigned)memmap_count());

    /* Capture the bootloader's VBE summary BEFORE pmm_init / paging_init
     * touch low memory. The bootloader stashes 12 bytes at physical
     * 0x9100; once paging_init starts allocating page tables out of the
     * PMM, that region gets reused. We snapshot now and the actual
     * paging_map of the framebuffer happens later (vbe_init), after the
     * PD exists. */
    vbe_capture_bootinfo();

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

    /* Bring up the framebuffer console — has to happen after paging is
     * on (we identity-map the FB pages into the kernel PD). The serial
     * + VGA-text sinks remain active in parallel; fbcon_init does
     * nothing if the bootloader couldn't set a graphics mode. */
    kputs("[boot] reading VBE summary from bootloader... ");
    vbe_init();
    fbcon_init();

    /* PS/2 mouse driver was removed when AdventOS narrowed to a
     * CLI-only OS for developers and AI agents. IRQ12 stays
     * masked by default. */

    kputs("[boot] initializing ATA driver... ");
    ata_init();
    kputs("ok\n");

    /* virtio-blk: paravirtualized block device. Silent no-op when no
     * such device is present. Registers as blkdev slot 1 (after ATA's
     * slot 0) when QEMU was launched with `-device virtio-blk-pci`. */
    kputs("[boot] probing virtio-blk... ");
    virtio_blk_init();
    kputs("done\n");

    /* AHCI SATA controller — registers each attached SATA disk as a
     * blkdev. Slots into the device-id space after ATA + virtio-blk
     * + USB MSC. Silent when no `-device ahci` is wired. */
    kputs("[boot] probing AHCI... ");
    ahci_init();
    kputs("done\n");

    /* virtio-scsi — paravirtualized SCSI HBA. Same blkdev plumbing
     * as virtio-blk but speaks SCSI on the wire (multi-LUN capable).
     * Silent no-op when not present. */
    kputs("[boot] probing virtio-scsi... ");
    virtio_scsi_init();
    kputs("done\n");

    /* virtio-rng / -console / -balloon: more paravirtualized devices.
     * All silently no-op when not present. */
    kputs("[boot] probing virtio-rng... ");
    virtio_rng_init();
    kputs("done\n");
    kputs("[boot] probing virtio-console... ");
    virtio_console_init();
    kputs("done\n");
    kputs("[boot] probing virtio-balloon... ");
    virtio_balloon_init();
    kputs("done\n");
    /* virtio-9p init does Tversion + Tattach right here; the mount
     * into the VFS namespace is deferred to after vfs_init below. */
    kputs("[boot] probing virtio-9p... ");
    virtio_9p_init();
    kputs("done\n");

    /* Block cache must be live BEFORE fs_init — fs.c reads its
     * superblock through bcache_read on the first call. */
    kputs("[boot] initializing block cache... ");
    bcache_init();
    kprintf("ok (%d slots, %d KiB)\n",
            (int)BCACHE_NR, (int)(BCACHE_NR * BCACHE_BLKSZ / 1024));

    kputs("[boot] mounting AdventFS... ");
    fs_init();

    /* Bring the VFS layer up: register the rootfs (the on-disk
     * AdventFS) at "/" and the synthetic procfs at "/proc". From
     * here on the syscall layer goes through vfs_open / vfs_read
     * etc. instead of calling fs.c directly. */
    kputs("[boot] mounting VFS... ");
    vfs_init();
    vfs_mount("/",     "rootfs", fs_rootfs_ops());
    vfs_mount("/proc", "procfs", procfs_ops());
    /* Session 73: ensure the agent KV root exists.  Both mkdirs are
     * idempotent (vfs_mkdir returns -1 when the directory already
     * exists, which we ignore).  Failure is non-fatal: if the FS is
     * read-only or the slot pool is full, KV operations just fail
     * cleanly later instead of crashing the boot.
     *
     * Session 77: same pattern for /var/cron — agentd persists each
     * scheduled task as /var/cron/<id>.json and scans the directory
     * at boot to repopulate the in-memory table. */
    vfs_mkdir("/var");
    vfs_mkdir("/var/kv");
    vfs_mkdir("/var/cron");
    /* virtio-9p auto-mount: if the device is present + ready, the
     * host's exported directory shows up at /mnt/9p. Silent no-op
     * otherwise (e.g. on Windows/MSYS2 QEMU which lacks 9p support). */
    vfs_mkdir("/mnt");
    if (virtio_9p_available()) {
        virtio_9p_mount("/mnt/9p");
    }
    kputs("ok\n");

    {
        struct rtc_time now;
        rtc_read(&now);
        kprintf("[boot] RTC: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                now.year, now.month, now.day,
                now.hour, now.min, now.sec);
    }

    /* Cache libc.bin from the FS so elf_load can map it into every
     * new user PD. After this, user programs can call into libc
     * via the LIBC_BASE export table. Must come AFTER fs/vfs init
     * (we need fs_open) and BEFORE any user task is launched (we
     * map libc into every PD elf_load builds). */
    kputs("[boot] caching libc.bin... ");
    dyld_init();

    kputs("[boot] initializing task system... ");
    task_init();
    kputs("ok\n");

    kputs("[boot] enabling interrupts\n");
    __asm__ volatile ("sti");

    /* SMP: parse ACPI MADT, enable LAPIC, bring up application
     * processors. Has to be after interrupts are on (we use PIT
     * to time the INIT-SIPI delay) but before any user task
     * runs (so per-CPU TSSes are in place when ring-3 runs). */
    kputs("[boot] starting SMP\n");
    smp_init();

    /* Tell the task layer the LAPIC + per-CPU table are alive. From
     * this point on, schedule() / tss_set_kernel_stack / cpu_current
     * can all dispatch via cpu_local() (= LAPIC ID lookup). Before
     * this flag flips, those paths short-circuit to BSP-only access. */
    task_smp_ready();

    /* NIC IRQs need to be live before init can succeed (the link-up
     * pre-fills the MAC via PIO, but later RX is IRQ-driven). */
    /* AC97 audio. Done after PIT (we use pit_sleep for codec reset
     * timing) but before user tasks launch. Probes PCI; logs whether
     * an Intel ICH AC97 codec was found. If absent, audio syscalls
     * just no-op. */
    ac97_init();

    /* USB stack: probes UHCI on the PIIX3 PCI device, enumerates
     * any attached HID keyboard, spawns a polling task that injects
     * keystrokes into the same ring buffer the PS/2 driver uses.
     * No-ops cleanly if QEMU was launched without `-usb`. */
    kputs("[boot] starting USB stack\n");
    usb_init();

    kputs("[boot] starting network stack\n");
    net_init();

    /* If net_init bound virtio-net, start its RX polling task NOW —
     * before DHCP runs. DHCP blocks waiting for an OFFER reply, and
     * the virtio-net RX path is polled (no IRQ), so the poller has
     * to be live for any DHCP packet to be received. The RTL8139
     * path is IRQ-driven and doesn't care about this ordering. */
    virtio_net_start_polling();

    /* UDP transport — must come before DHCP, which uses port 68/67. */
    udp_init();

    /* Synchronous DHCP lease (or fall back to SLIRP defaults if the
     * server doesn't reply within 2s twice). After this g_my_ip etc
     * are set up. */
    if (g_net_up) {
        dhcp_acquire_lease();
    }

    dns_init();
    /* Session 60 — if /etc/resolv.conf is present, pull in any extra
     * nameservers it lists as fail-overs behind the DHCP-provided
     * primary. Filesystem is already mounted by this point. */
    dns_load_resolv_conf();

    /* Sanity beep: short delay, prove the timer is ticking */
    pit_sleep(50);

    kputs("[boot] spawning reaper + demo tasks A, B\n");
    task_reaper_start();
    /*task_make_runnable(task_create(demo_task_a, "demo_a"));*/
    /*task_make_runnable(task_create(demo_task_b, "demo_b"));*/

    /* The bcache syncer needs the task system + interrupts up so its
     * pit_sleep() loop can actually be scheduled. */
    bcache_start_syncer();

    /* TCP and the socket layer must be ready before any user task
     * tries to bind/listen. The HTTP server is no longer in-kernel —
     * it's a userspace task spawned below as `httpd.elf`. */
    tcp_init();
    sock_init();
    pipe_init();
    pty_init();
    tmpfs_init();
    tty_init();

    /* Now that the task system is fully up and the BSP-only init
     * code has finished, kick off background polling tasks (USB
     * HID, etc.) that need to run on whichever CPU schedule(). */
    usb_start_polling();
    usb_cdc_acm_start_polling();
    /* virtio_net_start_polling already fired above before DHCP. */
    virtio_console_start_polling();
    virtio_balloon_start_task();

    /* Auto-mount AdventFS-formatted disks on USB / AHCI / virtio-scsi
     * blkdevs at /mnt/usb, /mnt/sata, /mnt/scsi respectively. Silently
     * no-ops if no such drive is present or its sector 0 doesn't have
     * the AdventFS magic. */
    {
        extern int blkdev_count(void);
        extern struct blkdev *blkdev_get(int idx);
        int usb_mounted = 0, sata_mounted = 0, scsi_mounted = 0;
        for (int i = 1; i < blkdev_count(); i++) {
            struct blkdev *b = blkdev_get(i);
            if (!b) continue;
            int is_usb   = (b->name[0] == 'u' && b->name[1] == 's' &&
                            b->name[2] == 'b');
            int is_sata  = (b->name[0] == 'a' && b->name[1] == 'h' &&
                            b->name[2] == 'c' && b->name[3] == 'i');
            int is_vscsi = (b->name[0] == 'v' && b->name[1] == 's' &&
                            b->name[2] == 'c' && b->name[3] == 's' &&
                            b->name[4] == 'i');
            if (!is_usb && !is_sata && !is_vscsi) continue;
            const char *mp     = is_usb   ? "/mnt/usb"
                              : is_sata  ? "/mnt/sata"
                                         : "/mnt/scsi";
            const char *fsname = is_usb   ? "usbfs"
                              : is_sata  ? "satafs"
                                         : "scsifs";
            int *flag          = is_usb   ? &usb_mounted
                              : is_sata  ? &sata_mounted
                                         : &scsi_mounted;
            if (*flag) continue;
            struct fs_instance *inst =
                fs_create_instance(b, /*base_lba=*/0, b->n_blocks);
            if (!inst) continue;
            struct vfs_fs_ops *ops = fs_make_ops_for(inst);
            if (ops && vfs_mount(mp, fsname, ops) == 0) {
                kprintf("[boot] mounted %s at %s\n", b->name, mp);
                *flag = 1;
            }
        }
    }

    kputs("\n");
    banner();
    SMP_LOG("kmain post-banner");

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
    /* kmain isn't in syscall context, so it doesn't naturally hold
     * the BKL. But the LAUNCH path touches fs / elf / paging state
     * that reaper / syncer (already running BKL-protected on the
     * AP) might be touching. Take the lock for the duration of the
     * launch so we serialize cleanly with them. */
    extern void bkl_lock(void);
    extern void bkl_unlock(void);
    bkl_lock();
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
    bkl_unlock();
    kputc('\n');

    /* If the shell launched, we expect it to drive the system from
     * here. kmain becomes the idle task and just hlt's forever. */
    for (;;) __asm__ volatile ("sti; hlt");
}
