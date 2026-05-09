/*
 * AdventOS init — the userspace PID-1 equivalent.
 *
 * Reads /inittab at startup, fork()+exec()s every service line, then
 * loops in a wait()-and-respawn loop reaping zombies and (optionally)
 * restarting any service marked `respawn`. Orphans whose parent died
 * with us still alive get reparented to us by the kernel — the
 * adoption + zombie reap of those falls out of the same loop.
 *
 * inittab format (one entry per non-comment line):
 *
 *     <mode> <program> [args...]
 *
 * where <mode> is:
 *
 *     once     run once at boot; don't restart on exit
 *     respawn  fork+exec a fresh copy whenever this one exits
 *
 * Lines starting with '#' or empty lines are ignored.
 *
 * The kernel knows our pid via task_set_init_pid (called from kmain
 * right after LAUNCH("init.elf", ...)) and uses it as the reparent
 * target in task_exit_current.
 */

#include "libuser.h"

#define MAX_SERVICES   16
#define MAX_LINE_LEN   128
#define MAX_ARGS       8

struct service {
    int   in_use;
    int   respawn;
    int   pid;                       /* current child pid, -1 if dead/done */
    char  raw[MAX_LINE_LEN];         /* tokenized in place */
    char *argv[MAX_ARGS + 1];        /* NULL-terminated */
    int   argc;
};

static struct service g_services[MAX_SERVICES];

/* ---- helpers ------------------------------------------------------- */

/* Slurp file `name` into buf (capacity cap). Returns bytes read or -1. */
static int read_whole_file(const char *name, char *buf, int cap) {
    int fd = sys_open(name);
    if (fd < 0) return -1;
    int total = 0;
    while (total < cap - 1) {
        int n = sys_read(fd, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    sys_close(fd);
    buf[total] = 0;
    return total;
}

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

/* ---- parser -------------------------------------------------------- */

/* Tokenize `line` in place on whitespace. Fills `out` with up to
 * MAX_ARGS pointers + NULL terminator. Returns argc. */
static int tokenize(char *line, char **out) {
    int n = 0;
    char *p = line;
    while (*p && n < MAX_ARGS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        out[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = 0; p++; }
    }
    out[n] = 0;
    return n;
}

/* Parse inittab text. Each line becomes one service entry. */
static int parse_inittab(char *text) {
    int idx = 0;
    char *p = text;
    while (*p) {
        /* Find end-of-line. */
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') { *p = 0; p++; }

        /* Skip leading whitespace. */
        while (*line == ' ' || *line == '\t') line++;
        if (*line == 0 || *line == '#') continue;       /* blank / comment */

        if (idx >= MAX_SERVICES) {
            puts("init: too many services in inittab\n");
            break;
        }

        /* Copy into the service's own buffer so tokenize's NUL writes
         * don't trample the next line. (We already split text on \n.) */
        struct service *s = &g_services[idx];
        int len = my_strlen(line);
        if (len >= MAX_LINE_LEN) len = MAX_LINE_LEN - 1;
        for (int i = 0; i < len; i++) s->raw[i] = line[i];
        s->raw[len] = 0;

        char *toks[MAX_ARGS + 1];
        int n = tokenize(s->raw, toks);
        if (n < 2) continue;          /* need at least mode + prog */

        if (strcmp(toks[0], "once") == 0)         s->respawn = 0;
        else if (strcmp(toks[0], "respawn") == 0) s->respawn = 1;
        else {
            printf("init: unknown mode '%s' (expected once/respawn)\n", toks[0]);
            continue;
        }

        /* argv = tokens 1..n-1, NULL-terminated. */
        for (int i = 1; i < n; i++) s->argv[i - 1] = toks[i];
        s->argv[n - 1] = 0;
        s->argc = n - 1;
        s->in_use = 1;
        s->pid    = -1;
        idx++;
    }
    return idx;
}

/* ---- spawn / lookup ------------------------------------------------ */

static int spawn_service(struct service *s) {
    int pid = sys_fork();
    if (pid < 0) {
        puts("init: fork failed\n");
        return -1;
    }
    if (pid == 0) {
        sys_exec(s->argv[0], (const char *const *)s->argv);
        /* If exec returns, it failed. Tell init via stderr+exit. */
        puts("init: exec failed: ");
        puts(s->argv[0]);
        puts("\n");
        sys_exit(127);
    }
    s->pid = pid;
    return pid;
}

static int find_service_by_pid(int pid) {
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (g_services[i].in_use && g_services[i].pid == pid) return i;
    }
    return -1;
}

/* ---- main ---------------------------------------------------------- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("init: pid=%d, reading /etc/inittab\n", sys_getpid());

    /* Slurp inittab. */
    static char tab[2048];
    int n = read_whole_file("/etc/inittab", tab, sizeof(tab));
    if (n <= 0) {
        puts("init: cannot open /etc/inittab; nothing to do\n");
        /* Don't exit — that would cascade kpanic-style "init died".
         * Sit in a wait loop so any orphans can still be reaped. */
    }

    int nsvc = (n > 0) ? parse_inittab(tab) : 0;
    printf("init: %d service(s) in inittab\n", nsvc);

    /* Spawn everything once. */
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (!g_services[i].in_use) continue;
        spawn_service(&g_services[i]);
        printf("init: started '%s' as pid %d (%s)\n",
               g_services[i].argv[0],
               g_services[i].pid,
               g_services[i].respawn ? "respawn" : "once");
    }

    puts("init: entering reap+respawn loop\n");

    /* The classic init main loop: wait for ANY child, react. */
    for (;;) {
        int code = 0;
        int pid = sys_wait(&code);
        if (pid < 0) {
            /* No children at all — nothing to do; loop and try again
             * in case someone hands us an orphan via reparenting. */
            sys_sleep_ms(200);
            continue;
        }

        int idx = find_service_by_pid(pid);
        if (idx < 0) {
            /* Adopted orphan — reaped on behalf of a dead grandparent.
             * Print so the demo can verify it. */
            printf("init: reaped orphan pid=%d code=%d\n", pid, code);
            continue;
        }

        struct service *s = &g_services[idx];
        printf("init: '%s' (pid=%d) exited code=%d\n",
               s->argv[0], pid, code);
        if (s->respawn) {
            int new_pid = spawn_service(s);
            if (new_pid > 0) {
                printf("init: respawned '%s' as pid=%d\n",
                       s->argv[0], new_pid);
            }
        } else {
            s->pid = -1;       /* dead, don't restart */
        }
    }
}
