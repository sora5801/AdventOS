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

static const char *g_prompt = "advent$ ";

/* ---- helpers ------------------------------------------------------- */

/* Tokenize on whitespace AND on `|`/`>` — the operator characters
 * become standalone tokens. Writes NULs over separators in `line` and
 * fills tokens[]. Returns token count. Stops at `cap-1` tokens.
 *
 * Example: "echo hi | cat > foo" → ["echo","hi","|","cat",">","foo"]. */
static int tokenize(char *line, char **tokens, int cap) {
    int n = 0;
    char *p = line;
    while (*p && n < cap - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '|' || *p == '>') {
            char saved = *p;
            *p++ = 0;        /* nothing to terminate, but be tidy */
            (void)saved;
            /* Write a one-char token in a separate static buffer per
             * operator, since the source byte itself is now NUL. */
            static char pipe_tok[2] = {'|', 0};
            static char gt_tok  [2] = {'>', 0};
            tokens[n++] = (saved == '|') ? pipe_tok : gt_tok;
            continue;
        }

        tokens[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '|' && *p != '>') p++;
        if (*p == ' ' || *p == '\t') { *p = 0; p++; }
        /* If we hit '|' or '>', leave them for the next iteration. */
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
    const char  *outfile;       /* > target, or NULL */
};

/* Walk tokens[] and split into stages by `|`. A trailing `>` <name>
 * binds to the last stage. Returns 0 on success, -1 on syntax error.
 *
 * tokens[] is mutated: the operator slots get NUL'd to terminate
 * each stage's argv slice, so argv[argc] is NULL as exec expects. */
static int parse_pipeline(char **tokens, int ntok, struct pipeline *pl) {
    pl->nstages = 0;
    pl->outfile = 0;

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

    for (int i = 0; i < n; i++) {
        int pid = sys_fork();
        if (pid < 0) {
            puts("sh: fork() failed mid-pipeline\n");
            /* Best effort: close everything, wait for the children
             * we already spawned. */
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
            /* Child stage i.
             *   - if i > 0   : dup the prev pipe's read end to fd 0.
             *   - if i < n-1 : dup this pipe's write end to fd 1.
             *   - if i==n-1 and outfile: dup outfd to fd 1.
             *   - then close ALL pipe fds + outfd we don't need.
             *   - exec.
             */
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
            /* exec only returns on failure. */
            sys_write(2, "sh: exec failed: ", 17);
            sys_write(2, path, (int)strlen(path));
            sys_write(2, "\n", 1);
            sys_exit(127);
        }
        pids[i] = pid;
    }

    /* Parent: drop all pipe references so writers can see EOF when
     * all children writing to a given pipe exit. Critical — if we
     * held the read end of pipe[0] open, the cat in stage 1 reading
     * from it would never see EOF after echo in stage 0 exits. */
    for (int i = 0; i < n - 1; i++) {
        sys_close(pipes[i][0]);
        sys_close(pipes[i][1]);
    }
    if (outfd >= 0) sys_close(outfd);

    /* Wait for every child. Track the last-stage exit code as the
     * pipeline result. */
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
    puts("  exit [CODE]       exit the shell\n");
    puts("\n");
    puts("Pipelines and redirection:\n");
    puts("  cmd1 | cmd2 | ... [> outfile]\n");
    puts("  Each stage is a separate fork()+exec(); | wires stdin/stdout\n");
    puts("  via SYS_PIPE+SYS_DUP2; > opens an in-RAM tmpfs file you can\n");
    puts("  later cat back. Programs in /:  hello count cat echo httpd\n");
}

static void cmd_pid(void)  { printf("shell pid: %d\n", sys_getpid()); }
static void cmd_time(void) { printf("epoch: %u\n", sys_time()); }

static void cmd_sleep(const char *arg) {
    uint32_t ms = 0;
    while (*arg >= '0' && *arg <= '9') { ms = ms * 10 + (*arg - '0'); arg++; }
    if (ms == 0) { puts("sleep: usage: sleep <ms>\n"); return; }
    sys_sleep_ms(ms);
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

    puts("\nAdventOS userspace shell, pid="); printf("%d\n", sys_getpid());
    puts("Type 'help' for builtins. | and > are honored.\n\n");

    if (run_selftest) selftest();

    char  line[LINE_MAX];
    char *toks[ARG_MAX];

    for (;;) {
        puts(g_prompt);
        int n = sys_read_line(line, sizeof(line));
        if (n <= 0) continue;

        int ntok = tokenize(line, toks, ARG_MAX);
        if (ntok == 0) continue;

        /* Builtins (only valid as the SOLE token sequence — pipelines
         * containing builtins are not supported, since builtins run
         * inline in the shell process). */
        if (strcmp(toks[0], "help")     == 0) { cmd_help();  continue; }
        if (strcmp(toks[0], "pid")      == 0) { cmd_pid();   continue; }
        if (strcmp(toks[0], "time")     == 0) { cmd_time();  continue; }
        if (strcmp(toks[0], "forktest") == 0) { cmd_forktest(); continue; }
        if (strcmp(toks[0], "sleep")    == 0) {
            cmd_sleep(ntok > 1 ? toks[1] : "");
            continue;
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
