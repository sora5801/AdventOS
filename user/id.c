/*
 * id — print the calling task's process credentials.
 *
 *   $ id
 *   uid=1000 gid=1000 pid=42 pgid=42 sid=42
 *
 * Mirrors the Unix `id` command at a minimum. Pid / pgid / sid are
 * tossed in because they cost one syscall each and the test harness
 * uses them.
 */
#include "libuser.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int uid = sys_getuid();
    int gid = sys_getgid();
    int pid = sys_getpid();
    /* session 20 added the pgid wrapper as plain `getpgid` (no sys_
     * prefix) because it's the POSIX name; here we just call it. */
    int pgid = getpgid(0);
    printf("uid=%d gid=%d pid=%d pgid=%d\n", uid, gid, pid, pgid);
    return 0;
}
