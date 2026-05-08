#include "shell.h"
#include "kprintf.h"
#include "keyboard.h"
#include "string.h"
#include "vga.h"
#include "pit.h"
#include "memmap.h"
#include "pmm.h"
#include "paging.h"
#include "kmalloc.h"
#include "task.h"
#include "ata.h"
#include "rtc.h"
#include "fs.h"
#include "elf.h"
#include "mutex.h"
#include "serial.h"
#include "net.h"
#include "arp.h"
#include "ip.h"
#include "icmp.h"
#include "../include/io.h"

extern uint8_t up1_start[];
extern uint8_t up1_end[];
extern uint8_t up2_start[];
extern uint8_t up2_end[];

#define USER_CODE_VA   0x40000000u
#define USER_STACK_VA  0x40100000u

#define LINE_MAX 128

/* Linker-defined: declared without the leading underscore here so the
 * compiler's automatic underscore-prefix lands on the matching symbol. */
extern uint8_t kernel_start;
extern uint8_t bss_end;

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf));
}

static void cmd_help(void) {
    kputs("Available commands:\n");
    kputs("  help          - this list\n");
    kputs("  echo X        - print X\n");
    kputs("  clear         - clear the screen\n");
    kputs("  uptime        - ticks and seconds since boot\n");
    kputs("  cpuid         - vendor + features from CPUID\n");
    kputs("  mem           - kernel memory layout\n");
    kputs("  meminfo       - E820 + PMM + heap stats\n");
    kputs("  paginfo       - paging state and PD entries\n");
    kputs("  pagefault     - read an unmapped address (triggers #PF)\n");
    kputs("  kmalloc N     - allocate N bytes from the heap\n");
    kputs("  ppalloc       - allocate one physical page from PMM\n");
    kputs("  tasks         - list scheduled tasks\n");
    kputs("  yield         - voluntarily call schedule()\n");
    kputs("  userprog      - launch ring-3 program 1 (Hello + getpid + yield)\n");
    kputs("  userprog2     - launch ring-3 program 2 (counter + sleep + time)\n");
    kputs("  ls            - list files on AdventFS\n");
    kputs("  cat NAME      - dump first 128 bytes of a file\n");
    kputs("  exec NAME     - load + run an ELF program from the filesystem\n");
    kputs("  ifconfig      - show NIC + IP configuration\n");
    kputs("  arp           - show ARP cache  (arp WHO sends a request)\n");
    kputs("  ping IP       - send ICMP echo request, wait for reply\n");
    kputs("  kfree A       - free heap block at address A\n");
    kputs("  kmtest        - alloc + free demo (shows coalescing)\n");
    kputs("  ata read N    - read sector N of primary disk\n");
    kputs("  ata write N   - fill sector N with a pattern\n");
    kputs("  time          - read RTC + wall-clock time\n");
    kputs("  mtest         - spawn two tasks contending on a mutex\n");
    kputs("  color FG      - set VGA foreground color (0-15)\n");
    kputs("  panic         - trigger a deliberate divide-by-zero\n");
    kputs("  reboot        - reset the machine via 8042\n");
    kputs("  halt          - cli + hlt\n");
}

static void cmd_uptime(void) {
    uint32_t t = pit_ticks();
    uint32_t s = pit_seconds();
    kprintf("ticks=%u  seconds=%u\n", (unsigned)t, (unsigned)s);
}

static void cmd_cpuid(void) {
    uint32_t a, b, c, d;
    char vendor[13];

    cpuid(0, &a, &b, &c, &d);
    *(uint32_t *)(vendor + 0) = b;
    *(uint32_t *)(vendor + 4) = d;
    *(uint32_t *)(vendor + 8) = c;
    vendor[12] = 0;
    kprintf("CPU vendor : %s\n", vendor);
    kprintf("Max leaf   : %u\n", (unsigned)a);

    cpuid(1, &a, &b, &c, &d);
    kprintf("Family     : %u  Model: %u  Stepping: %u\n",
            (unsigned)((a >> 8) & 0xF),
            (unsigned)((a >> 4) & 0xF),
            (unsigned)(a & 0xF));
    kprintf("Features   : EDX=0x%08x  ECX=0x%08x\n", (unsigned)d, (unsigned)c);
    kputs("  ");
    if (d & (1 << 0))  kputs("FPU ");
    if (d & (1 << 4))  kputs("TSC ");
    if (d & (1 << 5))  kputs("MSR ");
    if (d & (1 << 9))  kputs("APIC ");
    if (d & (1 << 23)) kputs("MMX ");
    if (d & (1 << 25)) kputs("SSE ");
    if (d & (1 << 26)) kputs("SSE2 ");
    if (c & (1 << 0))  kputs("SSE3 ");
    if (c & (1 << 19)) kputs("SSE4.1 ");
    if (c & (1 << 20)) kputs("SSE4.2 ");
    if (c & (1 << 28)) kputs("AVX ");
    kputc('\n');
}

static void cmd_mem(void) {
    uint32_t start = (uint32_t)(uintptr_t)&kernel_start;
    uint32_t end   = (uint32_t)(uintptr_t)&bss_end;
    kprintf("Kernel image: 0x%08x .. 0x%08x  (%u bytes)\n",
            (unsigned)start, (unsigned)end, (unsigned)(end - start));
    kputs("Boot sector : 0x00007C00 .. 0x00007E00  (512 bytes)\n");
    kputs("Stack       : grows down from 0x90000\n");
    kputs("VGA buffer  : 0x000B8000\n");
}

static uint32_t parse_uint(const char *s) {
    uint32_t n = 0;
    int hex = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { hex = 1; s += 2; }
    while (*s) {
        char c = *s++;
        if (hex) {
            if (c >= '0' && c <= '9')      n = (n << 4) + (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') n = (n << 4) + (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') n = (n << 4) + (uint32_t)(c - 'A' + 10);
            else break;
        } else {
            if (c >= '0' && c <= '9') n = n * 10 + (uint32_t)(c - '0');
            else break;
        }
    }
    return n;
}

static void put_hex64(uint64_t v) {
    kputc('0'); kputc('x');
    static const char digits[] = "0123456789abcdef";
    for (int i = 60; i >= 0; i -= 4) {
        kputc(digits[(v >> i) & 0xF]);
    }
}

static void cmd_meminfo(void) {
    uint32_t n = memmap_count();
    if (n == 0) {
        kputs("No BIOS E820 memory map available.\n");
    } else {
        kprintf("BIOS E820 memory map (%u entries):\n", (unsigned)n);
        kputs("  base               length             type\n");
        for (uint32_t i = 0; i < n; i++) {
            const struct e820_entry *e = memmap_entry(i);
            kputs("  ");
            put_hex64(e->base);
            kputs(" ");
            put_hex64(e->length);
            kputs(" ");
            kputs(memmap_type_name(e->type));
            kputc('\n');
        }
        uint64_t usable = memmap_total_usable();
        /* Constant shifts on uint64_t are inlined by gcc — no libgcc */
        uint32_t mb = (uint32_t)(usable >> 20);
        uint32_t kb = (uint32_t)(usable >> 10);
        kprintf("\nTotal usable RAM: %u MB (%u KB)\n",
                (unsigned)mb, (unsigned)kb);
    }

    kputs("\nPhysical Memory Manager:\n");
    kprintf("  total pages : %u  (%u KB)\n",
            (unsigned)pmm_total_pages(),
            (unsigned)(pmm_total_pages() * 4));
    kprintf("  used pages  : %u  (%u KB)\n",
            (unsigned)pmm_used_pages(),
            (unsigned)(pmm_used_pages() * 4));
    kprintf("  free pages  : %u  (%u KB)\n",
            (unsigned)pmm_free_pages(),
            (unsigned)(pmm_free_pages() * 4));

    {
        uint32_t ub, fb;
        kmalloc_block_counts(&ub, &fb);
        kputs("\nKernel heap (free-list allocator on PMM):\n");
        kprintf("  range       : 0x%08x .. 0x%08x  (%u KB)\n",
                (unsigned)kmalloc_heap_start(),
                (unsigned)kmalloc_heap_end(),
                (unsigned)(kmalloc_total() >> 10));
        kprintf("  used        : %u bytes  (%u blocks)\n",
                (unsigned)kmalloc_used(), (unsigned)ub);
        kprintf("  free        : %u bytes  (%u blocks)\n",
                (unsigned)kmalloc_free(), (unsigned)fb);
        kprintf("  largest free: %u bytes\n",
                (unsigned)kmalloc_largest_free());
    }
}

static void cmd_paginfo(void) {
    if (!paging_is_enabled()) {
        kputs("Paging is OFF.\n");
        return;
    }
    uint32_t cr0, cr3;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    kprintf("Paging        : ENABLED\n");
    kprintf("CR0           : 0x%08x  (PG=%u, PE=%u, WP=%u)\n",
            (unsigned)cr0,
            (unsigned)((cr0 >> 31) & 1),
            (unsigned)((cr0 >>  0) & 1),
            (unsigned)((cr0 >> 16) & 1));
    kprintf("CR3 (PD phys) : 0x%08x\n", (unsigned)cr3);
    kprintf("PD entries    : %u/1024 present\n", (unsigned)paging_pd_used());

    /* Dump the present PDEs so we can see the identity map. */
    const uint32_t *pd = (const uint32_t *)paging_pd_addr();
    int shown = 0;
    for (int i = 0; i < 1024; i++) {
        if (!(pd[i] & 1)) continue;
        if (shown >= 12) {
            kputs("    ... (truncated)\n");
            break;
        }
        uintptr_t va_lo = (uintptr_t)i << 22;
        uintptr_t va_hi = va_lo + 0x400000 - 1;
        kprintf("    PDE[%d] = 0x%08x  -> VA 0x%08x..0x%08x\n",
                i,
                (unsigned)pd[i],
                (unsigned)va_lo,
                (unsigned)va_hi);
        shown++;
    }

    /* Round-trip a translation to prove the mapping is real. */
    uintptr_t v = 0xB8000;            /* VGA buffer */
    uintptr_t p = paging_translate(v);
    kprintf("translate(0x%x) = 0x%x\n", (unsigned)v, (unsigned)p);
}

static void cmd_pagefault(void) {
    /* Anything well above our identity map will be unmapped. 1 GiB is
     * outside any reasonable guest's physical range and outside the
     * 1 GiB cap paging_init applies. */
    uintptr_t bad = 0x40000000u;
    kprintf("Reading 0x%08x — expect a page fault...\n", (unsigned)bad);
    volatile uint32_t v = *(volatile uint32_t *)bad;
    (void)v;
}

static void cmd_ppalloc(void) {
    void *p = pmm_alloc_page();
    if (!p) {
        kputs("ppalloc: out of physical pages\n");
        return;
    }
    kprintf("pmm_alloc_page() = 0x%08x  (%u/%u pages used)\n",
            (unsigned)(uintptr_t)p,
            (unsigned)pmm_used_pages(),
            (unsigned)pmm_total_pages());
    /* Touch it to prove it's accessible under the identity map. */
    *(volatile uint32_t *)p = 0xDEADBEEF;
    uint32_t back = *(volatile uint32_t *)p;
    kprintf("wrote/read 0x%08x at the new page (paging works)\n",
            (unsigned)back);
}

static void cmd_kmalloc(const char *arg) {
    if (!*arg) { kputs("kmalloc: usage: kmalloc <size>\n"); return; }
    uint32_t size = parse_uint(arg);
    if (size == 0) { kputs("kmalloc: size must be > 0\n"); return; }
    void *p = kmalloc(size);
    if (!p) {
        kprintf("kmalloc(%u): out of memory (%u bytes free)\n",
                (unsigned)size, (unsigned)kmalloc_free());
        return;
    }
    /* Touch the memory so we can prove the allocation is real */
    volatile uint8_t *b = (volatile uint8_t *)p;
    b[0]        = 0xAB;
    b[size - 1] = 0xCD;
    kprintf("kmalloc(%u) = 0x%08x  (%u bytes free, %u bytes used)\n",
            (unsigned)size,
            (unsigned)(uintptr_t)p,
            (unsigned)kmalloc_free(),
            (unsigned)kmalloc_used());
}

static void cmd_tasks(void) {
    kputs(" ID  STATE  NAME             ESP         SWITCHES\n");
    struct task *cur = task_current();
    for (uint32_t i = 0; i < 16; i++) {
        struct task *t = task_at(i);
        if (!t) continue;
        kprintf(" %2u  %-5s  %-16s 0x%08x  %u%s\n",
                (unsigned)t->id,
                task_state_name(t->state),
                t->name,
                (unsigned)t->esp,
                (unsigned)t->switches_in,
                (t == cur) ? "  <-- current" : "");
    }
}

static void cmd_yield(void) {
    uint32_t before = task_current()->switches_in;
    kputs("calling schedule()...\n");
    task_yield();
    uint32_t after = task_current()->switches_in;
    kprintf("back. switches_in: %u -> %u\n", (unsigned)before, (unsigned)after);
}

static void cmd_kfree(const char *arg) {
    if (!*arg) { kputs("kfree: usage: kfree <hex_address>\n"); return; }
    uint32_t addr = parse_uint(arg);
    if (addr == 0) { kputs("kfree: bad address\n"); return; }
    size_t before = kmalloc_used();
    kfree((void *)(uintptr_t)addr);
    size_t after = kmalloc_used();
    kprintf("kfree(0x%08x): used %u -> %u bytes\n",
            (unsigned)addr, (unsigned)before, (unsigned)after);
}

static void show_heap_short(const char *tag) {
    uint32_t ub, fb;
    kmalloc_block_counts(&ub, &fb);
    kprintf("  [%s] used=%u free=%u  blocks=%u/%u  largest=%u\n",
            tag,
            (unsigned)kmalloc_used(),
            (unsigned)kmalloc_free(),
            (unsigned)ub, (unsigned)fb,
            (unsigned)kmalloc_largest_free());
}

static void cmd_kmtest(void) {
    kputs("kmtest: alloc 3 blocks, free middle, then ends, observe coalescing.\n");
    show_heap_short("init");

    void *a = kmalloc(1024);
    void *b = kmalloc(2048);
    void *c = kmalloc(512);
    kprintf("  a=0x%08x  b=0x%08x  c=0x%08x\n",
            (unsigned)(uintptr_t)a,
            (unsigned)(uintptr_t)b,
            (unsigned)(uintptr_t)c);
    show_heap_short("post-alloc");

    /* Touch each allocation to prove it's writable. */
    if (a) ((uint8_t *)a)[0] = 0xA1;
    if (b) ((uint8_t *)b)[2047] = 0xB1;
    if (c) ((uint8_t *)c)[256] = 0xC1;

    kfree(b);
    show_heap_short("after kfree(b)");
    kfree(a);
    show_heap_short("after kfree(a)  (coalesces fwd into b's hole)");
    kfree(c);
    show_heap_short("after kfree(c)  (coalesces back to one big free block)");
}

static void hex_dump(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i += 16) {
        kprintf("  %04x: ", (unsigned)i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n) kprintf("%02x ", p[i + j]);
            else           kputs("   ");
        }
        kputs(" ");
        for (size_t j = 0; j < 16 && i + j < n; j++) {
            uint8_t c = p[i + j];
            kputc((c >= 32 && c < 127) ? (char)c : '.');
        }
        kputc('\n');
    }
}

static void cmd_ata(const char *arg) {
    static uint8_t buf[512];

    if (strncmp(arg, "read ", 5) == 0) {
        uint32_t lba = parse_uint(arg + 5);
        if (ata_read_sector(lba, buf) != 0) {
            kprintf("ata: read sector %u FAILED\n", (unsigned)lba);
            return;
        }
        kprintf("Sector %u (first 64 bytes):\n", (unsigned)lba);
        hex_dump(buf, 64);
        if (lba == 0) {
            uint16_t mbr_sig = buf[510] | ((uint16_t)buf[511] << 8);
            kprintf("  MBR signature at offset 510 = 0x%04x %s\n",
                    (unsigned)mbr_sig, mbr_sig == 0xAA55 ? "(valid)" : "(invalid)");
        }
        return;
    }

    if (strncmp(arg, "write ", 6) == 0) {
        uint32_t lba = parse_uint(arg + 6);
        if (lba < 64) {
            kputs("ata: refusing to write below sector 64 (kernel image lives there)\n");
            return;
        }
        for (int i = 0; i < 512; i++) buf[i] = (uint8_t)(0xA0 + (i & 0x0F));
        if (ata_write_sector(lba, buf) != 0) {
            kprintf("ata: write sector %u FAILED\n", (unsigned)lba);
            return;
        }
        /* Read back to verify. */
        static uint8_t verify[512];
        for (int i = 0; i < 512; i++) verify[i] = 0;
        if (ata_read_sector(lba, verify) != 0) {
            kprintf("ata: readback sector %u FAILED\n", (unsigned)lba);
            return;
        }
        int ok = 1;
        for (int i = 0; i < 512; i++) {
            if (verify[i] != buf[i]) { ok = 0; break; }
        }
        kprintf("ata: wrote sector %u; readback %s\n",
                (unsigned)lba, ok ? "MATCHES (write/read OK)" : "MISMATCH (FAIL)");
        return;
    }

    kputs("Usage: ata read <sector>  |  ata write <sector>\n");
}

static void cmd_time(void) {
    struct rtc_time t;
    rtc_read(&t);
    kprintf("RTC : %04d-%02d-%02d %02d:%02d:%02d UTC\n",
            t.year, t.month, t.day, t.hour, t.min, t.sec);
    kprintf("epoch: %u seconds since 1970-01-01\n",
            (unsigned)rtc_to_epoch(&t));
}

/* Mutex demo state. Two kernel tasks fight over `g_demo_mtx`; while a
 * task holds it, no other task should be able to print between its
 * "[Mx-IN]" and "[Mx-OUT]" tags on serial. */
static mutex_t g_demo_mtx = MUTEX_INIT;

static void mtxa(void) {
    for (uint32_t i = 0; ; i++) {
        mutex_lock(&g_demo_mtx);
        serial_write("[MA-IN ");
        pit_sleep(120);
        serial_write("MA-OUT]");
        mutex_unlock(&g_demo_mtx);
        pit_sleep(80);
    }
}

static void mtxb(void) {
    pit_sleep(60);   /* offset start so contention shows up */
    for (uint32_t i = 0; ; i++) {
        mutex_lock(&g_demo_mtx);
        serial_write("[MB-IN ");
        pit_sleep(120);
        serial_write("MB-OUT]");
        mutex_unlock(&g_demo_mtx);
        pit_sleep(80);
    }
}

static void cmd_mtest(void) {
    static int spawned = 0;
    if (spawned) {
        kputs("mtest: tasks already spawned — they're still running.\n");
        return;
    }
    mutex_init(&g_demo_mtx);
    task_create(mtxa, "mtx_a");
    task_create(mtxb, "mtx_b");
    spawned = 1;
    kputs("mtest: spawned mtx_a and mtx_b. Watch serial for [MA-IN ... MA-OUT] /\n");
    kputs("       [MB-IN ... MB-OUT] — the two should never interleave inside a\n");
    kputs("       single critical section.\n");
}

/* Spawn a fresh ring-3 task running the program found in the
 * [src, src+len) byte range. Each invocation gets its own page
 * directory, code page and stack page. */
static void spawn_user_task(const uint8_t *src, size_t len, const char *name) {
    uint32_t *user_pd = paging_create_user_pd();
    if (!user_pd) { kprintf("%s: out of memory (PD)\n", name); return; }

    void *code_page  = pmm_alloc_page();
    void *stack_page = pmm_alloc_page();
    if (!code_page || !stack_page) {
        if (code_page)  pmm_free_page(code_page);
        if (stack_page) pmm_free_page(stack_page);
        paging_destroy_user_pd(user_pd);
        kprintf("%s: out of memory (pages)\n", name);
        return;
    }

    /* Copy program bytes into the freshly allocated physical page.
     * The page is identity-mapped in the kernel master PD, so we write
     * directly via its physical address. */
    for (size_t i = 0; i < len; i++) {
        ((uint8_t *)code_page)[i] = src[i];
    }

    if (paging_map_in(user_pd, USER_CODE_VA,  (uintptr_t)code_page,
                      PTE_WRITABLE | PTE_USER) != 0 ||
        paging_map_in(user_pd, USER_STACK_VA, (uintptr_t)stack_page,
                      PTE_WRITABLE | PTE_USER) != 0) {
        paging_destroy_user_pd(user_pd);
        pmm_free_page(code_page);
        pmm_free_page(stack_page);
        kprintf("%s: map failed\n", name);
        return;
    }

    uint32_t user_esp = USER_STACK_VA + 0x1000;

    struct task *t = task_create_user(USER_CODE_VA, user_esp,
                                      (uint32_t)(uintptr_t)user_pd, name);
    if (!t) {
        paging_destroy_user_pd(user_pd);
        kprintf("%s: task_create_user failed\n", name);
        return;
    }

    kprintf("spawned %s pid=%u  cr3=0x%08x  code=%u bytes @ phys 0x%08x\n",
            name,
            (unsigned)t->id,
            (unsigned)(uintptr_t)user_pd,
            (unsigned)len,
            (unsigned)(uintptr_t)code_page);
}

static int parse_ipv4(const char *s, struct ip_addr *out) {
    int idx = 0;
    int v   = 0;
    int saw_digit = 0;
    while (*s && idx < 4) {
        if (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (v > 255) return -1;
            saw_digit = 1;
        } else if (*s == '.') {
            if (!saw_digit) return -1;
            out->b[idx++] = (uint8_t)v;
            v = 0;
            saw_digit = 0;
        } else {
            return -1;
        }
        s++;
    }
    if (!saw_digit || idx != 3) return -1;
    out->b[3] = (uint8_t)v;
    return 0;
}

static void cmd_ifconfig(void) {
    if (!g_net_up) {
        kputs("ifconfig: NIC not initialized (no RTL8139?)\n");
        return;
    }
    kputs("eth0:\n");
    kputs("    HWaddr  ");  net_print_mac(&g_my_mac);       kputc('\n');
    kputs("    inet    ");  net_print_ip (&g_my_ip);        kputc('\n');
    kputs("    netmask ");  net_print_ip (&g_subnet_mask);  kputc('\n');
    kputs("    gateway ");  net_print_ip (&g_gateway_ip);   kputc('\n');
}

static void cmd_arp(const char *arg) {
    if (!g_net_up) { kputs("arp: NIC not initialized\n"); return; }

    if (!*arg) { arp_print_cache(); return; }

    struct ip_addr target;
    if (parse_ipv4(arg, &target) != 0) {
        kprintf("arp: bad address '%s'\n", arg);
        return;
    }
    arp_send_request(&target);
    /* Wait up to ~1s for the reply to come in. */
    struct mac_addr mac;
    for (int i = 0; i < 100; i++) {
        if (arp_lookup(&target, &mac) == 0) {
            kputs("arp: ");  net_print_ip(&target);
            kputs("  ->  "); net_print_mac(&mac);
            kputc('\n');
            return;
        }
        pit_sleep(10);
    }
    kputs("arp: timeout\n");
}

static void cmd_ping(const char *arg) {
    if (!g_net_up) { kputs("ping: NIC not initialized\n"); return; }
    if (!*arg)     { kputs("ping: usage: ping <ip>\n"); return; }

    struct ip_addr target;
    if (parse_ipv4(arg, &target) != 0) {
        kprintf("ping: bad address '%s'\n", arg);
        return;
    }

    /* If the target's MAC isn't cached, run an ARP first. ip_send
     * will probe ARP itself (returning -2) but we want a clear
     * "ARP timeout" diagnostic distinct from "ICMP timeout". */
    struct ip_addr  nexthop = net_is_local(&target) ? target : g_gateway_ip;
    struct mac_addr mac;
    if (arp_lookup(&nexthop, &mac) != 0) {
        arp_send_request(&nexthop);
        for (int i = 0; i < 50; i++) {
            if (arp_lookup(&nexthop, &mac) == 0) break;
            pit_sleep(10);
        }
        if (arp_lookup(&nexthop, &mac) != 0) {
            kputs("ping: ARP resolution failed\n");
            return;
        }
    }

    static uint16_t seq = 0;
    seq++;
    g_ping_received = 0;

    uint32_t t_send = pit_ticks();
    if (icmp_send_echo(&target, 0xBEEF, seq) < 0) {
        kputs("ping: send failed\n");
        return;
    }

    /* 1-second timeout, polled at the same 10 ms granularity. */
    for (int i = 0; i < 100; i++) {
        if (g_ping_received && g_ping_last_seq == seq) break;
        pit_sleep(10);
    }

    if (g_ping_received && g_ping_last_seq == seq) {
        uint32_t dt = g_ping_last_tick - t_send;
        kputs("PONG from ");
        net_print_ip(&target);
        kprintf("  seq=%u  time=%u ms\n",
                (unsigned)seq, (unsigned)(dt * 10));   /* ticks → ms at 100 Hz */
    } else {
        kputs("ping: timeout\n");
    }
}

static void cmd_ls(void) {
    int n = fs_count();
    if (n <= 0) {
        kputs("ls: filesystem not mounted (or empty)\n");
        return;
    }
    kprintf("Total %d file%s on AdventFS:\n", n, (n == 1) ? "" : "s");
    kputs("  NAME             SIZE\n");
    for (int i = 0; i < n; i++) {
        kprintf("  %-16s %u bytes\n", fs_name(i), (unsigned)fs_size(i));
    }
}

static void cmd_cat(const char *arg) {
    if (!*arg) { kputs("Usage: cat <name>\n"); return; }
    int fd = fs_open(arg);
    if (fd < 0) { kprintf("cat: %s: not found\n", arg); return; }

    uint32_t total = fs_size(fd);
    kprintf("%s (%u bytes):\n", arg, (unsigned)total);

    uint8_t  buf[128];
    uint32_t take = total < sizeof(buf) ? total : sizeof(buf);
    int rd = fs_read(fd, 0, buf, take);
    if (rd <= 0) { kputs("cat: read failed\n"); return; }
    hex_dump(buf, (size_t)rd);
}

static void cmd_exec(const char *arg) {
    if (!*arg) { kputs("Usage: exec <name>\n"); return; }
    int fd = fs_open(arg);
    if (fd < 0) { kprintf("exec: %s: not found\n", arg); return; }

    struct elf_load_result r;
    int err = elf_load(fd, &r);
    if (err != 0) {
        kprintf("exec: %s: elf_load failed (code %d)\n", arg, err);
        return;
    }

    struct task *t = task_create_user(r.entry, r.user_esp, r.cr3, arg);
    if (!t) {
        kputs("exec: task_create_user failed\n");
        paging_destroy_user_pd((uint32_t *)(uintptr_t)r.cr3);
        return;
    }
    kprintf("exec: pid=%u  cr3=0x%08x  entry=0x%08x  esp=0x%08x  (loaded %s)\n",
            (unsigned)t->id,
            (unsigned)r.cr3,
            (unsigned)r.entry,
            (unsigned)r.user_esp,
            arg);
}

static void cmd_userprog(void) {
    spawn_user_task(up1_start, (size_t)(up1_end - up1_start), "userprog1");
}

static void cmd_userprog2(void) {
    spawn_user_task(up2_start, (size_t)(up2_end - up2_start), "userprog2");
}

static void cmd_color(const char *arg) {
    int n = 0;
    while (*arg >= '0' && *arg <= '9') {
        n = n * 10 + (*arg - '0');
        arg++;
    }
    if (n < 0 || n > 15) { kputs("color: expected value 0-15\n"); return; }
    vga_set_color((uint8_t)n, VGA_BLACK);
    kprintf("Foreground color set to %d\n", n);
}

static void cmd_reboot(void) {
    kputs("Rebooting via keyboard controller...\n");
    /* Wait for keyboard buffer to be empty, then issue reset */
    while (inb(0x64) & 0x02) {}
    outb(0x64, 0xFE);
    /* If that fails, triple fault: load null IDT and execute int */
    struct { uint16_t l; uint32_t b; } __attribute__((packed)) zero_idt = {0, 0};
    __asm__ volatile ("lidt %0; int $0x03" :: "m"(zero_idt));
    for (;;) __asm__ volatile ("hlt");
}

static void cmd_panic(void) {
    kputs("Triggering divide-by-zero on purpose...\n");
    /* Use inline asm so the optimizer can't fold the div away. */
    __asm__ volatile (
        "xor %%edx, %%edx \n"
        "xor %%ecx, %%ecx \n"
        "mov $1, %%eax    \n"
        "div %%ecx        \n"
        ::: "eax", "ecx", "edx"
    );
}

static void read_line(char *buf, size_t cap) {
    size_t i = 0;
    for (;;) {
        char c = keyboard_wait_char();
        if (c == '\n') {
            kputc('\n');
            buf[i] = 0;
            return;
        }
        if (c == '\b') {
            if (i > 0) { i--; kputc('\b'); }
            continue;
        }
        if ((uint8_t)c >= 32 && i + 1 < cap) {
            buf[i++] = c;
            kputc(c);
        }
    }
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ') s++;
    return s;
}

static void run_command(char *line) {
    /* Trim leading spaces */
    while (*line == ' ') line++;
    if (*line == 0) return;

    /* Find argument */
    char *space = strchr(line, ' ');
    char *arg = "";
    if (space) {
        *space = 0;
        arg = (char *)skip_spaces(space + 1);
    }

    if      (strcmp(line, "help")    == 0) cmd_help();
    else if (strcmp(line, "?")       == 0) cmd_help();
    else if (strcmp(line, "echo")    == 0) { kputs(arg); kputc('\n'); }
    else if (strcmp(line, "clear")   == 0) vga_clear();
    else if (strcmp(line, "cls")     == 0) vga_clear();
    else if (strcmp(line, "uptime")  == 0) cmd_uptime();
    else if (strcmp(line, "cpuid")   == 0) cmd_cpuid();
    else if (strcmp(line, "mem")     == 0) cmd_mem();
    else if (strcmp(line, "meminfo") == 0) cmd_meminfo();
    else if (strcmp(line, "paginfo") == 0) cmd_paginfo();
    else if (strcmp(line, "pagefault") == 0) cmd_pagefault();
    else if (strcmp(line, "ppalloc") == 0) cmd_ppalloc();
    else if (strcmp(line, "tasks")   == 0) cmd_tasks();
    else if (strcmp(line, "yield")    == 0) cmd_yield();
    else if (strcmp(line, "userprog")  == 0) cmd_userprog();
    else if (strcmp(line, "userprog2") == 0) cmd_userprog2();
    else if (strcmp(line, "ls")        == 0) cmd_ls();
    else if (strcmp(line, "cat")       == 0) cmd_cat(arg);
    else if (strcmp(line, "exec")      == 0) cmd_exec(arg);
    else if (strcmp(line, "ifconfig")  == 0) cmd_ifconfig();
    else if (strcmp(line, "arp")       == 0) cmd_arp(arg);
    else if (strcmp(line, "ping")      == 0) cmd_ping(arg);
    else if (strcmp(line, "kmalloc") == 0) cmd_kmalloc(arg);
    else if (strcmp(line, "kfree")   == 0) cmd_kfree(arg);
    else if (strcmp(line, "kmtest")  == 0) cmd_kmtest();
    else if (strcmp(line, "ata")     == 0) cmd_ata(arg);
    else if (strcmp(line, "time")    == 0) cmd_time();
    else if (strcmp(line, "mtest")   == 0) cmd_mtest();
    else if (strcmp(line, "color")   == 0) cmd_color(arg);
    else if (strcmp(line, "panic")   == 0) cmd_panic();
    else if (strcmp(line, "reboot")  == 0) cmd_reboot();
    else if (strcmp(line, "halt")   == 0) {
        kputs("System halted.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    else {
        kprintf("unknown command: %s (try 'help')\n", line);
    }
}

void shell_run(void) {
    char line[LINE_MAX];
    for (;;) {
        kputs("advent> ");
        read_line(line, sizeof(line));
        run_command(line);
    }
}
