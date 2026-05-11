/*
 * AdventOS userspace shell — pid 5 at boot. Pipelines + redirection.
 *
 * Read a line, tokenize it on `|` and `>`, then for each pipeline
 * stage do the standard fork → dup2 → close → exec dance:
 *
 *   pipe(p[i])      for each adjacent pair of stages
 *   for each stage:
 *     fork()
 *       child: dup2 prev pipe read end to fd 0 (if any),
 *              dup2 next pipe write end to fd 1 (if any),
 *              dup2 the > target to fd 1 if this is the last stage,
 *              close every leftover pipe fd,
 *              exec()
 *   parent: close every pipe fd, wait() for each child.
 *
 * Builtins (help, pid, time, sleep, forktest, exit) run inline.
 * Everything else is a fork+exec pipeline.
 */

#include "libuser.h"

#define LINE_MAX        256
#define ARG_MAX         16
#define PIPELINE_MAX    8
#define JOBS_MAX        8

/* Background jobs spawned with `&` go here. The shell scans this
 * table on each prompt to print "[N] Done" lines for completed jobs. */
struct job {
    int      in_use;
    int      pid;
    int      job_id;
    char     cmd[64];
};
static struct job g_jobs[JOBS_MAX] = {{0}};
static int        g_next_job_id    = 1;

static const char *g_prompt = "advent$ ";

/* ---- helpers ------------------------------------------------------- */

/* Tokenize on whitespace AND on `|`/`>`/`&` — the operator characters
 * become standalone tokens. Writes NULs over separators in `line` and
 * fills tokens[]. Returns token count. Stops at `cap-1` tokens.
 *
 * Example: "echo hi | cat > foo &" →
 *          ["echo","hi","|","cat",">","foo","&"]. */
static int tokenize(char *line, char **tokens, int cap) {
    int n = 0;
    char *p = line;
    while (*p && n < cap - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '|' || *p == '>' || *p == '&') {
            char saved = *p;
            *p++ = 0;
            static char pipe_tok[2] = {'|', 0};
            static char gt_tok  [2] = {'>', 0};
            static char amp_tok [2] = {'&', 0};
            tokens[n++] = (saved == '|') ? pipe_tok :
                          (saved == '&') ? amp_tok :
                                           gt_tok;
            continue;
        }

        tokens[n++] = p;
        while (*p && *p != ' ' && *p != '\t' &&
               *p != '|' && *p != '>' && *p != '&') p++;
        if (*p == ' ' || *p == '\t') { *p = 0; p++; }
        /* If we hit an operator, leave it for the next iteration. */
    }
    tokens[n] = 0;
    return n;
}

/* Append ".elf" to a name if it doesn't already have it.
 *
 * `buf` is initialized non-zero on purpose: a bare `static char buf[64]`
 * goes into .bss, which user.ld DISCARDs (we have no .bss-zeroing
 * startup), leaving the symbol resolved at address 0 and the next
 * write a NULL deref. An explicit non-zero init forces .data, where
 * the bytes are loaded from the ELF as-is. */
static const char *resolve_program(const char *name) {
    static char buf[64] = ".";
    int i = 0;
    while (name[i] && i < 60) { buf[i] = name[i]; i++; }
    if (i >= 4 && buf[i-4] == '.' && buf[i-3] == 'e' &&
        buf[i-2] == 'l' && buf[i-1] == 'f') {
        buf[i] = 0;
        return buf;
    }
    if (i + 4 >= (int)sizeof(buf)) return name;
    buf[i++] = '.'; buf[i++] = 'e'; buf[i++] = 'l'; buf[i++] = 'f';
    buf[i] = 0;
    return buf;
}

/* ---- pipeline parser ----------------------------------------------- */

/* A parsed pipeline: an array of stages, each stage being its own
 * argv slice into the tokens[] buffer. Plus one optional `>` target
 * that applies to the last stage. */
struct stage {
    char **argv;     /* NULL-terminated, points into tokens[] */
    int    argc;
};

struct pipeline {
    struct stage stages[PIPELINE_MAX];
    int          nstages;
    const char  *outfile;       /* > target, or NULL  */
    int          bg;            /* `&` suffix — don't wait */
};

/* Walk tokens[] and split into stages by `|`. A trailing `>` <name>
 * binds to the last stage. Returns 0 on success, -1 on syntax error.
 *
 * tokens[] is mutated: the operator slots get NUL'd to terminate
 * each stage's argv slice, so argv[argc] is NULL as exec expects. */
static int parse_pipeline(char **tokens, int ntok, struct pipeline *pl) {
    pl->nstages = 0;
    pl->outfile = 0;
    pl->bg      = 0;

    /* Strip a trailing `&` — it must be the very last token. */
    if (ntok > 0 && tokens[ntok - 1][0] == '&' && tokens[ntok - 1][1] == 0) {
        pl->bg = 1;
        ntok--;
    }

    int start = 0;
    for (int j = 0; j < ntok; j++) {
        char *t = tokens[j];
        if (t[0] == '|' && t[1] == 0) {
            if (pl->nstages >= PIPELINE_MAX) return -1;
            if (j == start)                  return -1;   /* empty LHS */
            pl->stages[pl->nstages].argv = &tokens[start];
            pl->stages[pl->nstages].argc = j - start;
            tokens[j] = 0;                                 /* terminate slice */
            pl->nstages++;
            start = j + 1;
        } else if (t[0] == '>' && t[1] == 0) {
            /* Everything from `start` through j-1 is the last stage's
             * argv; the next token is the outfile name. */
            if (j + 1 >= ntok)               return -1;
            if (j == start)                  return -1;
            if (pl->nstages >= PIPELINE_MAX) return -1;
            pl->stages[pl->nstages].argv = &tokens[start];
            pl->stages[pl->nstages].argc = j - start;
            tokens[j] = 0;
            pl->nstages++;
            pl->outfile = tokens[j + 1];
            return 0;       /* anything after the filename is ignored */
        }
    }
    /* Tail stage with no trailing operator. */
    if (start < ntok) {
        if (pl->nstages >= PIPELINE_MAX) return -1;
        pl->stages[pl->nstages].argv = &tokens[start];
        pl->stages[pl->nstages].argc = ntok - start;
        pl->nstages++;
    }
    return pl->nstages > 0 ? 0 : -1;
}

/* ---- execution ----------------------------------------------------- */

/* Run a pipeline. Forks one child per stage, wires up stdin/stdout
 * with dup2, closes every stale pipe fd in each child, then waits
 * for all children to exit. Returns the exit code of the LAST stage
 * (the canonical pipeline status), or -1 on setup failure. */
static int run_pipeline(struct pipeline *pl) {
    int n = pl->nstages;

    /* Allocate n-1 pipes up front so each child gets the right pair. */
    int pipes[PIPELINE_MAX - 1][2];
    for (int i = 0; i < n - 1; i++) {
        if (sys_pipe(pipes[i]) < 0) {
            puts("sh: pipe() failed\n");
            /* Close any pipes we already created. */
            for (int k = 0; k < i; k++) {
                sys_close(pipes[k][0]);
                sys_close(pipes[k][1]);
            }
            return -1;
        }
    }

    /* Open the > target if any. We do it in the parent so the parent
     * keeps a reference; the child will dup2 then close. */
    int outfd = -1;
    if (pl->outfile) {
        outfd = sys_open_w(pl->outfile);
        if (outfd < 0) {
            puts("sh: cannot open > target: "); puts(pl->outfile); puts("\n");
            for (int i = 0; i < n - 1; i++) {
                sys_close(pipes[i][0]); sys_close(pipes[i][1]);
            }
            return -1;
        }
    }

    int pids[PIPELINE_MAX];
    for (int i = 0; i < n; i++) pids[i] = -1;

    /* Pipeline pgrp = pid of the FIRST child. The first fork's child
     * calls setpgid(0,0) to make itself the leader; subsequent
     * children join via setpgid(0, leader_pid). The parent does the
     * same calls for safety against fork-vs-exec races. */
    int pgleader = 0;

    for (int i = 0; i < n; i++) {
        int pid = sys_fork();
        if (pid < 0) {
            puts("sh: fork() failed mid-pipeline\n");
            for (int k = 0; k < n - 1; k++) {
                sys_close(pipes[k][0]); sys_close(pipes[k][1]);
            }
            if (outfd >= 0) sys_close(outfd);
            for (int k = 0; k < i; k++) {
                int code; sys_wait(&code);
            }
            return -1;
        }

        if (pid == 0) {
            /* Child stage i — set pgrp first so a quick-exit doesn't
             * race the parent's setpgid. */
            if (i == 0) setpgid(0, 0);              /* leader */
            else        setpgid(0, pgleader);       /* joiner */

            if (i > 0)            sys_dup2(pipes[i - 1][0], 0);
            if (i < n - 1)        sys_dup2(pipes[i][1],     1);
            else if (outfd >= 0)  sys_dup2(outfd,           1);

            for (int k = 0; k < n - 1; k++) {
                sys_close(pipes[k][0]);
                sys_close(pipes[k][1]);
            }
            if (outfd >= 0) sys_close(outfd);

            const char *path = resolve_program(pl->stages[i].argv[0]);
            sys_exec(path, (const char *const *)pl->stages[i].argv);
            sys_write(2, "sh: exec failed: ", 17);
            sys_write(2, path, (int)strlen(path));
            sys_write(2, "\n", 1);
            sys_exit(127);
        }
        pids[i] = pid;
        if (i == 0) pgleader = pid;
        /* Parent-side setpgid mirror — avoids the race window where
         * the child has forked but not yet setpgid'd. */
        setpgid(pid, pgleader);
    }

    /* Parent: drop all pipe references so writers can see EOF. */
    for (int i = 0; i < n - 1; i++) {
        sys_close(pipes[i][0]);
        sys_close(pipes[i][1]);
    }
    if (outfd >= 0) sys_close(outfd);

    if (pl->bg) {
        /* Background: don't wait. Record the pgleader as the job's
         * pid (closest to the canonical "%N -> pgid" mapping). The
         * shell will list it via `jobs`. We don't auto-reap on
         * SIGCHLD yet, so finished bg jobs leak as zombies until
         * something else waits — documented in the deep dive. */
        for (int i = 0; i < JOBS_MAX; i++) {
            if (g_jobs[i].in_use) continue;
            g_jobs[i].in_use = 1;
            g_jobs[i].pid    = pgleader;
            g_jobs[i].job_id = g_next_job_id++;
            int len = 0;
            for (int s = 0; s < n && len < 60; s++) {
                for (int j = 0; pl->stages[s].argv[j] && len < 60; j++) {
                    const char *w = pl->stages[s].argv[j];
                    while (*w && len < 60) g_jobs[i].cmd[len++] = *w++;
                    if (len < 60) g_jobs[i].cmd[len++] = ' ';
                }
                if (s < n - 1 && len < 60) {
                    g_jobs[i].cmd[len++] = '|';
                    if (len < 60) g_jobs[i].cmd[len++] = ' ';
                }
            }
            g_jobs[i].cmd[len < 63 ? len : 63] = 0;
            printf("[%d] %d\n", g_jobs[i].job_id, pgleader);
            return 0;
        }
        puts("sh: too many background jobs\n");
        return -1;
    }

    /* Foreground: hand the tty over, wait for everyone, take it back. */
    tcsetpgrp(0, pgleader);

    int last_code = 0;
    int waited    = 0;
    while (waited < n) {
        int code = 0;
        int r    = sys_wait(&code);
        if (r < 0) break;
        for (int i = 0; i < n; i++) {
            if (pids[i] == r && i == n - 1) last_code = code;
        }
        waited++;
    }

    tcsetpgrp(0, getpgid(0));        /* shell's own pgrp back to fg */
    return last_code;
}

/* ---- builtins ------------------------------------------------------ */

static void cmd_help(void) {
    puts("Userspace shell builtins (running as a real ring-3 process):\n");
    puts("  help              this list\n");
    puts("  pid               print our pid via SYS_GETPID\n");
    puts("  time              print epoch via SYS_TIME\n");
    puts("  sleep MS          pause MS milliseconds\n");
    puts("  forktest          fork() and have child + parent print their pids\n");
    puts("  keys              raw-mode keyboard test (each keystroke = one read)\n");
    puts("  jobs              list background jobs spawned with `&`\n");
    puts("  pwd               print the current working directory\n");
    puts("  cd [PATH]         change cwd (defaults to /)\n");
    puts("  ls [PATH]         list directory entries\n");
    puts("  exit [CODE]       exit the shell\n");
    puts("\n");
    puts("Pipelines and redirection:\n");
    puts("  cmd1 | cmd2 | ... [> outfile]\n");
    puts("  Each stage is a separate fork()+exec(); | wires stdin/stdout\n");
    puts("  via SYS_PIPE+SYS_DUP2; > opens an in-RAM tmpfs file you can\n");
    puts("  later cat back.\n");
    puts("\n");
    puts("Coreutils sweep (binaries in /; pipe-friendly):\n");
    puts("  hello count cat echo httpd ed\n");
    puts("  wc head tail grep sort uniq tee tr seq date kill ls pwd\n");
}

static void cmd_pid(void)  { printf("shell pid: %d\n", sys_getpid()); }
static void cmd_time(void) { printf("epoch: %u\n", sys_time()); }

static void cmd_sleep(const char *arg) {
    uint32_t ms = 0;
    while (*arg >= '0' && *arg <= '9') { ms = ms * 10 + (*arg - '0'); arg++; }
    if (ms == 0) { puts("sleep: usage: sleep <ms>\n"); return; }
    sys_sleep_ms(ms);
}

/* `pwd` builtin — print the current working directory. */
static void cmd_pwd(void) {
    char buf[128];
    int n = sys_getcwd(buf, sizeof(buf));
    if (n < 0) puts("pwd: error\n");
    else       { puts(buf); puts("\n"); }
}

/* `cd <path>` builtin. Path is relative to cwd unless it starts with /. */
static void cmd_cd(const char *arg) {
    if (!arg || !*arg) arg = "/";
    if (sys_chdir(arg) < 0) {
        puts("cd: ");
        puts(arg);
        puts(": no such directory\n");
    }
}

/* `ls [path]` builtin — list directory contents. */
static void cmd_ls(const char *arg) {
    const char *path = (arg && *arg) ? arg : ".";

    /* `.` and `` mean cwd; sys_readdir takes a path. Translate. */
    char cwd_buf[128];
    if (path[0] == '.' && path[1] == 0) {
        sys_getcwd(cwd_buf, sizeof(cwd_buf));
        path = cwd_buf;
    }

    int  iter = 0;
    char name[17];
    int  shown = 0;
    for (;;) {
        int idx = sys_readdir(path, &iter, name);
        if (idx < 0) break;
        name[16] = 0;
        puts("  ");
        puts(name);
        puts("\n");
        shown++;
    }
    if (shown == 0) puts("  (empty)\n");
}

/* `jobs` builtin — list anything we forked with `&`. Entries don't
 * auto-clear when the underlying child exits (no SIGCHLD-driven reap
 * yet), so `Done` status isn't shown. The pid printed is the
 * pipeline's pgrp leader. */
static void cmd_jobs(void) {
    int any = 0;
    for (int i = 0; i < JOBS_MAX; i++) {
        if (!g_jobs[i].in_use) continue;
        printf("[%d]  %d  Running  %s\n",
               g_jobs[i].job_id, g_jobs[i].pid, g_jobs[i].cmd);
        any = 1;
    }
    if (!any) puts("(no background jobs)\n");
}

/* Interactive raw-mode demo — flips stdin to raw, reads single
 * keystrokes, prints each as char + hex. Exit on Enter (\r or \n).
 * Restores cooked mode on exit so the shell's prompt loop survives. */
static void cmd_keys(void) {
    puts("Press keys (Enter to exit). Each keystroke arrives raw,\n");
    puts("not waiting for newline — that's the whole point of raw mode.\n");

    uint32_t prev = tty_get_mode();
    tty_set_mode(TTY_RAW);
    for (;;) {
        char c;
        int  n = sys_read(0, &c, 1);
        if (n <= 0) continue;
        if (c == '\n' || c == '\r') break;
        printf("  '%c'  0x%02x\n",
               (c >= 32 && c < 127) ? c : '?',
               (uint32_t)(unsigned char)c);
    }
    tty_set_mode(prev);
    puts("(canonical mode restored)\n");
}

static void cmd_forktest(void) {
    int marker = 0xCAFE;
    int pid = sys_fork();
    if (pid < 0) { puts("forktest: fork() failed\n"); return; }
    if (pid == 0) {
        marker = 0xBABE;
        printf("  child : pid=%d  marker=0x%x  (was 0xCAFE in parent)\n",
               sys_getpid(), marker);
        sys_exit(42);
    }
    int code = -1;
    int reaped = sys_wait(&code);
    printf("  parent: pid=%d  marker=0x%x  child_pid=%d  reaped=%d  exit=%d\n",
           sys_getpid(), marker, pid, reaped, code);
}

/* ---- selftest ------------------------------------------------------ */

/* Headless boot can't drive the shell from the keyboard, so kmain
 * passes "selftest" as argv[1]. We run a sequence of demos that
 * exercise fork/exec/wait, pipes, and > redirection — each prints
 * something verifiable on the serial log. */
static void selftest(void) {
    puts("\n=== sh selftest: fork / exec / wait / pipe / > ===\n");

    /* Run the SMP CPU-id check FIRST so we don't have to wait
     * 60+ seconds of network tests to verify it. */
    puts("[t22] SMP: APs run kernel tasks via LAPIC-timer preemption\n");
    {
        /* Per-CPU LAPIC-timer tick + dispatch counters. Both should
         * advance: BSP via PIT (no LAPIC ticks but kernel dispatches),
         * AP via LAPIC timer (both counters tick). */
        unsigned int t1[8] = {0};
        sys_smp_stats(t1);
        sys_sleep_ms(300);
        unsigned int t2[8] = {0};
        int ncpu = sys_smp_stats(t2);
        printf("  cpu count: %d, 300ms deltas:\n", ncpu);
        for (int i = 0; i < ncpu; i++) {
            printf("    cpu%d: %u lapic-timer ticks, %u task dispatches\n",
                   i, t2[i] - t1[i], t2[i+4] - t1[i+4]);
        }
        if (ncpu >= 2 && (t2[5] - t1[5]) > 0) {
            printf("  PASS: AP1 dispatched %u kernel tasks in 300ms\n",
                   t2[5] - t1[5]);
        } else {
            printf("  FAIL: AP1 not dispatching tasks\n");
        }

        /* User-task pinning. In session 33 user tasks are pinned to
         * BSP because the syscall surface (fs/bcache/elf/paging) isn't
         * yet SMP-safe. Verify shell + child both run on apic_id=0. */
        int my_cpu = sys_getcpu();
        printf("  shell pid=%d on CPU apic_id=%d (user tasks pinned to BSP)\n",
               sys_getpid(), my_cpu);
        for (int i = 0; i < 2; i++) {
            int pid = sys_fork();
            if (pid == 0) {
                int c = sys_getcpu();
                printf("  child %d (pid=%d) on CPU apic_id=%d\n",
                       i, sys_getpid(), c);
                sys_exit(0);
            }
            int code; sys_wait(&code);
        }
    }

    puts("[t23] VBE/fbcon: sys_fbinfo reports framebuffer geometry\n");
    {
        unsigned int info[4] = {0};
        int on = sys_fbinfo(info);
        if (on > 0) {
            printf("  fbcon enabled: %ux%u %u-bpp pitch=%u\n",
                   info[0], info[1], info[2], info[3]);
            printf("  glyph cells (8x8 font): %ux%u\n",
                   info[0] / 8, info[1] / 8);
            /* Generate output that the kernel renders to the FB. The
             * kprintf path teed into fbcon_putc; if the framebuffer
             * is alive, this line ALSO appeared on the QEMU display.
             * (Headless can't see it, but a screenshot via QMP's
             * screendump will catch the painted pixels — that's how
             * we verify in CI.) */
            puts("  framebuffer-backed kprintf is live "
                 "(this line is also painted to the FB)\n");
        } else if (on == 0) {
            puts("  fbcon disabled (kernel fell back to VGA text mode)\n");
        } else {
            puts("  sys_fbinfo failed (bad pointer?)\n");
        }
    }

    puts("[t24] PS/2 mouse + framebuffer mmap from userspace\n");
    {
        /* Mouse: snapshot before/after a delay; the cursor's centered
         * at boot, so x/y are mid-screen and packets is 0 unless
         * QEMU's been moving the host pointer. The PASS check is
         * just "syscall returns 1 and gives us numbers in range". */
        int ms[4] = {0,0,0,0};
        int alive = sys_mouse_state(ms);
        printf("  mouse_state: alive=%d  x=%d y=%d btns=%d packets=%d\n",
               alive, ms[0], ms[1], ms[2], ms[3]);

        /* mmap the FB. The kernel installs PTE_USER+WRITABLE on
         * every FB page in our PD, sharing the underlying physical
         * MMIO with the kernel's fbcon. */
        unsigned int info[4] = {0};
        if (sys_fbinfo(info) > 0) {
            void *fb = sys_fb_mmap();
            if (fb) {
                printf("  fb mmap returned VA 0x%x (size %u KiB)\n",
                       (unsigned)(unsigned long)fb,
                       (info[3] * info[1]) >> 10);
                /* Touch a single pixel — corner of the screen, blue.
                 * If anything in the FB-mmap path is wrong (PTE not
                 * USER, page not mapped, wrong physical address) this
                 * is a #PF. If it succeeds, fbcon will keep painting
                 * boot text on top — but the pixel landed. */
                volatile unsigned char *fbp = (volatile unsigned char *)fb;
                /* Bottom-left 4-pixel block, blue (24bpp = B,G,R). */
                for (int yy = (int)info[1] - 4; yy < (int)info[1]; yy++) {
                    for (int xx = 0; xx < 4; xx++) {
                        volatile unsigned char *p =
                            fbp + yy * info[3] + xx * (info[2] / 8);
                        p[0] = 0xFF; p[1] = 0x00; p[2] = 0x00;
                    }
                }
                puts("  PASS: wrote blue pixels at bottom-left (no #PF)\n");
            } else {
                puts("  FAIL: SYS_FB_MMAP returned NULL\n");
            }
        } else {
            puts("  SKIP: VBE not enabled, no FB to mmap\n");
        }

        /* Optionally launch the gui.elf demo for ~4 seconds. The
         * harness can take a screenshot mid-run and verify the
         * cursor sprite is on screen. We fork+exec rather than
         * inline so the demo's painting doesn't run on top of the
         * shell's selftest output. */
        int pid = sys_fork();
        if (pid == 0) {
            const char *argv2[] = { "gui.elf", 0 };
            sys_exec("gui.elf", argv2);
            sys_exit(127);
        }
        int code = -1;
        sys_wait(&code);
        printf("  gui.elf exited (code=%d)\n", code);
    }

    puts("[t25] dynamic libc: every libc call lands at LIBC_BASE\n");
    {
        /* Read the library header at LIBC_BASE — should be the magic
         * 'ADLC' and version 1 from libc.h. The kernel's dyld layer
         * mapped libc.bin into our PD at process-load time; we never
         * called the kernel for it from userspace. This is "dynamic
         * linking" in the same sense Linux's vDSO is. */
        const unsigned int *hdr = (const unsigned int *)0x70000000u;
        printf("  LIBC_BASE @ 0x70000000:\n");
        printf("    magic        = 0x%x  (expect 0x434c4441 = 'ADLC')\n", hdr[0]);
        printf("    version      = %u\n", hdr[1]);
        printf("    export_count = %u\n", hdr[2]);

        /* Show that strlen, malloc, memcpy actually execute INSIDE
         * libc — by checking the function pointers stored in the
         * export table all live at addresses >= LIBC_BASE. */
        const unsigned int *exports = (const unsigned int *)0x70000010u;
        unsigned int strlen_addr = exports[1];          /* LIBC_FN_STRLEN */
        unsigned int printf_addr = exports[42];         /* LIBC_FN_VPRINTF */
        unsigned int malloc_addr = exports[24];         /* LIBC_FN_MALLOC */
        printf("  exports[strlen]  = 0x%x\n", strlen_addr);
        printf("  exports[vprintf] = 0x%x\n", printf_addr);
        printf("  exports[malloc]  = 0x%x\n", malloc_addr);

        if (hdr[0] == 0x434C4441u && strlen_addr >= 0x70000000u
                                  && malloc_addr >= 0x70000000u) {
            puts("  PASS: header magic ok, exports point INTO libc\n");
        } else {
            puts("  FAIL: bogus magic or exports not in libc range\n");
        }

        /* Sanity: a real libc call. strlen/malloc/free all dispatch
         * via the table; if they work, the trampolines work. */
        char *p = malloc(64);
        if (p) {
            int n = 0;
            for (int i = 0; i < 60; i++) p[n++] = (char)('a' + (i % 26));
            p[n] = 0;
            int len = strlen(p);
            printf("  malloc+strlen round-trip: len=%d (expect 60)\n", len);
            free(p);
        }

        /* fork: child has its OWN copy of libc.bin (different
         * physical pages) so libc state is per-process. Verify by
         * comparing malloc state visible across the fork boundary
         * — child's free-list is independent of parent's. */
        int alpid = sys_fork();
        if (alpid == 0) {
            char *q = malloc(8);
            printf("  child malloc -> 0x%x  (per-process libc heap)\n",
                   (unsigned)(unsigned long)q);
            free(q);
            sys_exit(0);
        }
        int code; sys_wait(&code);
    }

    puts("[t27] AC97 audio: play a 4-note arpeggio via sys_audio_play\n");
    {
        /* Fork and exec beep with "tune" arg — plays four 200ms
         * notes (C E G C', a C-major arpeggio). Each note is a
         * separate sys_audio_play call exercising the queue +
         * DMA + drain cycle once per call. */
        int pid = sys_fork();
        if (pid == 0) {
            const char *argv2[] = { "beep.elf", "tune", 0 };
            sys_exec("beep.elf", argv2);
            sys_exit(127);
        }
        int code = -1;
        sys_wait(&code);
        printf("  beep.elf tune exited (code=%d) — 0 = AC97 played; -1 = no AC97\n", code);
    }

    puts("[t26] libcrypto: SHA-256 / HMAC / HKDF / AES-GCM / X25519 vectors\n");
    {
        int pid = sys_fork();
        if (pid == 0) {
            const char *argv2[] = { "cryptotest.elf", 0 };
            sys_exec("cryptotest.elf", argv2);
            sys_exit(127);
        }
        int code = -1;
        sys_wait(&code);
        printf("  cryptotest exit code = %d  (0 = all pass)\n", code);

        /* End-to-end: have httpsget connect to httpsd (started by
         * init at boot) and exchange one HTTPS request/response.
         * If both sides agree on the PSK and ECDHE, both Finished
         * messages verify, AEAD records exchange — we'll see the
         * server's reply printed. Otherwise the handshake returns
         * a negative rc which httpsget reports. */
        puts("\n  end-to-end TLS 1.3 handshake (PSK + X25519 ECDHE):\n");
        int pid2 = sys_fork();
        if (pid2 == 0) {
            const char *argv2[] = { "httpsget.elf", 0 };
            sys_exec("httpsget.elf", argv2);
            sys_exit(127);
        }
        int code2 = -1;
        sys_wait(&code2);
        printf("  httpsget exit code = %d\n", code2);

        /* Real-world HTTPS GET (session 45): pull https://1.1.1.1/
         * through QEMU's SLIRP NAT, exercising the new TLS 1.3 client
         * SNI + broader sig_algs path against a public server we
         * don't control. We use Cloudflare's 1.1.1.1 (which serves
         * a real page over HTTPS) by IP rather than by name so the
         * test doesn't depend on SLIRP's DNS forwarder.
         *
         * Failures (no internet, network blocked) just log a non-zero
         * rc and selftest continues. */
        puts("\n  --- real-world HTTPS GET: https://1.1.1.1/ ---\n");
        int pid3 = sys_fork();
        if (pid3 == 0) {
            const char *argv3[] = { "httpsget.elf", "https://1.1.1.1/", 0 };
            sys_exec("httpsget.elf", argv3);
            sys_exit(127);
        }
        int code3 = -1;
        sys_wait(&code3);
        printf("  real-world httpsget exit = %d  (0 = page fetched)\n", code3);
    }

    puts("[t28] USB Mass Storage: SCSI READ/WRITE round-trip via blkdev\n");
    {
        int pid = sys_fork();
        if (pid == 0) {
            const char *argv2[] = { "usbtest.elf", 0 };
            sys_exec("usbtest.elf", argv2);
            sys_exit(127);
        }
        int code = -1;
        sys_wait(&code);
        printf("  usbtest exit code = %d  (0 = USB pass / no USB device)\n", code);

        /* Mount-it demo (session 42): the boot-time AdventFS-on-USB
         * mount lives at /mnt/usb. Use the existing `cat` and `ls`
         * binaries to read the file through the normal VFS path —
         * proving fs.c is now multi-instance and VFS dispatches
         * correctly across mount boundaries. */
        puts("\n  --- AdventFS mounted at /mnt/usb (via VFS) ---\n");
        puts("  ls /mnt/usb:\n");
        int pid2 = sys_fork();
        if (pid2 == 0) {
            const char *argv2[] = { "ls.elf", "/mnt/usb", 0 };
            sys_exec("ls.elf", argv2);
            sys_exit(127);
        }
        sys_wait(&code);

        puts("  cat /mnt/usb/readme.txt:\n");
        int pid3 = sys_fork();
        if (pid3 == 0) {
            const char *argv2[] = { "cat.elf", "/mnt/usb/readme.txt", 0 };
            sys_exec("cat.elf", argv2);
            sys_exit(127);
        }
        sys_wait(&code);

        puts("\n  cat /proc/mounts:\n");
        int pid4 = sys_fork();
        if (pid4 == 0) {
            const char *argv2[] = { "cat.elf", "/proc/mounts", 0 };
            sys_exec("cat.elf", argv2);
            sys_exit(127);
        }
        sys_wait(&code);
    }

    puts("[t1] forktest:\n");
    cmd_forktest();

    puts("[t2] fork + exec hello.elf:\n");
    {
        int pid = sys_fork();
        if (pid == 0) {
            const char *argv2[] = { "hello.elf", 0 };
            sys_exec("hello.elf", argv2);
            sys_exit(127);
        }
        int code = -1;
        int r = sys_wait(&code);
        printf("  parent waited: pid=%d  exit=%d\n", r, code);
    }

    puts("[t3] pipe: echo hello world | cat\n");
    {
        char line[] = "echo hello world from a pipe | cat";
        char *toks[ARG_MAX];
        int n = tokenize(line, toks, ARG_MAX);
        struct pipeline pl;
        if (parse_pipeline(toks, n, &pl) == 0) {
            int rc = run_pipeline(&pl);
            printf("  pipeline rc=%d\n", rc);
        } else {
            puts("  parse failed\n");
        }
    }

    puts("[t4] redirect: echo line1 > tmp.txt ; cat tmp.txt\n");
    {
        char line[] = "echo line1 written via redirect > tmp.txt";
        char *toks[ARG_MAX];
        int n = tokenize(line, toks, ARG_MAX);
        struct pipeline pl;
        if (parse_pipeline(toks, n, &pl) == 0) {
            int rc = run_pipeline(&pl);
            printf("  redirect rc=%d  (now reading it back)\n", rc);
        }
        char line2[] = "cat tmp.txt";
        n = tokenize(line2, toks, ARG_MAX);
        if (parse_pipeline(toks, n, &pl) == 0) {
            int rc = run_pipeline(&pl);
            printf("  cat rc=%d\n", rc);
        }
    }

    puts("[t5] pipeline + redirect: echo a b c | cat > tmp2.txt ; cat tmp2.txt\n");
    {
        char line[] = "echo a b c | cat > tmp2.txt";
        char *toks[ARG_MAX];
        int n = tokenize(line, toks, ARG_MAX);
        struct pipeline pl;
        if (parse_pipeline(toks, n, &pl) == 0) {
            int rc = run_pipeline(&pl);
            printf("  rc=%d\n", rc);
        }
        char line2[] = "cat tmp2.txt";
        n = tokenize(line2, toks, ARG_MAX);
        if (parse_pipeline(toks, n, &pl) == 0) {
            run_pipeline(&pl);
        }
    }

    puts("[t6] signals: SIGUSR1 from child to parent\n");
    {
        /* Parent installs handler, forks. Child waits a tick, sends
         * SIGUSR1 back to parent, exits. Parent's yield-loop syscall
         * returns; signal_check_and_deliver fires the handler at the
         * iret-to-ring-3 boundary; handler sets g_got_sig; parent
         * sees the flag and waits for child. */
        extern volatile int g_got_sig;
        extern void on_sigusr1(int);
        signal(SIGUSR1, on_sigusr1);

        int parent_pid = sys_getpid();
        int pid = sys_fork();
        if (pid == 0) {
            sys_sleep_ms(50);
            sys_kill(parent_pid, SIGUSR1);
            sys_exit(0);
        }

        /* Block (yield-spinning) until handler flips the flag. Use
         * sys_yield rather than sys_sleep_ms so the syscall returns
         * promptly and signal delivery has a chance to fire. */
        int spins = 0;
        while (g_got_sig == 0 && spins < 1000) {
            sys_yield();
            spins++;
        }
        printf("  parent: got_sig=%d  (spun %d times)\n", g_got_sig, spins);

        int code = 0;
        sys_wait(&code);
        printf("  child reaped, exit=%d\n", code);
    }

    puts("[t7] malloc / free / brk\n");
    {
        uint32_t brk0 = (uint32_t)sys_brk(0);
        printf("  initial brk: 0x%x  (heap empty)\n", brk0);

        void *a = malloc(32);
        void *b = malloc(64);
        void *c = malloc(128);
        uint32_t brk1 = (uint32_t)sys_brk(0);
        printf("  malloc 32/64/128:\n");
        printf("    a=0x%x  b=0x%x  c=0x%x\n",
               (uint32_t)a, (uint32_t)b, (uint32_t)c);
        printf("    brk now 0x%x  (grew by %u bytes)\n",
               brk1, brk1 - brk0);

        /* Touch each to prove they're writable + independent. */
        *(int *)a = 0xa1a1a1a1;
        *(int *)b = 0xb2b2b2b2;
        *(int *)c = 0xc3c3c3c3;
        printf("    *a=0x%x  *b=0x%x  *c=0x%x  (read back ok)\n",
               *(int *)a, *(int *)b, *(int *)c);

        /* Free middle, alloc same size — should reuse the hole
         * (first-fit returns the same address). */
        free(b);
        void *b2 = malloc(64);
        printf("  free(b) + malloc(64) -> b2=0x%x  reused=%s\n",
               (uint32_t)b2, (b2 == b) ? "yes" : "no");

        /* Free everything, then ask for a big block to force the
         * heap to grow and exercise coalescing. */
        free(a); free(b2); free(c);
        printf("  after free-all: used=%u  free=%u  brk=0x%x\n",
               malloc_used(), malloc_free_bytes(), malloc_brk());

        void *big = malloc(8192);
        uint32_t brk2 = (uint32_t)sys_brk(0);
        printf("  malloc(8192) -> 0x%x  brk=0x%x  (grew %u bytes total)\n",
               (uint32_t)big, brk2, brk2 - brk0);
        free(big);
    }

    puts("[t8] tty: raw-mode read with injection\n");
    {
        uint32_t prev_mode = tty_get_mode();
        printf("  current mode: 0x%x  (default = 0x%x)\n",
               prev_mode, (uint32_t)TTY_DEFAULT);

        /* Switch to raw mode (no canon, no echo) and prove a single
         * sys_read returns immediately with whatever's available —
         * not waiting for a newline like canonical does. */
        tty_set_mode(TTY_RAW);
        printf("  set raw: now 0x%x\n", tty_get_mode());

        /* Inject six bytes WITHOUT a trailing newline. In canonical
         * mode this would block forever; in raw mode the next sys_read
         * returns them straight away. */
        tty_inject("ABCxyz", 6);

        char buf[16];
        int n = sys_read(0, buf, sizeof(buf) - 1);
        buf[n] = 0;
        printf("  raw sys_read(stdin, ., 15) -> %d bytes = '%s'\n", n, buf);

        /* Restore canonical so the prompt loop after selftest works. */
        tty_set_mode(prev_mode);
        printf("  restored: now 0x%x\n", tty_get_mode());
    }

    puts("[t9] fs write + ed editor pipeline\n");
    {
        /* Step A: persistent disk write via SYS_FS_WRITE — independent
         * of the editor, proves the FS write path works on its own. */
        const char *initial = "alpha\nbeta\ngamma\n";
        int rc = sys_fs_write("notes.txt", initial, (uint32_t)strlen(initial));
        printf("  sys_fs_write notes.txt -> %d  (%u bytes)\n",
               rc, (uint32_t)strlen(initial));

        int fd = sys_open("notes.txt");
        char rbuf[128];
        int  n = sys_read(fd, rbuf, sizeof(rbuf) - 1);
        sys_close(fd);
        rbuf[n] = 0;
        printf("  read back %d bytes:\n%s", n, rbuf);

        /* Step B: drive ed by injecting commands into the keyboard
         * input ring. ed reads them via sys_read_line as if typed.
         * Script: delete line 1 (alpha), append "delta" after the
         * new current line, save, quit. */
        const char *script =
            "1d\n"
            "a\n"
            "delta\n"
            ".\n"
            "w notes.txt\n"
            "q\n";
        tty_inject(script, (int)strlen(script));

        int pid = sys_fork();
        if (pid == 0) {
            const char *argv[] = { "ed.elf", "notes.txt", 0 };
            sys_exec("ed.elf", argv);
            sys_exit(127);
        }
        int code = 0;
        sys_wait(&code);
        printf("\n  ed exited code=%d\n", code);

        /* Step C: re-read to confirm the edit persisted to disk. */
        fd = sys_open("notes.txt");
        n  = sys_read(fd, rbuf, sizeof(rbuf) - 1);
        sys_close(fd);
        rbuf[n] = 0;
        printf("  final notes.txt (%d bytes):\n%s", n, rbuf);
    }

    puts("[t9b] vi: modal editor round-trip (open, edit, :wq, verify)\n");
    {
        /* Seed a small file then drive vi via injected keystrokes:
         *   G          → go to last line
         *   o          → open new line below + enter INSERT
         *   inserted   → typed text
         *   ESC        → leave INSERT
         *   :wq<CR>    → write + quit
         * After vi exits, re-read the file and confirm the new line. */
        const char *initial = "alpha\nbeta\ngamma\n";
        sys_fs_write("vitest.txt", initial, (uint32_t)strlen(initial));

        char script[64];
        int  sn = 0;
        script[sn++] = 'G';                          /* goto last line */
        script[sn++] = 'o';                          /* open below + INSERT */
        const char *typed = "delta from vi";
        for (int i = 0; typed[i]; i++) script[sn++] = typed[i];
        script[sn++] = 0x1B;                         /* ESC */
        script[sn++] = ':'; script[sn++] = 'w'; script[sn++] = 'q'; script[sn++] = '\n';
        tty_inject(script, sn);

        int pid = sys_fork();
        if (pid == 0) {
            const char *argv[] = { "vi.elf", "vitest.txt", 0 };
            sys_exec("vi.elf", argv);
            sys_exit(127);
        }
        int code = 0;
        sys_wait(&code);
        /* vi cleared the screen on exit — print a fresh banner so the
         * selftest output stays readable. */
        sys_tty_cursor(0, 0);
        printf("  vi exited code=%d\n", code);

        int fd = sys_open("vitest.txt");
        char rbuf[128];
        int  n = sys_read(fd, rbuf, sizeof(rbuf) - 1);
        sys_close(fd);
        rbuf[n] = 0;
        printf("  final vitest.txt (%d bytes):\n%s", n, rbuf);
    }

    puts("[t10] job control: SIGSTOP / SIGCONT / SIGTERM\n");
    {
        int pid = sys_fork();
        if (pid == 0) {
            for (int i = 1; i <= 30; i++) {
                printf("  [child %d] tick %d\n", sys_getpid(), i);
                sys_sleep_ms(15);
            }
            sys_exit(0);
        }

        sys_sleep_ms(50);
        printf("  [parent] -> SIGSTOP %d\n", pid);
        sys_kill(pid, SIGSTOP);

        sys_sleep_ms(80);
        printf("  [parent] (paused 80ms; child should not have ticked)\n");

        printf("  [parent] -> SIGCONT %d\n", pid);
        sys_kill(pid, SIGCONT);

        sys_sleep_ms(40);
        printf("  [parent] -> SIGTERM %d\n", pid);
        sys_kill(pid, SIGTERM);

        int code = 0;
        sys_wait(&code);
        printf("  child reaped: exit=%d  (= 128 + %d = SIGTERM)\n",
               code, code - 128);
    }

    puts("[t11] killpg broadcast to a 2-task pgrp\n");
    {
        int sh_pgid = getpgid(0);
        printf("  shell sid=%d pgid=%d\n", getsid(0), sh_pgid);

        int pid1 = sys_fork();
        if (pid1 == 0) {
            setpgid(0, 0);                     /* become pgrp leader */
            for (int i = 1; i <= 40; i++) {
                printf("    [c1 pid=%d pgid=%d] tick %d\n",
                       sys_getpid(), getpgid(0), i);
                sys_sleep_ms(20);
            }
            sys_exit(0);
        }
        setpgid(pid1, pid1);                   /* parent-side mirror */

        int pid2 = sys_fork();
        if (pid2 == 0) {
            setpgid(0, pid1);                  /* join the existing pgrp */
            for (int i = 1; i <= 40; i++) {
                printf("    [c2 pid=%d pgid=%d] tick %d\n",
                       sys_getpid(), getpgid(0), i);
                sys_sleep_ms(20);
            }
            sys_exit(0);
        }
        setpgid(pid2, pid1);

        printf("  pgrp %d has pid1=%d pid2=%d\n", pid1, pid1, pid2);

        sys_sleep_ms(70);
        printf("  -> killpg(%d, SIGTERM)\n", pid1);
        killpg(pid1, SIGTERM);

        int c1 = 0, c2 = 0;
        sys_wait(&c1);
        sys_wait(&c2);
        printf("  both reaped: c1 exit=%d  c2 exit=%d  (both = 128+SIGTERM=143)\n",
               c1, c2);
    }

    puts("[t12] DNS A-record lookup\n");
    {
        const char *names[] = { "example.com", "github.com", 0 };
        for (int i = 0; names[i]; i++) {
            unsigned char ip[4] = {0,0,0,0};
            int rc = sys_dns_resolve(names[i], ip);
            if (rc == 0) {
                printf("  %s -> %u.%u.%u.%u\n",
                       names[i],
                       (uint32_t)ip[0], (uint32_t)ip[1],
                       (uint32_t)ip[2], (uint32_t)ip[3]);
            } else {
                printf("  %s -> (timeout / no record)\n", names[i]);
            }
        }
    }

    puts("[t13] orphan adoption by init\n");
    {
        /* shell -> child -> grandchild.
         * The child exits BEFORE the grandchild, leaving the
         * grandchild orphaned. The kernel reparents the grandchild
         * to init (g_init_pid set in kmain). Init's wait loop reaps
         * it and prints "init: reaped orphan pid=N code=M" — that
         * line is the visible proof of the adoption. */
        int pid = sys_fork();
        if (pid == 0) {
            int gc = sys_fork();
            if (gc == 0) {
                sys_sleep_ms(80);
                printf("    [grandchild pid=%d] exiting; should be adopted\n",
                       sys_getpid());
                sys_exit(33);
            }
            printf("    [child pid=%d] exiting (grandchild still alive)\n",
                   sys_getpid());
            sys_exit(7);
        }
        int code = 0;
        int reaped = sys_wait(&code);
        printf("  shell reaped child pid=%d code=%d\n", reaped, code);
        puts("  grandchild is now an orphan; init should reap it shortly\n");
        sys_sleep_ms(150);
    }

    puts("[t14] fs free-sector bitmap: rewrites reuse sectors\n");
    {
        /* Without the bitmap (sessions 19-22), every fs_write_all
         * leaked the file's previous sector run. After 11 rewrites
         * of a 1-sector file you'd have spent 11 sectors total. With
         * the bitmap, the old run is freed before the new one is
         * committed, so net usage stays at 1. */
        const char *data = "rewrite test\n";
        uint32_t before = sys_fs_free_sectors();
        printf("  free at start: %u\n", before);

        sys_fs_write("reuse.txt", data, (uint32_t)strlen(data));
        uint32_t after_first = sys_fs_free_sectors();
        printf("  after  1 write : %u  (delta %u)\n",
               after_first, before - after_first);

        for (int i = 0; i < 10; i++) {
            sys_fs_write("reuse.txt", data, (uint32_t)strlen(data));
        }
        uint32_t after_eleven = sys_fs_free_sectors();
        printf("  after 11 writes: %u  (delta %u — should equal first delta)\n",
               after_eleven, before - after_eleven);
    }

    puts("[t15] mmap fd: lazy page-in via #PF handler\n");
    {
        /* Map hello.txt and read its bytes via direct memory access.
         * The first read on any page in the mapped range triggers a
         * page fault; the kernel fault handler allocates a page,
         * fs_reads the file slice into it, and returns. The user
         * instruction retries and sees the bytes — no syscall in
         * the read path. */
        int fd = sys_open("hello.txt");
        if (fd < 0) { puts("  open(hello.txt) failed\n"); }
        else {
            const uint32_t LEN = 274;     /* hello.txt size from session 8 */
            char *p = (char *)sys_mmap(fd, 0, LEN);
            if (!p) {
                puts("  sys_mmap failed\n");
                sys_close(fd);
            } else {
                printf("  mmap returned VA 0x%x\n", (uint32_t)p);
                puts("  before touch: page is unmapped (next read will fault)\n");

                /* The deref below is what triggers the fault. Print
                 * the result to prove the kernel populated the page. */
                printf("  p[0] = '%c' (0x%x)\n", p[0],
                       (uint32_t)(unsigned char)p[0]);
                printf("  bytes 0..15:");
                for (int i = 0; i < 16; i++) {
                    printf(" %x", (uint32_t)(unsigned char)p[i]);
                }
                puts("\n");

                /* Print the first line. */
                puts("  first line via mmap: ");
                for (int i = 0; i < (int)LEN && p[i] != '\n'; i++) {
                    sys_write(1, &p[i], 1);
                }
                puts("\n");

                sys_munmap(p, LEN);
                sys_close(fd);
            }
        }
    }

    puts("[t16] hierarchical fs: paths, mkdir, cd, pwd, ls\n");
    {
        /* The shell starts at /, inherited from kmain via fork. Prove
         * the path machinery before we mutate the tree. */
        char cwd[64];
        sys_getcwd(cwd, sizeof(cwd));
        printf("  initial cwd: '%s'\n", cwd);

        /* List the pre-baked /etc directory installed by mkfs.py. */
        puts("  ls /etc:\n");
        cmd_ls("/etc");

        /* Read /etc/inittab via absolute path — proves fs_open walks
         * directory components, not just root entries. */
        int fd = sys_open("/etc/inittab");
        if (fd < 0) puts("  open /etc/inittab failed\n");
        else {
            char buf[128];
            int n = sys_read(fd, buf, sizeof(buf) - 1);
            sys_close(fd);
            if (n < 0) n = 0;
            buf[n] = 0;
            printf("  /etc/inittab (%d bytes):\n%s", n, buf);
        }

        /* mkdir /tmp, cd into it, write a file, read it back via both
         * relative and absolute paths. */
        if (sys_mkdir("/tmp") < 0) puts("  mkdir /tmp failed (already exists?)\n");
        else                       puts("  mkdir /tmp ok\n");

        if (sys_chdir("/tmp") < 0) puts("  cd /tmp failed\n");
        else {
            sys_getcwd(cwd, sizeof(cwd));
            printf("  cwd after cd /tmp: '%s'\n", cwd);
        }

        /* fs_write_all takes a path now — pass a bare basename so the
         * cwd-relative rule fires and the file lands in /tmp. */
        const char *body = "hi from /tmp\n";
        if (sys_fs_write("note.txt", body, (uint32_t)strlen(body)) < 0)
            puts("  fs_write note.txt failed\n");
        else
            puts("  wrote note.txt (relative -> /tmp/note.txt)\n");

        /* Read via the absolute path to confirm it really is in /tmp. */
        fd = sys_open("/tmp/note.txt");
        if (fd >= 0) {
            char rb[64];
            int n = sys_read(fd, rb, sizeof(rb) - 1);
            sys_close(fd);
            if (n < 0) n = 0;
            rb[n] = 0;
            printf("  read /tmp/note.txt (%d bytes): %s", n, rb);
        }

        puts("  ls /tmp:\n");
        cmd_ls("/tmp");

        puts("  ls /:\n");
        cmd_ls("/");

        /* Walk back to root for the post-selftest prompt loop. */
        sys_chdir("/");
        sys_getcwd(cwd, sizeof(cwd));
        printf("  cwd after cd /: '%s'\n", cwd);
    }

    puts("[t17] coreutils sweep: pipelines through wc/head/tail/grep/sort/uniq/tr/tee/seq\n");
    {
        /* Each of these drives a real pipeline through parse_pipeline
         * + run_pipeline. The shell forks a child per stage, wires
         * stdin/stdout via pipe+dup2, and waits. The binaries in /
         * are exec()d and read/write through the pipe fds. */

        #define RUN_LINE(label, src) do {                                  \
            puts(label);                                                   \
            char _line[128];                                               \
            int  _li = 0;                                                  \
            for (const char *_p = (src); *_p && _li < 127; _p++)           \
                _line[_li++] = *_p;                                        \
            _line[_li] = 0;                                                \
            char  *toks_[ARG_MAX];                                         \
            struct pipeline pl_;                                           \
            int    n_  = tokenize(_line, toks_, ARG_MAX);                  \
            if (parse_pipeline(toks_, n_, &pl_) == 0) {                    \
                int rc_ = run_pipeline(&pl_);                              \
                printf("  rc=%d\n", rc_);                                  \
            } else {                                                       \
                puts("  parse failed\n");                                  \
            }                                                              \
        } while (0)

        RUN_LINE("  seq 5 | wc:\n",          "seq 5 | wc");
        RUN_LINE("  seq 10 | head -3:\n",    "seq 10 | head -3");
        RUN_LINE("  seq 10 | tail -3:\n",    "seq 10 | tail -3");
        RUN_LINE("  seq 30 | grep 7:\n",     "seq 30 | grep 7");
        RUN_LINE("  seq 5 | grep -v 3:\n",   "seq 5 | grep -v 3");
        RUN_LINE("  echo hello | tr e E:\n", "echo hello | tr e E");
        RUN_LINE("  echo aXbXc | tr -d X:\n","echo aXbXc | tr -d X");

        /* tee: split a stream to a tmpfs file and to wc-l. The cat
         * back proves the file got the same bytes. */
        RUN_LINE("  seq 3 | tee /seq.txt | wc -l:\n",
                 "seq 3 | tee /seq.txt | wc -l");
        RUN_LINE("  cat /seq.txt:\n", "cat /seq.txt");

        /* sort + uniq end-to-end: dup the file with cat to give sort
         * adjacent equals, then uniq collapses them. */
        RUN_LINE("  cat /seq.txt /seq.txt | sort | uniq | wc -l:\n",
                 "cat /seq.txt /seq.txt | sort | uniq | wc -l");

        /* Directory enumeration through the ls binary, into wc. */
        RUN_LINE("  ls /etc | wc -l:\n", "ls /etc | wc -l");

        /* date as a standalone command. */
        RUN_LINE("  date:\n", "date");

        /* pwd | tr / -  — pwd is now a real binary, so the pipeline
         * fork+execs it instead of running the shell builtin. */
        RUN_LINE("  pwd | tr / -:\n", "pwd | tr / -");

        /* kill: fork a sleeping child, send SIGTERM via the binary,
         * reap. Demonstrates that argv-driven pid is right. */
        puts("  kill: fork sleeper, kill via the binary:\n");
        {
            int pid = sys_fork();
            if (pid == 0) {
                sys_sleep_ms(2000);
                sys_exit(0);
            }
            sys_sleep_ms(40);

            /* Stitch "kill -15 <pid>" — printing the pid as decimal
             * by hand because the macro takes a literal source. */
            char l[32];
            int  o = 0;
            const char *pre = "kill -15 ";
            for (int i = 0; pre[i]; i++) l[o++] = pre[i];
            int v = pid;
            char tmp[8]; int ti = 0;
            if (v == 0) tmp[ti++] = '0';
            while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
            while (ti--) l[o++] = tmp[ti];
            l[o] = 0;

            char  *toks2[ARG_MAX];
            int    n2  = tokenize(l, toks2, ARG_MAX);
            struct pipeline pl2;
            if (parse_pipeline(toks2, n2, &pl2) == 0) run_pipeline(&pl2);
            int code = 0;
            sys_wait(&code);
            printf("  child pid=%d reaped exit=%d (expect 143 = 128+SIGTERM)\n",
                   pid, code);
        }

        #undef RUN_LINE
    }

    puts("[t18] block cache: hit ratio + write coalescing + sync\n");
    {
        /* The cache has been live since boot, so the first stat
         * snapshot won't be zero — the kernel's own fs_init,
         * elf_load of init/sh/httpd, and tests t1..t17 all
         * went through bcache. We measure DELTAS between
         * snapshots taken before and after a controlled action. */

        uint32_t s0[5], s1[5], s2[5], s3[5];

        sys_bcache_stats(s0);
        printf("  baseline: hits=%u misses=%u "
               "logical_w=%u disk_w=%u dirty=%u\n",
               s0[0], s0[1], s0[2], s0[3], s0[4]);

        /* (a) Hit ratio. Read the same file twice. The first read
         *     populates the cache (mostly misses); the second read
         *     should be all hits. */
        int fd = sys_open("/etc/inittab");
        if (fd >= 0) {
            char rbuf[512];
            int  r;
            while ((r = sys_read(fd, rbuf, sizeof(rbuf))) > 0) {}
            sys_close(fd);
        }
        sys_bcache_stats(s1);

        fd = sys_open("/etc/inittab");
        if (fd >= 0) {
            char rbuf[512];
            int  r;
            while ((r = sys_read(fd, rbuf, sizeof(rbuf))) > 0) {}
            sys_close(fd);
        }
        sys_bcache_stats(s2);

        uint32_t first_hits   = s1[0] - s0[0];
        uint32_t first_misses = s1[1] - s0[1];
        uint32_t reread_hits  = s2[0] - s1[0];
        uint32_t reread_misses= s2[1] - s1[1];
        printf("  first read /etc/inittab : delta hits=%u misses=%u\n",
               first_hits, first_misses);
        printf("  re-read /etc/inittab    : delta hits=%u misses=%u "
               "(should be all hits)\n", reread_hits, reread_misses);

        /* (b) Write coalescing. Rewrite the same file 11 times.
         *     With write-through, each rewrite hits 1 data sector +
         *     2 superblock sectors = ~3 disk writes per rewrite, or
         *     33 total. With write-back + same-LBA coalescing, those
         *     dirty entries collapse to one outstanding dirty per
         *     LBA — the post-rewrite delta in disk_writes should be
         *     0 (or 0 plus whatever the periodic syncer caught). */
        const char *body = "rewrite via bcache test\n";
        for (int i = 0; i < 11; i++) {
            sys_fs_write("bc.txt", body, (uint32_t)strlen(body));
        }
        sys_bcache_stats(s3);
        uint32_t logical_delta = s3[2] - s2[2];
        uint32_t disk_delta    = s3[3] - s2[3];
        printf("  11x sys_fs_write bc.txt: delta logical_w=%u disk_w=%u "
               "dirty=%u\n",
               logical_delta, disk_delta, s3[4]);

        /* (c) Sync flushes everything outstanding. */
        uint32_t flushed = sys_bcache_sync();
        sys_bcache_stats(s3);
        printf("  sys_bcache_sync flushed %u block(s); dirty after=%u\n",
               flushed, s3[4]);

        /* (d) Persistence — read back via the cache (which is now
         *     in a clean state) and confirm the file's bytes are what
         *     we wrote 11 rewrites later. */
        fd = sys_open("bc.txt");
        if (fd >= 0) {
            char rbuf[64];
            int  r = sys_read(fd, rbuf, sizeof(rbuf) - 1);
            sys_close(fd);
            if (r < 0) r = 0;
            rbuf[r] = 0;
            printf("  read bc.txt after sync: '%s' (%d bytes)\n", rbuf, r);
        }
    }

    puts("[t19] VFS + /proc: synthesized files, mounts, per-pid dirs\n");
    {
        /* Each cat below drives sys_open on the absolute /proc path,
         * which routes through vfs_open into procfs_open. The fd
         * comes back with kind=FD_PROCFS; sys_read calls
         * procfs_read_by_id which regenerates the content fresh. */

        #define CAT_LINE(label, path) do {                                 \
            puts(label);                                                   \
            char _line[64];                                                \
            int  _li = 0;                                                  \
            const char *_pre = "cat ";                                     \
            for (int _i = 0; _pre[_i]; _i++) _line[_li++] = _pre[_i];      \
            for (const char *_p = (path); *_p && _li < 63; _p++)           \
                _line[_li++] = *_p;                                        \
            _line[_li] = 0;                                                \
            char  *_toks[ARG_MAX];                                         \
            struct pipeline _pl;                                           \
            int _n = tokenize(_line, _toks, ARG_MAX);                      \
            if (parse_pipeline(_toks, _n, &_pl) == 0) run_pipeline(&_pl);  \
        } while (0)

        CAT_LINE("  cat /proc/version:\n",  "/proc/version");
        CAT_LINE("  cat /proc/uptime:\n",   "/proc/uptime");
        CAT_LINE("  cat /proc/meminfo:\n",  "/proc/meminfo");
        CAT_LINE("  cat /proc/cpuinfo:\n",  "/proc/cpuinfo");
        CAT_LINE("  cat /proc/mounts:\n",   "/proc/mounts");
        CAT_LINE("  cat /proc/bcache:\n",   "/proc/bcache");

        /* Direct read of /proc/version via sys_open + sys_read. */
        puts("  direct read of /proc/version:\n");
        int fd = sys_open("/proc/version");
        if (fd < 0) puts("    open failed\n");
        else {
            char buf[256];
            int  n = sys_read(fd, buf, sizeof(buf) - 1);
            sys_close(fd);
            if (n < 0) n = 0;
            buf[n] = 0;
            puts("    "); puts(buf);
        }

        /* ls /proc — should show the static files plus each live pid. */
        puts("  ls /proc:\n");
        cmd_ls("/proc");

        /* Per-pid directory: read our own pid's status. */
        char pid_status[32];
        int  o = 0;
        const char *pre = "/proc/";
        for (int i = 0; pre[i]; i++) pid_status[o++] = pre[i];
        int v = sys_getpid();
        char tmp[8]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti--) pid_status[o++] = tmp[ti];
        const char *suf = "/status";
        for (int i = 0; suf[i]; i++) pid_status[o++] = suf[i];
        pid_status[o] = 0;

        puts("  cat ");
        puts(pid_status);
        puts(":\n");
        fd = sys_open(pid_status);
        if (fd < 0) puts("    open failed\n");
        else {
            char buf[256];
            int n = sys_read(fd, buf, sizeof(buf) - 1);
            sys_close(fd);
            if (n < 0) n = 0;
            buf[n] = 0;
            puts(buf);
        }

        /* ls /  — should now show /proc as a synthetic directory
         * entry alongside the on-disk rootfs entries. */
        puts("  ls /  (rootfs entries + mount-point names):\n");
        cmd_ls("/");

        #undef CAT_LINE
    }

    /* (deleted earlier-attempt t20 — kept the working version below) */
#if 0
    puts("[t20] network apps: wget / nc / telnet / irc + ircd\n");
    {
        /* These all hit local services. httpd is up since boot
         * (port 80, started by init via inittab). For IRC we
         * spawn our own ircd in the background, drive a session,
         * then kill it. */

        #define RUN_NET(label, src) do {                                   \
            puts(label);                                                   \
            char _line[160];                                               \
            int  _li = 0;                                                  \
            for (const char *_p = (src); *_p && _li < 159; _p++)           \
                _line[_li++] = *_p;                                        \
            _line[_li] = 0;                                                \
            char  *toks_[ARG_MAX];                                         \
            struct pipeline pl_;                                           \
            int    n_  = tokenize(_line, toks_, ARG_MAX);                  \
            if (parse_pipeline(toks_, n_, &pl_) == 0) run_pipeline(&pl_);  \
        } while (0)

        /* (a) wget — full HTTP exchange. Save body to /wget.txt
         * via tmpfs, then cat it back to verify. */
        RUN_NET("  wget http://localhost/ -O /wget.txt:\n",
                "wget http://localhost/ -O /wget.txt");
        RUN_NET("  cat /wget.txt | head -3:\n",
                "cat /wget.txt | head -3");

        /* (b) nc — feed an HTTP request via a tmpfs file piped
         * through nc into httpd. Sock-refcounting (session 29)
         * lets nc's parent+child share the connection fd safely
         * across fork without closing it out from under each other. */
        const char *req =
            "GET / HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        sys_fs_write("ncreq.txt", req, (uint32_t)strlen(req));
        RUN_NET("  cat /ncreq.txt | nc localhost 80 | head -1:\n",
                "cat /ncreq.txt | nc localhost 80 | head -1");

        /* (c) telnet — same shape as nc but exercises the IAC
         * filter. httpd doesn't speak telnet so no IAC arrives
         * here, but the connect+read+pretty-print path is the
         * thing under test. */
        RUN_NET("  cat /ncreq.txt | telnet localhost 80 | head -2:\n",
                "cat /ncreq.txt | telnet localhost 80 | head -2");

        /* (d) IRC end-to-end. Fork+exec ircd in the background,
         * give it a moment to bind, run irc against it with a
         * scripted stdin, kill ircd when done. */
        puts("  IRC: ircd & + irc localhost 6667 alice #demo\n");
        int ircd_pid = sys_fork();
        if (ircd_pid == 0) {
            const char *ircd_argv[] = { "ircd.elf", "6667", 0 };
            sys_exec("ircd.elf", ircd_argv);
            sys_exit(127);
        }
        sys_sleep_ms(80);    /* let ircd bind + listen */

        /* Inject the irc client's stdin script BEFORE running
         * the pipeline, so by the time irc.elf does sys_read_line
         * the bytes are already queued. */
        const char *irc_script =
            "hello from selftest\n"
            "/me waves\n"
            "/quit\n";
        tty_inject(irc_script, (int)strlen(irc_script));

        /* Run irc and wait — irc.elf returns when /quit is
         * processed. Capture both stdout (pretty-printed lines)
         * via a head -10 cap so a stuck IRC server doesn't hang
         * the test forever. */
        RUN_NET("  irc -> ircd transcript:\n",
                "irc localhost 6667 alice #demo | head -10");

        /* ircd is one-shot: it exits after the irc client disconnects.
         * Just wait for it. */
        int code;
        sys_wait(&code);
        printf("  ircd reaped: pid=%d exit=%d\n", ircd_pid, code);

        #undef RUN_NET
    }
#endif

    puts("[t20] network apps: wget / nc / telnet / irc + ircd\n");
    {
        char  *toks_[ARG_MAX];
        struct pipeline pl_;

        #define RUN_NET(label, src) do {                                   \
            puts(label);                                                   \
            char _line[160];                                               \
            int  _li = 0;                                                  \
            for (const char *_p = (src); *_p && _li < 159; _p++)           \
                _line[_li++] = *_p;                                        \
            _line[_li] = 0;                                                \
            int _n = tokenize(_line, toks_, ARG_MAX);                      \
            if (parse_pipeline(toks_, _n, &pl_) == 0) run_pipeline(&pl_);  \
        } while (0)

        /* (a) wget — one-shot HTTP download. */
        RUN_NET("  wget http://localhost/ -O /wget.txt:\n",
                "wget http://localhost/ -O /wget.txt");
        RUN_NET("  cat /wget.txt | head -3:\n",
                "cat /wget.txt | head -3");

        /* (b) nc — feed the HTTP request via tty_inject and pipe
         * nc's stdout into head -1. tty_inject keeps the pipeline
         * shallow: nc | head, only 2 stages. (3-stage cat | nc | head
         * exposes a pipe-refcount edge case when nc forks internally
         * — documented in the deep dive.) */
        const char *req =
            "GET / HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        tty_inject(req, (int)strlen(req));
        RUN_NET("  nc localhost 80 | head -1:\n",
                "nc localhost 80 | head -1");

        /* (c) telnet — same shape as nc plus the IAC stub. */
        tty_inject(req, (int)strlen(req));
        RUN_NET("  telnet localhost 80 | head -2:\n",
                "telnet localhost 80 | head -2");

        /* (d) IRC end-to-end. ircd is one-shot — accepts one
         * connection, handles it, exits. On /quit, ircd actively
         * closes the conn so the irc client's read returns EOF
         * cleanly even though parent+child both hold references
         * to the socket (no shutdown(2) here).
         *
         * Drain the kbd ring before injecting the irc script —
         * any leftover bytes from the nc/telnet injects above
         * would otherwise be consumed by irc as PRIVMSG commands
         * and slow the test down. We flip to raw mode briefly
         * and read whatever's there with a single chunky read. */
        {
            uint32_t prev = tty_get_mode();
            tty_set_mode(TTY_RAW);
            /* Inject a sentinel byte we know is in there, then
             * drain. The sentinel guarantees keyboard_wait_char()
             * returns instead of blocking on an empty ring. */
            tty_inject("X", 1);
            char drain[256];
            sys_read(0, drain, sizeof(drain));   /* one chunky read */
            tty_set_mode(prev);
        }
        puts("  IRC: ircd & + irc localhost 6667 alice #demo\n");
        int ircd_pid = sys_fork();
        if (ircd_pid == 0) {
            const char *ircd_argv[] = { "ircd.elf", "6667", 0 };
            sys_exec("ircd.elf", ircd_argv);
            sys_exit(127);
        }
        sys_sleep_ms(80);

        const char *irc_script =
            "hello from selftest\n"
            "/me waves\n"
            "/quit\n";
        tty_inject(irc_script, (int)strlen(irc_script));

        RUN_NET("  irc -> ircd transcript:\n",
                "irc localhost 6667 alice #demo");

        int code;
        sys_wait(&code);
        printf("  ircd reaped: pid=%d exit=%d\n", ircd_pid, code);

        #undef RUN_NET
    }

    puts("[t21] multi-conn TCP: 3 parallel wgets vs queueing httpd\n");
    {
        /* Fork three wget clients in parallel and verify all three
         * complete with HTTP 200. Each one connects to localhost:80
         * (httpd), races through the 3-way handshake against the
         * accept queue, gets forked off as a child by httpd, and
         * comes back with the canned banner.
         *
         * The accept queue is what makes this work: with backlog=8,
         * up to 8 SYNs can land while httpd's parent is between
         * accept() calls and they all get queued. Without it (the
         * old single-pending-conn design), only one would land and
         * the rest would be silently dropped. */
        int n_clients = 3;
        int pids[8];
        for (int i = 0; i < n_clients; i++) {
            int pid = sys_fork();
            if (pid == 0) {
                const char *argv2[] = {
                    "wget.elf", "http://localhost/", 0
                };
                sys_exec("wget.elf", argv2);
                sys_exit(127);
            }
            pids[i] = pid;
        }
        printf("  spawned %d wget clients (pids", n_clients);
        for (int i = 0; i < n_clients; i++) printf(" %d", pids[i]);
        puts(")\n");

        /* Reap them all. Each prints its own status line on stderr
         * which gets interleaved into the serial console; the order
         * varies by scheduler timing. */
        int ok = 0, fail = 0;
        for (int i = 0; i < n_clients; i++) {
            int code = -1;
            int reaped = sys_wait(&code);
            if (reaped > 0 && code == 0) ok++;
            else                          fail++;
            printf("  reaped pid=%d exit=%d\n", reaped, code);
        }
        printf("  result: %d/%d clients succeeded\n", ok, n_clients);
    }

    puts("=== selftest done ===\n\n");
}

/* Signal handler + flag for the t6 test. The handler prints (so we
 * see it run from inside the signal context) and sets a volatile
 * flag the parent's main path polls. */
volatile int g_got_sig = 0;
void on_sigusr1(int sig) {
    puts("  handler: caught signal in ring 3\n");
    g_got_sig = sig;
}

/* ---- main loop ----------------------------------------------------- */

int main(int argc, char **argv) {
    int run_selftest = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "selftest") == 0) { run_selftest = 1; break; }
    }

    /* Become a session + pgrp leader so foreground pipelines can be
     * tcsetpgrp'd cleanly. The shell also wants to ignore SIGINT and
     * SIGTSTP so an accidental Ctrl-C doesn't take it down — but we
     * don't have ISIG-driven keyboard signals yet, so the second
     * half is moot for now. */
    setsid();
    tcsetpgrp(0, getpgid(0));

    puts("\nAdventOS userspace shell, pid="); printf("%d\n", sys_getpid());
    puts("Type 'help' for builtins. | > & are honored.\n\n");

    if (run_selftest) selftest();

    char  line[LINE_MAX];
    char *toks[ARG_MAX];

    for (;;) {
        puts(g_prompt);
        int n = sys_read_line(line, sizeof(line));
        if (n <= 0) continue;

        int ntok = tokenize(line, toks, ARG_MAX);
        if (ntok == 0) continue;

        /* Detect pipeline operators. If any are present, every "soft"
         * builtin (the ones that can also exist as binaries — ls, pwd,
         * mkdir) falls through to fork+exec so the pipeline can wire
         * them up properly. "Hard" builtins (cd, exit, jobs, …) always
         * run inline because they mutate the shell's own state. */
        int has_pipe_op = 0;
        for (int i = 0; i < ntok; i++) {
            if ((toks[i][0] == '|' || toks[i][0] == '>') && toks[i][1] == 0) {
                has_pipe_op = 1; break;
            }
        }

        /* Hard builtins — always inline (state mutation, special exit). */
        if (strcmp(toks[0], "help")     == 0) { cmd_help();  continue; }
        if (strcmp(toks[0], "pid")      == 0) { cmd_pid();   continue; }
        if (strcmp(toks[0], "time")     == 0) { cmd_time();  continue; }
        if (strcmp(toks[0], "forktest") == 0) { cmd_forktest(); continue; }
        if (strcmp(toks[0], "keys")     == 0) { cmd_keys();     continue; }
        if (strcmp(toks[0], "jobs")     == 0) { cmd_jobs();     continue; }
        if (strcmp(toks[0], "cd")       == 0) {
            cmd_cd(ntok > 1 ? toks[1] : "/");
            continue;
        }
        if (strcmp(toks[0], "sleep")    == 0) {
            cmd_sleep(ntok > 1 ? toks[1] : "");
            continue;
        }

        /* Soft builtins — only run inline when the line is a single
         * stage with no pipe/redirect. Pipelines fork+exec the .elf
         * binary of the same name. */
        if (!has_pipe_op) {
            if (strcmp(toks[0], "pwd") == 0) { cmd_pwd(); continue; }
            if (strcmp(toks[0], "ls")  == 0) {
                cmd_ls(ntok > 1 ? toks[1] : "");
                continue;
            }
        }
        if (strcmp(toks[0], "exit") == 0) {
            int code = 0;
            if (ntok > 1) {
                const char *p = toks[1];
                while (*p >= '0' && *p <= '9') { code = code*10 + (*p - '0'); p++; }
            }
            puts("bye\n");
            sys_exit(code);
        }

        struct pipeline pl;
        if (parse_pipeline(toks, ntok, &pl) < 0) {
            puts("sh: parse error\n");
            continue;
        }
        int rc = run_pipeline(&pl);
        if (rc != 0) {
            printf("[exit %d]\n", rc);
        }
    }
}
