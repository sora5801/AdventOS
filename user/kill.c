/*
 * kill — send a signal to a process by pid.
 *
 *   kill 7              -> SIGTERM (15) to pid 7
 *   kill -9 7           -> SIGKILL  (9) to pid 7
 *   kill -SIGUSR1 7     -> not supported (numeric only)
 *
 * Just a thin wrapper over SYS_KILL. The default signal is SIGTERM,
 * matching the bash builtin. Numeric signal names only — saving the
 * lookup table; see libuser.h for the SIG* constants we have.
 */

#include "libuser.h"

int main(int argc, char **argv) {
    int sig  = SIGTERM;
    int argi = 1;

    if (argi < argc && argv[argi][0] == '-') {
        sig = atoi(argv[argi] + 1);
        if (sig <= 0 || sig > 31) {
            sys_write(2, "kill: bad signal\n", 17);
            return 1;
        }
        argi++;
    }

    if (argi >= argc) {
        sys_write(2, "kill: usage: kill [-SIGNUM] PID...\n", 35);
        return 2;
    }

    int rc = 0;
    for (int i = argi; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (pid <= 0) {
            sys_write(2, "kill: bad pid: ", 15);
            sys_write(2, argv[i], (int)strlen(argv[i]));
            sys_write(2, "\n", 1);
            rc = 1;
            continue;
        }
        if (sys_kill(pid, sig) < 0) {
            printf("kill: pid %d: no such process\n", pid);
            rc = 1;
        }
    }
    return rc;
}
