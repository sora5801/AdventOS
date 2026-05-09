/*
 * AdventOS userspace shell — pid 5 at boot, plus a `forktest` builtin
 * that demonstrates fork/exec/wait independently of running an external
 * program.
 *
 * Read a line, tokenize it, then:
 *   - if it matches a builtin, run it inline,
 *   - otherwise fork(); the child execs argv[0]+".elf" via SYS_EXEC,
 *     and the parent waits for it.
 *
 * The previous SYS_KCMD path is gone — every external command is now
 * a real ring-3 process spawned with fork() + exec(). The kernel
 * commands (ifconfig, tasks, meminfo, ...) live in kernel/shell.c
 * still, but they're no longer reachable from this shell. They could
 * be ported to user-mode programs in a future session.
 */

#include "libuser.h"

#define LINE_MAX  256
#define ARG_MAX   16

static const char *g_prompt = "advent$ ";

/* ---- helpers ------------------------------------------------------- */

/* In-place tokenize on spaces. Writes NULs over separators and fills
 * argv[]. Returns argc. Stops at ARG_MAX-1 args (leaves room for the
 * NULL terminator argv expects). */
static int tokenize(char *line, char **argv) {
    int argc = 0;
    char *p = line;
    while (*p && argc < ARG_MAX - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = 0; p++; }
    }
    argv[argc] = 0;
    return argc;
}

/* Append ".elf" to a name if it doesn't already have it. Static buffer
 * — fine, the shell is single-threaded against itself. */
static const char *resolve_program(const char *name) {
    static char buf[64];
    int i = 0;
    while (name[i] && i < 60) { buf[i] = name[i]; i++; }
    /* Already has .elf? */
    if (i >= 4 && buf[i-4] == '.' && buf[i-3] == 'e' &&
        buf[i-2] == 'l' && buf[i-1] == 'f') {
        buf[i] = 0;
        return buf;
    }
    /* Append. */
    if (i + 4 >= (int)sizeof(buf)) return name;   /* too long, give up */
    buf[i++] = '.'; buf[i++] = 'e'; buf[i++] = 'l'; buf[i++] = 'f';
    buf[i] = 0;
    return buf;
}

/* ---- builtins ------------------------------------------------------ */

static void cmd_help(void) {
    puts("Userspace shell builtins (running as a real ring-3 process):\n");
    puts("  help              this list\n");
    puts("  pid               print our pid via SYS_GETPID\n");
    puts("  time              print epoch via SYS_TIME\n");
    puts("  sleep MS          pause MS milliseconds (yields to other tasks)\n");
    puts("  forktest          fork() and have child + parent print their pids\n");
    puts("  exit [CODE]       exit the shell\n");
    puts("\n");
    puts("Anything else is launched as a separate process via fork()+exec().\n");
    puts("The kernel will look up <NAME>.elf in AdventFS, build a fresh user PD\n");
    puts("for it, and run it concurrently with the shell. The shell then waits\n");
    puts("for the child to exit and prints its exit code.\n");
    puts("\n");
    puts("Programs available in /:  hello  count  cat  echo  httpd\n");
}

static void cmd_pid(void) {
    printf("shell pid: %d\n", sys_getpid());
}

static void cmd_time(void) {
    printf("epoch: %u\n", sys_time());
}

static void cmd_sleep(const char *arg) {
    uint32_t ms = 0;
    while (*arg >= '0' && *arg <= '9') { ms = ms * 10 + (*arg - '0'); arg++; }
    if (ms == 0) { puts("sleep: usage: sleep <ms>\n"); return; }
    sys_sleep_ms(ms);
}

/* fork() demo — proves the syscall returns twice with two different
 * EAX values, and that the child gets its own user PD (we modify a
 * stack variable in the child and parent never sees the change). */
static void cmd_forktest(void) {
    int marker = 0xCAFE;
    int pid = sys_fork();
    if (pid < 0) {
        puts("forktest: fork() failed\n");
        return;
    }
    if (pid == 0) {
        /* Child */
        marker = 0xBABE;
        printf("  child : pid=%d  marker=0x%x  (was 0xCAFE in parent)\n",
               sys_getpid(), marker);
        sys_exit(42);
    }
    /* Parent */
    int code = -1;
    int reaped = sys_wait(&code);
    printf("  parent: pid=%d  marker=0x%x  child_pid=%d  reaped=%d  exit=%d\n",
           sys_getpid(), marker, pid, reaped, code);
}

/* ---- main loop ----------------------------------------------------- */

/* Auto-run a fork/exec/wait demo at startup. Triggered by passing
 * "selftest" as argv[1] — kmain does this so that the milestone is
 * verifiable on a headless boot without needing keyboard input. */
static void selftest(void) {
    puts("\n=== sh selftest: fork / exec / wait ===\n");

    /* Test 1: fork() returns twice with two different EAX values. */
    puts("[t1] forktest:\n");
    cmd_forktest();

    /* Test 2: fork + exec hello.elf, parent waits. */
    puts("[t2] fork + exec hello.elf:\n");
    int pid = sys_fork();
    if (pid == 0) {
        const char *argv2[] = { "hello.elf", 0 };
        sys_exec("hello.elf", argv2);
        sys_exit(127);
    }
    int code = -1;
    int r = sys_wait(&code);
    printf("  parent waited: pid=%d  exit=%d\n", r, code);

    /* Test 3: nested fork — parent forks, child forks again, all three
     * print pids and exit. Demonstrates that a forked child can fork. */
    puts("[t3] nested fork:\n");
    int pid2 = sys_fork();
    if (pid2 == 0) {
        printf("  child1 pid=%d, about to fork()\n", sys_getpid());
        int pid3 = sys_fork();
        if (pid3 == 0) {
            printf("  child2 pid=%d (parent=%d)\n",
                   sys_getpid(), pid2);
            sys_exit(7);
        }
        int c2 = 0;
        sys_wait(&c2);
        printf("  child1 reaped grandchild, exit=%d\n", c2);
        sys_exit(11);
    }
    int c1 = 0;
    sys_wait(&c1);
    printf("  parent reaped child1, exit=%d\n", c1);

    puts("=== selftest done ===\n\n");
}

int main(int argc, char **argv) {
    /* If invoked with "selftest" as a positional arg, run the
     * fork/exec/wait demo before entering the prompt loop. */
    int run_selftest = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "selftest") == 0) { run_selftest = 1; break; }
    }

    puts("\nAdventOS userspace shell, pid="); printf("%d\n", sys_getpid());
    puts("Type 'help' for builtins. External programs run via fork()+exec().\n\n");

    if (run_selftest) selftest();

    char  line[LINE_MAX];
    char *targv[ARG_MAX];

    for (;;) {
        puts(g_prompt);
        int n = sys_read_line(line, sizeof(line));
        if (n <= 0) continue;

        /* Tokenize a working copy so the rest of the shell sees argv[]. */
        int targc = tokenize(line, targv);
        if (targc == 0) continue;

        /* Builtins. */
        if (strcmp(targv[0], "help")     == 0) { cmd_help(); continue; }
        if (strcmp(targv[0], "pid")      == 0) { cmd_pid();  continue; }
        if (strcmp(targv[0], "time")     == 0) { cmd_time(); continue; }
        if (strcmp(targv[0], "forktest") == 0) { cmd_forktest(); continue; }
        if (strcmp(targv[0], "sleep")    == 0) {
            cmd_sleep(targc > 1 ? targv[1] : "");
            continue;
        }
        if (strcmp(targv[0], "exit") == 0) {
            int code = 0;
            if (targc > 1) {
                const char *p = targv[1];
                while (*p >= '0' && *p <= '9') { code = code*10 + (*p - '0'); p++; }
            }
            puts("bye\n");
            sys_exit(code);
        }

        /* External: fork() then exec() in the child, wait() in parent. */
        const char *path = resolve_program(targv[0]);
        int pid = sys_fork();
        if (pid < 0) {
            puts("sh: fork() failed\n");
            continue;
        }
        if (pid == 0) {
            /* Child: argv[] still points into the (cloned) parent
             * stack page, which is private to us now. Pass it through
             * to exec — the kernel snapshots strings before tearing
             * down our address space. */
            sys_exec(path, (const char *const *)targv);
            /* If exec returns at all, it failed. */
            puts("sh: exec failed: "); puts(path); puts("\n");
            sys_exit(127);
        }

        /* Parent: wait for the child we just forked. Our shell only
         * has one outstanding child at a time so this works without
         * matching pid against the returned wait value. */
        int code = 0;
        int reaped = sys_wait(&code);
        if (reaped > 0 && code != 0) {
            printf("[pid %d exited with code %d]\n", reaped, code);
        }
    }
}
