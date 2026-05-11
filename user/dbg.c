/*
 * dbg — a ptrace-based interactive debugger for AdventOS (session 57).
 *
 * Builds on the kernel's SYS_PTRACE multiplex (see kernel/ptrace.c).
 * Flow:
 *
 *   1. Parse argv into <target.elf> + tracee args.
 *   2. Load <target>.syms (an `nm`-format text file shipped alongside
 *      the binary in the fs) into a symbol table the debugger can
 *      look up by name.
 *   3. fork(); child does PTRACE_TRACEME + sys_exec(target). Target
 *      is expected to issue `int $3` early so we get an entry stop
 *      without racing the kernel — dbgtest.c demonstrates this.
 *   4. Parent loop:
 *        - PTRACE_WAIT  → block until tracee stops or exits
 *        - present a prompt, accept commands (regs / syms / break /
 *          delete / cont / step / mem / quit)
 *        - PTRACE_CONT / PTRACE_STEP → resume
 *
 * Software breakpoints follow the standard ptrace-software-bp dance:
 *
 *   set:    save original byte at addr, write 0xCC
 *   hit:    EIP = addr + 1 (CPU advances past INT3). Tracer wants
 *           "as if the instruction is about to execute" — rewind EIP
 *           to addr, write the original byte back, then PTRACE_STEP
 *           (TF=1) to execute ONE instruction, then re-plant 0xCC,
 *           then PTRACE_CONT.
 *
 * `--auto` mode runs a scripted demo on `dbgtest.elf` that the t40
 * selftest greps: entry trap → set bp at `_square` → continue → hit
 * → step → continue → exit. Each step prints a structured line the
 * selftest matches.
 */

#include "libuser.h"
#include "../include/types.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- Tiny libc helpers ------------------------------------------ */

static int s_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int s_isspace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static uint32_t parse_hex(const char *s) {
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++;
        uint32_t d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | d;
    }
    return v;
}

/* ---- Symbol table ------------------------------------------------ */

#define MAX_SYMS  256
struct sym {
    uint32_t addr;
    char     name[40];
};
static struct sym g_syms[MAX_SYMS];
static int g_nsyms = 0;

/* Load `<prog>.syms` — one entry per line, "<hex8> <name>". The mingw
 * linker prefixes user names with `_`; strip on the way in so the user
 * can type `b main` not `b _main`. */
static int load_syms(const char *progname) {
    char path[64];
    int n = 0;
    for (int i = 0; progname[i] && n < 60; i++) path[n++] = progname[i];
    /* strip trailing ".elf" if present */
    if (n >= 4 && path[n-4]=='.' && path[n-3]=='e' && path[n-2]=='l' && path[n-1]=='f') n -= 4;
    const char *suf = ".syms";
    for (int i = 0; suf[i]; i++) path[n++] = suf[i];
    path[n] = 0;

    int fd = sys_open(path);
    if (fd < 0) {
        printf("dbg: WARN no symbols for %s (open %s failed)\n", progname, path);
        return -1;
    }

    static char buf[8192];
    int total = 0;
    for (;;) {
        int rn = sys_read(fd, buf + total, (int)sizeof(buf) - 1 - total);
        if (rn <= 0) break;
        total += rn;
        if (total >= (int)sizeof(buf) - 1) break;
    }
    sys_close(fd);
    buf[total] = 0;

    g_nsyms = 0;
    int p = 0;
    while (p < total && g_nsyms < MAX_SYMS) {
        /* skip leading whitespace */
        while (p < total && s_isspace(buf[p])) p++;
        if (p >= total) break;

        /* hex address */
        int hex_start = p;
        while (p < total && !s_isspace(buf[p])) p++;
        char saved = buf[p];
        buf[p] = 0;
        uint32_t addr = parse_hex(buf + hex_start);
        buf[p] = saved;

        /* name */
        while (p < total && s_isspace(buf[p])) p++;
        int name_start = p;
        while (p < total && !s_isspace(buf[p])) p++;
        int name_end = p;

        if (name_end <= name_start) continue;
        /* Skip section-name pseudo-symbols (".text", ".rdata", etc.) —
         * nm emits these alongside real T symbols and they'd otherwise
         * shadow the function symbol at the same address in
         * addr_to_sym lookups. */
        if (buf[name_start] == '.') continue;

        struct sym *s = &g_syms[g_nsyms++];
        s->addr = addr;
        int ns = name_start;
        if (buf[ns] == '_') ns++;        /* strip mingw underscore */
        int j = 0;
        while (ns < name_end && j < (int)sizeof(s->name) - 1) {
            s->name[j++] = buf[ns++];
        }
        s->name[j] = 0;

        /* skip to end of line */
        while (p < total && buf[p] != '\n') p++;
    }
    return g_nsyms;
}

static struct sym *find_sym(const char *name) {
    for (int i = 0; i < g_nsyms; i++) {
        if (s_streq(g_syms[i].name, name)) return &g_syms[i];
    }
    return NULL;
}

static const char *addr_to_sym(uint32_t addr, uint32_t *off_out) {
    /* Find the closest <= addr text symbol. */
    struct sym *best = NULL;
    for (int i = 0; i < g_nsyms; i++) {
        if (g_syms[i].addr <= addr) {
            if (!best || g_syms[i].addr > best->addr) best = &g_syms[i];
        }
    }
    if (!best) return NULL;
    if (off_out) *off_out = addr - best->addr;
    return best->name;
}

/* ---- Breakpoint table ------------------------------------------ */

#define MAX_BPS  16
struct bp {
    int      in_use;
    int      id;
    uint32_t addr;
    uint8_t  orig_byte;
};
static struct bp g_bps[MAX_BPS];
static int g_next_bp_id = 1;

static int g_child_pid = -1;

static int peek_bytes(uint32_t addr, void *buf, uint32_t n) {
    struct ptrace_args a = { .addr = addr, .size = n, .buf = buf, .regs = 0 };
    return sys_ptrace(PTRACE_PEEKDATA, g_child_pid, &a);
}
static int poke_bytes(uint32_t addr, const void *buf, uint32_t n) {
    struct ptrace_args a = { .addr = addr, .size = n, .buf = (void *)buf, .regs = 0 };
    return sys_ptrace(PTRACE_POKEDATA, g_child_pid, &a);
}
static int get_regs(struct ptrace_regs *r) {
    struct ptrace_args a = { .addr = 0, .size = 0, .buf = 0, .regs = r };
    return sys_ptrace(PTRACE_GETREGS, g_child_pid, &a);
}
static int set_regs(const struct ptrace_regs *r) {
    struct ptrace_args a = { .addr = 0, .size = 0, .buf = 0, .regs = (struct ptrace_regs *)r };
    return sys_ptrace(PTRACE_SETREGS, g_child_pid, &a);
}

/* Install a breakpoint: read original byte, write 0xCC. */
static int bp_set(uint32_t addr) {
    for (int i = 0; i < MAX_BPS; i++) {
        if (g_bps[i].in_use && g_bps[i].addr == addr) return g_bps[i].id;
    }
    int slot = -1;
    for (int i = 0; i < MAX_BPS; i++) if (!g_bps[i].in_use) { slot = i; break; }
    if (slot < 0) return -1;

    uint8_t orig = 0;
    if (peek_bytes(addr, &orig, 1) != 1) return -1;
    uint8_t cc = 0xCC;
    if (poke_bytes(addr, &cc, 1) != 1) return -1;

    g_bps[slot].in_use    = 1;
    g_bps[slot].id        = g_next_bp_id++;
    g_bps[slot].addr      = addr;
    g_bps[slot].orig_byte = orig;
    return g_bps[slot].id;
}

static struct bp *bp_at(uint32_t addr) {
    for (int i = 0; i < MAX_BPS; i++) {
        if (g_bps[i].in_use && g_bps[i].addr == addr) return &g_bps[i];
    }
    return NULL;
}

static int bp_delete(int id) {
    for (int i = 0; i < MAX_BPS; i++) {
        if (g_bps[i].in_use && g_bps[i].id == id) {
            poke_bytes(g_bps[i].addr, &g_bps[i].orig_byte, 1);
            g_bps[i].in_use = 0;
            return 0;
        }
    }
    return -1;
}

/* ---- Continuing past a breakpoint hit ----------------------------
 *
 * EIP on entry to this is (bp_addr + 1) — the CPU advanced past the
 * 0xCC byte. To resume cleanly we must:
 *   (a) write the original byte back at bp_addr so the actual
 *       instruction executes
 *   (b) rewind EIP to bp_addr
 *   (c) single-step ONE instruction with TF=1
 *   (d) after the step trap, re-plant 0xCC at bp_addr
 *   (e) PTRACE_CONT
 *
 * If no breakpoint exists at (EIP - 1) we treat this as a regular
 * trap and just resume — the dbgtest's entry `int $3` flows through
 * this path the first time. */
static int continue_past_trap(int do_step_only) {
    struct ptrace_regs r;
    if (get_regs(&r) < 0) return -1;

    uint32_t bp_addr = r.eip - 1;
    struct bp *b = bp_at(bp_addr);

    if (b) {
        /* Restore original byte, rewind EIP, single-step. */
        poke_bytes(bp_addr, &b->orig_byte, 1);
        r.eip = bp_addr;
        set_regs(&r);

        /* Step one instruction. */
        sys_ptrace(PTRACE_STEP, g_child_pid, 0);
        int sig = sys_ptrace(PTRACE_WAIT, g_child_pid, 0);
        if (sig == 0) return 0;        /* tracee exited */

        /* Re-arm the bp. */
        uint8_t cc = 0xCC;
        poke_bytes(bp_addr, &cc, 1);
    }

    if (do_step_only) {
        sys_ptrace(PTRACE_STEP, g_child_pid, 0);
    } else {
        sys_ptrace(PTRACE_CONT, g_child_pid, 0);
    }
    return 0;
}

/* ---- Pretty-print --------------------------------------------- */

static void print_regs(const struct ptrace_regs *r) {
    printf("  eax=%08x ebx=%08x ecx=%08x edx=%08x\n",
           r->eax, r->ebx, r->ecx, r->edx);
    printf("  esi=%08x edi=%08x ebp=%08x esp=%08x\n",
           r->esi, r->edi, r->ebp, r->esp);
    uint32_t off = 0;
    const char *sym = addr_to_sym(r->eip, &off);
    if (sym) {
        printf("  eip=%08x  <%s+0x%x>  eflags=%08x\n",
               r->eip, sym, off, r->eflags);
    } else {
        printf("  eip=%08x  eflags=%08x\n", r->eip, r->eflags);
    }
}

static void print_bps(void) {
    int any = 0;
    for (int i = 0; i < MAX_BPS; i++) {
        if (!g_bps[i].in_use) continue;
        uint32_t off = 0;
        const char *sym = addr_to_sym(g_bps[i].addr, &off);
        printf("  bp #%d @ %08x", g_bps[i].id, g_bps[i].addr);
        if (sym) printf("  <%s+0x%x>", sym, off);
        puts("\n");
        any = 1;
    }
    if (!any) puts("  (no breakpoints)\n");
}

static void print_mem(uint32_t addr, int n) {
    if (n <= 0) n = 16;
    if (n > 256) n = 256;
    uint8_t buf[256];
    int got = peek_bytes(addr, buf, n);
    if (got <= 0) {
        printf("  (unreadable @ %08x)\n", addr);
        return;
    }
    for (int i = 0; i < got; i += 16) {
        printf("  %08x:", addr + i);
        for (int j = 0; j < 16 && i + j < got; j++) printf(" %02x", buf[i + j]);
        puts("\n");
    }
}

/* ---- Interactive prompt --------------------------------------- */

static int parse_cmd_line(char *line, char *argv_out[8]) {
    int argc = 0;
    int i = 0;
    while (line[i] && argc < 8) {
        while (line[i] && s_isspace(line[i])) i++;
        if (!line[i]) break;
        argv_out[argc++] = &line[i];
        while (line[i] && !s_isspace(line[i])) i++;
        if (line[i]) { line[i] = 0; i++; }
    }
    return argc;
}

static int prompt_and_run(void) {
    /* Show context: regs at current stop. */
    struct ptrace_regs r;
    if (get_regs(&r) == 0) {
        uint32_t off = 0;
        const char *sym = addr_to_sym(r.eip, &off);
        if (sym) printf("\n*** stopped at <%s+0x%x>  eip=%08x ***\n", sym, off, r.eip);
        else     printf("\n*** stopped at eip=%08x ***\n", r.eip);
    }
    for (;;) {
        sys_write(1, "dbg> ", 5);
        char line[128];
        int n = sys_read_line(line, sizeof(line) - 1);
        if (n <= 0) return -1;
        line[n] = 0;
        if (line[n - 1] == '\n') line[--n] = 0;
        char *a[8];
        int argc = parse_cmd_line(line, a);
        if (argc == 0) continue;
        const char *cmd = a[0];

        if (s_streq(cmd, "regs") || s_streq(cmd, "r")) {
            struct ptrace_regs rr;
            if (get_regs(&rr) == 0) print_regs(&rr);
            else                    puts("  (regs unavailable — tracee running?)\n");
        }
        else if (s_streq(cmd, "syms") || s_streq(cmd, "s")) {
            printf("  %d symbol(s):\n", g_nsyms);
            for (int i = 0; i < g_nsyms; i++) {
                printf("    %08x  %s\n", g_syms[i].addr, g_syms[i].name);
            }
        }
        else if (s_streq(cmd, "break") || s_streq(cmd, "b")) {
            if (argc < 2) { puts("  usage: break <symbol|0xADDR>\n"); continue; }
            uint32_t addr;
            struct sym *sym = find_sym(a[1]);
            if (sym) addr = sym->addr;
            else     addr = parse_hex(a[1]);
            int id = bp_set(addr);
            if (id < 0) puts("  bp_set failed\n");
            else        printf("  bp #%d @ %08x  %s\n",
                               id, addr, sym ? sym->name : "(addr)");
        }
        else if (s_streq(cmd, "bps")) {
            print_bps();
        }
        else if (s_streq(cmd, "delete") || s_streq(cmd, "d")) {
            if (argc < 2) { puts("  usage: delete <bp-id>\n"); continue; }
            int id = (int)parse_hex(a[1]);
            if (bp_delete(id) == 0) printf("  deleted bp #%d\n", id);
            else                    puts("  no such bp\n");
        }
        else if (s_streq(cmd, "cont") || s_streq(cmd, "c")) {
            continue_past_trap(0);
            return 0;
        }
        else if (s_streq(cmd, "step") || s_streq(cmd, "si")) {
            continue_past_trap(1);
            return 0;
        }
        else if (s_streq(cmd, "mem") || s_streq(cmd, "x")) {
            if (argc < 2) { puts("  usage: mem <0xADDR> [count]\n"); continue; }
            uint32_t addr = parse_hex(a[1]);
            int n = argc >= 3 ? (int)parse_hex(a[2]) : 16;
            print_mem(addr, n);
        }
        else if (s_streq(cmd, "quit") || s_streq(cmd, "q")) {
            sys_kill(g_child_pid, SIGKILL);
            return 1;       /* meaning: stop debugging */
        }
        else {
            puts("  commands: regs|r  syms|s  break|b <sym>  bps  delete|d <id>\n"
                 "            cont|c  step|si  mem|x <0xADDR> [n]  quit|q\n");
        }
    }
}

/* ---- Auto script (used by the t40 selftest) -------------------- */

static void auto_script(void) {
    /* Wait for the entry trap (dbgtest issues `int $3` at main when
     * invoked with "trap"). */
    int sig = sys_ptrace(PTRACE_WAIT, g_child_pid, 0);
    printf("dbg-auto: entry trap, sig=%d\n", sig);

    struct ptrace_regs r;
    get_regs(&r);
    uint32_t off = 0;
    const char *sym = addr_to_sym(r.eip, &off);
    printf("dbg-auto: stopped at <%s+0x%x>  eip=%08x\n",
           sym ? sym : "?", off, r.eip);

    /* Set a breakpoint at square(). */
    struct sym *sq = find_sym("square");
    if (!sq) { puts("dbg-auto: no `square` symbol\n"); return; }
    int id = bp_set(sq->addr);
    printf("dbg-auto: bp #%d @ %08x <square>\n", id, sq->addr);

    /* Continue from entry trap. */
    sys_ptrace(PTRACE_CONT, g_child_pid, 0);

    /* Wait for square's first hit. */
    sig = sys_ptrace(PTRACE_WAIT, g_child_pid, 0);
    printf("dbg-auto: square hit, sig=%d\n", sig);
    get_regs(&r);
    off = 0; sym = addr_to_sym(r.eip - 1, &off);
    printf("dbg-auto: stopped at <%s+0x%x>  eip-1=%08x\n",
           sym ? sym : "?", off, r.eip - 1);

    /* Continue past the bp.  continue_past_trap re-arms the 0xCC so
     * subsequent calls to square() also stop, but we just let the
     * tracee run to completion here. */
    continue_past_trap(0);

    /* Drain the rest of the runs. We'll keep hitting square as the
     * loop iterates; just keep continuing. */
    int hits = 1;
    for (;;) {
        sig = sys_ptrace(PTRACE_WAIT, g_child_pid, 0);
        if (sig == 0) break;
        hits++;
        continue_past_trap(0);
    }
    printf("dbg-auto: total square hits = %d (expected 5)\n", hits);

    /* Read the data segment: g_counter symbol after exit.  Sadly
     * after exit the child's PD is gone; we can't peek. Instead we
     * inferred from the hit count.  Reap. */
    int code = 0;
    sys_wait(&code);
    printf("dbg-auto: dbgtest exited code=%d\n", code);
}

/* ---- main ------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: dbg [--auto] <target.elf> [args...]\n");
        puts("       --auto runs a scripted demo (used by [t40])\n");
        return 1;
    }

    int auto_mode = 0;
    int ai = 1;
    if (s_streq(argv[ai], "--auto")) { auto_mode = 1; ai++; }
    if (ai >= argc) { puts("dbg: missing target\n"); return 1; }
    const char *target = argv[ai++];

    /* Build child argv. dbgtest gets passed "trap" so it does the
     * entry int $3 by itself when we're in auto mode. */
    const char *cargv[8];
    int cn = 0;
    cargv[cn++] = target;
    if (auto_mode) cargv[cn++] = "trap";
    while (ai < argc && cn < 7) cargv[cn++] = argv[ai++];
    cargv[cn] = 0;

    if (load_syms(target) < 0) {
        puts("dbg: continuing without symbols\n");
    } else {
        printf("dbg: loaded %d symbol(s) for %s\n", g_nsyms, target);
    }

    int pid = sys_fork();
    if (pid == 0) {
        /* Child: opt in to being traced, then exec. The kernel will
         * keep tracer_pid set across exec — the dbgtest target then
         * issues `int $3` from main and we stop there. */
        if (sys_ptrace(PTRACE_TRACEME, 0, 0) < 0) {
            puts("dbg-child: TRACEME failed\n"); sys_exit(127);
        }
        sys_exec(target, cargv);
        puts("dbg-child: exec failed\n");
        sys_exit(127);
    }
    if (pid < 0) { puts("dbg: fork failed\n"); return 1; }
    g_child_pid = pid;
    printf("dbg: traced %s pid=%d\n", target, pid);

    if (auto_mode) {
        auto_script();
        return 0;
    }

    /* Interactive loop. */
    for (;;) {
        int sig = sys_ptrace(PTRACE_WAIT, g_child_pid, 0);
        if (sig == 0) {
            puts("dbg: tracee exited\n");
            int code = 0;
            sys_wait(&code);
            printf("dbg: exit code = %d\n", code);
            return 0;
        }
        int q = prompt_and_run();
        if (q) {
            int code = 0;
            sys_wait(&code);
            return 0;
        }
    }
}
