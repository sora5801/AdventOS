#include "libuser.h"

/*
 * mingw32 GCC auto-emits `call ___main` at the top of any function
 * literally named `main`. It's a libgcc hook for C++ static
 * constructor invocation, etc. We're freestanding with no such
 * machinery — provide an empty stub. (The C source name `__main`
 * gets one mingw underscore added at compile time, producing the
 * `___main` symbol the linker is looking for.)
 */
void __main(void) {}

/* ---------- Syscall wrappers --------------------------------------- */

int sys_putchar(char c) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WRITE), "b"((int)(unsigned char)c)
                      : "memory");
    return ret;
}

int sys_getpid(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_GETPID)
                      : "memory");
    return ret;
}

void sys_exit(int code) {
    __asm__ volatile ("int $0x80"
                      :
                      : "a"(SYS_EXIT), "b"(code)
                      : "memory");
    /* unreachable — but a return path satisfies the compiler */
    for (;;) __asm__ volatile ("hlt");
}

void sys_yield(void) {
    __asm__ volatile ("int $0x80"
                      :
                      : "a"(SYS_YIELD)
                      : "memory");
}

int sys_write_str(const char *s) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WRITE_STR), "b"(s)
                      : "memory");
    return ret;
}

void sys_sleep_ms(uint32_t ms) {
    __asm__ volatile ("int $0x80"
                      :
                      : "a"(SYS_SLEEP_MS), "b"(ms)
                      : "memory");
}

uint32_t sys_time(void) {
    uint32_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TIME)
                      : "memory");
    return ret;
}

int sys_read_line(char *buf, int cap) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_READ_LINE), "b"(buf), "c"(cap)
                      : "memory");
    return ret;
}

int sys_open(const char *name) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_OPEN), "b"(name)
                      : "memory");
    return ret;
}

int sys_read(int fd, void *buf, int n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_READ), "b"(fd), "c"(buf), "d"(n)
                      : "memory");
    return ret;
}

int sys_write(int fd, const void *buf, int n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WRITE_FD), "b"(fd), "c"(buf), "d"(n)
                      : "memory");
    return ret;
}

int sys_close(int fd) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_CLOSE), "b"(fd)
                      : "memory");
    return ret;
}

int sys_socket(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SOCKET)
                      : "memory");
    return ret;
}

int sys_bind(int fd, int port) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_BIND), "b"(fd), "c"(port)
                      : "memory");
    return ret;
}

int sys_listen(int fd, int backlog) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_LISTEN), "b"(fd), "c"(backlog)
                      : "memory");
    return ret;
}

int sys_accept(int fd) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_ACCEPT), "b"(fd)
                      : "memory");
    return ret;
}

int sys_connect(int fd, const unsigned char ip[4], int port) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_CONNECT), "b"(fd), "c"(ip), "d"(port)
                      : "memory");
    return ret;
}

int sys_fork(void) {
    int ret;
    /* The kernel synthesizes the child's stack so its first user
     * instruction is the one immediately after `int $0x80`, with
     * EAX=0. So control returns from this asm twice: in the parent
     * with EAX=child_pid, in the child with EAX=0. */
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FORK)
                      : "memory");
    return ret;
}

int sys_exec(const char *path, const char *const *argv) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_EXEC), "b"(path), "c"(argv)
                      : "memory");
    return ret;
}

int sys_wait(int *exit_code) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WAIT), "b"(exit_code)
                      : "memory");
    return ret;
}

int sys_wait_nb(int *exit_code) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WAIT_NB), "b"(exit_code)
                      : "memory");
    return ret;
}

int sys_getcpu(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_GETCPU)
                      : "memory");
    return ret;
}

int sys_fbinfo(unsigned int out[4]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FBINFO), "b"(out)
                      : "memory");
    return ret;
}

int sys_smp_stats(unsigned int out[8]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SMP_STATS), "b"(out)
                      : "memory");
    return ret;
}

/* sys_mouse_state and sys_fb_mmap were removed when AdventOS narrowed
 * to a CLI-only OS. The kernel slots still return 0 if some stale binary
 * issues the syscall directly. */

int sys_audio_play(const void *pcm, int n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_AUDIO_PLAY), "b"(pcm), "c"(n)
                      : "memory");
    return ret;
}

int sys_block_info(int dev_idx, struct sys_block_info *out) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_BLOCK_INFO), "b"(dev_idx), "c"(out)
                      : "memory");
    return ret;
}
int sys_block_read(int dev_idx, unsigned int lba, unsigned int n, void *buf) {
    struct sys_block_args a = { (unsigned)dev_idx, lba, n, buf };
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_BLOCK_READ), "b"(&a)
                      : "memory");
    return ret;
}
int sys_block_write(int dev_idx, unsigned int lba, unsigned int n, const void *buf) {
    struct sys_block_args a = { (unsigned)dev_idx, lba, n, (void *)buf };
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_BLOCK_WRITE), "b"(&a)
                      : "memory");
    return ret;
}

int sys_tty_cursor(int row, int col) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TTY_CURSOR), "b"(row), "c"(col)
                      : "memory");
    return ret;
}
int sys_tty_get_cursor(int *out_row, int *out_col) {
    /* The kernel writes (row, col) into a 2-int buffer; we copy the
     * pieces out so the caller can pass nulls for fields they don't
     * care about. */
    int out[2] = { 0, 0 };
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TTY_GET_CURSOR), "b"(out)
                      : "memory");
    if (out_row) *out_row = out[0];
    if (out_col) *out_col = out[1];
    return ret;
}
int sys_tty_clear(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_TTY_CLEAR) : "memory");
    return ret;
}
int sys_tty_clear_eol(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_TTY_CLEAR_EOL) : "memory");
    return ret;
}

int sys_getuid(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_GETUID) : "memory");
    return ret;
}
int sys_getgid(void) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_GETGID) : "memory");
    return ret;
}
int sys_setuid(int uid) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_SETUID), "b"(uid) : "memory");
    return ret;
}
int sys_setgid(int gid) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_SETGID), "b"(gid) : "memory");
    return ret;
}
int sys_fs_owner(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_FS_OWNER), "b"(path) : "memory");
    return ret;
}
int sys_fs_mode(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_FS_MODE), "b"(path) : "memory");
    return ret;
}
/* Session 81: structured-pipeline ls reads this to populate `size`. */
int sys_fs_size(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_FS_SIZE), "b"(path) : "memory");
    return ret;
}
int sys_chmod(const char *path, int mode) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_CHMOD), "b"(path), "c"(mode)
                      : "memory");
    return ret;
}
int sys_chown(const char *path, int uid, int gid) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_CHOWN), "b"(path), "c"(uid), "d"(gid)
                      : "memory");
    return ret;
}

int sys_pipe(int fds[2]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_PIPE), "b"(fds)
                      : "memory");
    return ret;
}

int sys_openpty(int fds[2]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_OPENPTY), "b"(fds)
                      : "memory");
    return ret;
}

/* ---- Session 57: debugger syscalls ----
 *
 * The fb_takeover and mouse_inject wrappers were removed with the WM
 * and mouse driver. */

int sys_kbd_poll(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_KBD_POLL)
                      : "memory");
    return ret;
}

int sys_ptrace(int op, int pid, void *args) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_PTRACE), "b"(op), "c"(pid), "d"(args)
                      : "memory");
    return ret;
}

/* Session 107 — Path C framebuffer syscalls. */
int sys_fb_info(struct sys_fb_info *out) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FB_INFO), "b"(out)
                      : "memory");
    return ret;
}
int sys_fb_map(unsigned int user_va) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FB_MAP), "b"(user_va)
                      : "memory");
    return ret;
}
int sys_fb_unmap(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FB_UNMAP)
                      : "memory");
    return ret;
}

/* Session 109 — Path C mouse poll. */
int sys_mouse_poll(struct sys_mouse_state *out) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_MOUSE_POLL), "b"(out)
                      : "memory");
    return ret;
}

/* Session 112 — Path C WM client protocol. */
int sys_wm_bind(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WM_BIND)
                      : "memory");
    return ret;
}
int sys_wm_create(struct sys_wm_create *args) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WM_CREATE), "b"(args)
                      : "memory");
    return ret;
}
int sys_wm_poll(struct sys_wm_msg *out) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WM_POLL), "b"(out)
                      : "memory");
    return ret;
}
int sys_wm_destroy(unsigned int window_id) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_WM_DESTROY), "b"(window_id)
                      : "memory");
    return ret;
}

int sys_ntp_sync(const unsigned char ip[4]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_NTP_SYNC), "b"(ip)
                      : "memory");
    return ret;
}

int sys_ntp_test_responder(int on, unsigned int epoch) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_NTP_TEST_RESPONDER), "b"(on), "c"(epoch)
                      : "memory");
    return ret;
}

int sys_dns_cache_stats(unsigned int out[4]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_DNS_CACHE_STATS), "b"(out)
                      : "memory");
    return ret;
}

int sys_dhcp_info(struct sys_dhcp_info *out) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_DHCP_INFO), "b"(out)
                      : "memory");
    return ret;
}

int sys_fd_nb(int fd, int on) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FD_NB), "b"(fd), "c"(on)
                      : "memory");
    return ret;
}

int sys_serial_inject(const char *bytes, int n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SERIAL_INJECT), "b"(bytes), "c"(n)
                      : "memory");
    return ret;
}

/* Session 70: syscall sandbox.
 *
 * The kernel keeps a 4-word allow-bitmap per task. `sys_sandbox_install`
 * either installs it (first call) or AND-s it with the current mask
 * (subsequent calls). Policies are sticky and monotonic — you can
 * only tighten, never loosen. The mask is inherited across fork+exec.
 *
 * The policy builders below stage a 4-word mask in caller-supplied
 * storage, then it's up to the caller to actually install with
 * sys_sandbox_install. That gives the caller a chance to add or
 * remove syscalls from the predefined templates before committing.
 *
 * Every policy keeps SYS_SANDBOX_INSTALL allowed so the task can
 * ratchet down further (e.g. relinquish writes once setup is done).
 * Clear the bit yourself if you want to freeze the policy. */

int sys_sandbox_install(const uint32_t mask[4]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SANDBOX_INSTALL), "b"(mask)
                      : "memory");
    return ret;
}

static void sb_allow(uint32_t mask[4], int sc) {
    if (sc < 0 || sc >= 128) return;
    mask[sc / 32] |= 1u << (sc % 32);
}

void sandbox_policy_minimal(uint32_t mask[4]) {
    for (int i = 0; i < 4; i++) mask[i] = 0;
    /* Compute + ctrl: just enough for a CPU-bound tool that prints and
     * exits cleanly. */
    sb_allow(mask, SYS_WRITE);
    sb_allow(mask, SYS_WRITE_STR);
    sb_allow(mask, SYS_GETPID);
    sb_allow(mask, SYS_EXIT);
    sb_allow(mask, SYS_YIELD);
    sb_allow(mask, SYS_SLEEP_MS);
    sb_allow(mask, SYS_TIME);
    sb_allow(mask, SYS_BRK);
    sb_allow(mask, SYS_GETUID);
    sb_allow(mask, SYS_GETGID);
    sb_allow(mask, SYS_GETCPU);
    /* Process plumbing — needed by the `sandbox` wrapper itself
     * (install policy, then exec target) and by any tool that wants
     * to fork/wait on a child within the same sandbox. The child
     * inherits the mask verbatim. */
    sb_allow(mask, SYS_FORK);
    sb_allow(mask, SYS_EXEC);
    sb_allow(mask, SYS_WAIT);
    sb_allow(mask, SYS_WAIT_NB);
    /* Sandbox itself: callable so the policy can be tightened further
     * after early-startup steps complete. Clear this bit yourself to
     * freeze the policy permanently. */
    sb_allow(mask, SYS_SANDBOX_INSTALL);
}

void sandbox_policy_compute(uint32_t mask[4]) {
    sandbox_policy_minimal(mask);
    sb_allow(mask, SYS_MMAP);
    sb_allow(mask, SYS_MUNMAP);
}

void sandbox_policy_readfs(uint32_t mask[4]) {
    sandbox_policy_compute(mask);
    sb_allow(mask, SYS_OPEN);
    sb_allow(mask, SYS_READ);
    sb_allow(mask, SYS_CLOSE);
    sb_allow(mask, SYS_READDIR);
    sb_allow(mask, SYS_GETCWD);
    sb_allow(mask, SYS_CHDIR);
    sb_allow(mask, SYS_FS_OWNER);
    sb_allow(mask, SYS_FS_MODE);
}

void sandbox_policy_netclient(uint32_t mask[4]) {
    sandbox_policy_readfs(mask);
    sb_allow(mask, SYS_SOCKET);
    sb_allow(mask, SYS_CONNECT);
    sb_allow(mask, SYS_WRITE_FD);
    sb_allow(mask, SYS_DNS_RESOLVE);
    sb_allow(mask, SYS_FD_NB);
}

/* Session 71: resource-limit wrapper + helper. */
int sys_setlimit(const struct sys_limits *l) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SETLIMIT), "b"(l)
                      : "memory");
    return ret;
}

void limits_default(struct sys_limits *l) {
    l->max_rss_kb  = 0;
    l->max_cpu_ms  = 0;
    l->max_fds     = 0;
    l->max_wall_ms = 0;
}

/* ============================================================
 * Session 73: filesystem unlink + KV-store helpers
 * ============================================================ */

int sys_unlink(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_UNLINK), "b"(path)
                      : "memory");
    return ret;
}

int sys_rmdir(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_RMDIR), "b"(path)
                      : "memory");
    return ret;
}

/* Path-component validators. Both deliberately strict; we'd rather
 * reject a borderline-valid name than ship a path-traversal hole.
 * Returns 1 if valid, 0 if not.  No errno, no diagnostics — the
 * caller surfaces "Invalid params" upstream. */
static int kv_validate_ns(const char *ns) {
    if (!ns || !*ns) return 0;
    int n = 0;
    while (ns[n]) {
        char c = ns[n];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return 0;
        if (++n > 32) return 0;
    }
    return 1;
}
static int kv_validate_key(const char *key) {
    if (!key || !*key) return 0;
    if (key[0] == '.') return 0;            /* no leading dot */
    int n = 0;
    while (key[n]) {
        char c = key[n];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                 c == '.';
        if (!ok) return 0;                  /* rejects '/' explicitly */
        if (++n > 64) return 0;
    }
    return 1;
}

/* Build "/var/kv/<ns>/<key>" into out[].  Out buffer must be ≥ 128
 * chars (32 ns + 64 key + "/var/kv//" + NUL).  Returns -1 on any
 * validation failure; caller should treat that as a bad-request. */
static int kv_path(char *out, int cap, const char *ns, const char *key) {
    if (!kv_validate_ns(ns)) return -1;
    if (!kv_validate_key(key)) return -1;
    const char *prefix = "/var/kv/";
    int o = 0;
    for (int i = 0; prefix[i]; i++) {
        if (o >= cap - 1) return -1;
        out[o++] = prefix[i];
    }
    for (int i = 0; ns[i]; i++) {
        if (o >= cap - 1) return -1;
        out[o++] = ns[i];
    }
    if (o >= cap - 1) return -1;
    out[o++] = '/';
    for (int i = 0; key[i]; i++) {
        if (o >= cap - 1) return -1;
        out[o++] = key[i];
    }
    out[o] = 0;
    return 0;
}

/* "/var/kv/<ns>" prefix (no trailing key — used by kv_list). */
static int kv_ns_path(char *out, int cap, const char *ns) {
    if (!kv_validate_ns(ns)) return -1;
    const char *prefix = "/var/kv/";
    int o = 0;
    for (int i = 0; prefix[i]; i++) {
        if (o >= cap - 1) return -1;
        out[o++] = prefix[i];
    }
    for (int i = 0; ns[i]; i++) {
        if (o >= cap - 1) return -1;
        out[o++] = ns[i];
    }
    out[o] = 0;
    return 0;
}

int kv_get(const char *ns, const char *key, void *buf, int cap) {
    char path[128];
    if (kv_path(path, sizeof(path), ns, key) < 0) return -1;
    int fd = sys_open(path);
    if (fd < 0) return -1;
    int n = sys_read(fd, buf, cap);
    sys_close(fd);
    return n;
}

int kv_put(const char *ns, const char *key, const void *buf, int len) {
    char path[128];
    if (kv_path(path, sizeof(path), ns, key) < 0) return -1;
    if (len < 0 || len > 65536) return -1;

    /* Session 73: auto-create the namespace directory on first put.
     * /var/kv exists by boot-time bootstrap, but per-namespace dirs
     * are user-created. sys_mkdir is idempotent enough — it returns
     * -1 when the dir already exists, which we just ignore. */
    char nsdir[128];
    if (kv_ns_path(nsdir, sizeof(nsdir), ns) == 0) {
        (void)sys_mkdir(nsdir);
    }

    return sys_fs_write(path, buf, len);
}

int kv_del(const char *ns, const char *key) {
    char path[128];
    if (kv_path(path, sizeof(path), ns, key) < 0) return -1;
    return sys_unlink(path);
}

int kv_list(const char *ns, const char *prefix, int *iter, char *out_key) {
    char dir[128];
    if (kv_ns_path(dir, sizeof(dir), ns) < 0) return -1;

    /* Walk via sys_readdir.  *iter is mutated by the syscall — we
     * just propagate it. Filters out entries not matching `prefix`
     * (NULL prefix or "" = match all). Returns the entry index that
     * matched, or -1 if no more matches. */
    char name[16];
    for (;;) {
        int idx = sys_readdir(dir, iter, name);
        if (idx < 0) return -1;
        if (prefix && *prefix) {
            int ok = 1;
            for (int i = 0; prefix[i]; i++) {
                if (name[i] != prefix[i]) { ok = 0; break; }
            }
            if (!ok) continue;
        }
        /* Copy out — caller buffer must be ≥ 16 bytes. */
        for (int i = 0; i < 16; i++) {
            out_key[i] = name[i];
            if (name[i] == 0) break;
        }
        return idx;
    }
}

int kv_stat(const char *ns, const char *key, int *out_size) {
    char path[128];
    if (kv_path(path, sizeof(path), ns, key) < 0) return -1;
    int fd = sys_open(path);
    if (fd < 0) {
        if (out_size) *out_size = 0;
        return -1;
    }
    /* Read until EOF and count.  Cheaper would be a SYS_STAT call;
     * deferring that to a later session — for now this works and KV
     * values are small. */
    int total = 0;
    char tmp[256];
    int  n;
    while ((n = sys_read(fd, tmp, sizeof(tmp))) > 0) total += n;
    sys_close(fd);
    if (out_size) *out_size = total;
    return 0;
}

int sys_dup2(int oldfd, int newfd) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_DUP2), "b"(oldfd), "c"(newfd)
                      : "memory");
    return ret;
}

int sys_open_w(const char *name) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_OPEN_W), "b"(name)
                      : "memory");
    return ret;
}

int sys_kill(int pid, int sig) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_KILL), "b"(pid), "c"(sig)
                      : "memory");
    return ret;
}

uint32_t tty_set_mode(uint32_t flags) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TTY_SET_MODE), "b"(flags)
                      : "memory");
    return (uint32_t)ret;
}

uint32_t tty_get_mode(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TTY_GET_MODE)
                      : "memory");
    return (uint32_t)ret;
}

int tty_inject(const char *bytes, int n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TTY_INJECT), "b"(bytes), "c"(n)
                      : "memory");
    return ret;
}

int sys_fs_write(const char *name, const void *data, uint32_t n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FS_WRITE), "b"(name), "c"(data), "d"(n)
                      : "memory");
    return ret;
}

int setpgid(int pid, int pgid) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SETPGID), "b"(pid), "c"(pgid)
                      : "memory");
    return ret;
}

int getpgid(int pid) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_GETPGID), "b"(pid)
                      : "memory");
    return ret;
}

int setsid(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SETSID)
                      : "memory");
    return ret;
}

int getsid(int pid) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_GETSID), "b"(pid)
                      : "memory");
    return ret;
}

int killpg(int pgid, int sig) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_KILLPG), "b"(pgid), "c"(sig)
                      : "memory");
    return ret;
}

int tcsetpgrp(int fd, int pgid) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TCSETPGRP), "b"(fd), "c"(pgid)
                      : "memory");
    return ret;
}

int tcgetpgrp(int fd) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TCGETPGRP), "b"(fd)
                      : "memory");
    return ret;
}

int sys_dns_resolve(const char *name, unsigned char ip[4]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_DNS_RESOLVE), "b"(name), "c"(ip)
                      : "memory");
    return ret;
}

uint32_t sys_fs_free_sectors(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_FS_FREE_SECTORS)
                      : "memory");
    return (uint32_t)ret;
}

void *sys_mmap(int fd, uint32_t offset, uint32_t length) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_MMAP), "b"(fd), "c"(offset), "d"(length)
                      : "memory");
    return (void *)(uint32_t)ret;
}

int sys_munmap(void *addr, uint32_t length) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_MUNMAP), "b"(addr), "c"(length)
                      : "memory");
    return ret;
}

int sys_mkdir(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_MKDIR), "b"(path)
                      : "memory");
    return ret;
}

int sys_chdir(const char *path) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_CHDIR), "b"(path)
                      : "memory");
    return ret;
}

int sys_getcwd(char *buf, int cap) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_GETCWD), "b"(buf), "c"(cap)
                      : "memory");
    return ret;
}

int sys_readdir(const char *dir_path, int *iter, char *name_buf) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_READDIR), "b"(dir_path), "c"(iter), "d"(name_buf)
                      : "memory");
    return ret;
}

uint32_t sys_bcache_sync(void) {
    uint32_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_BCACHE_SYNC)
                      : "memory");
    return ret;
}

int sys_bcache_stats(uint32_t out[5]) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_BCACHE_STATS), "b"(out)
                      : "memory");
    return ret;
}

/*
 * Sigreturn trampoline. The kernel pushes (low to high on user stack):
 *   [trampoline_addr]   ← handler's "return address"
 *   [sig_num]           ← handler's arg
 *   [sigcontext: 64B]
 * before iretting into the user handler. When the handler ret's, ESP
 * lands at sig_num (handler popped trampoline_addr); we add 4 to skip
 * past sig_num so SIGRETURN sees ESP pointing exactly at sigcontext.
 */
/* Defined in asm so we control the exact instruction sequence. The
 * C-visible name is `sigreturn_tramp`; mingw32 prepends an underscore
 * to map C symbols to asm symbols, so the asm label is `_sigreturn_tramp`.
 */
__asm__ (
    ".global _sigreturn_tramp        \n"
    "_sigreturn_tramp:               \n"
    "    add    $4, %esp             \n"
    "    mov    $26, %eax            \n"   /* SYS_SIGRETURN */
    "    int    $0x80                \n"
    /* Should never return — sigreturn restores the saved frame and
     * irets to the pre-signal EIP/ESP. Loop just in case something
     * goes wrong. */
    "1:  hlt                         \n"
    "    jmp 1b                      \n"
);

extern void sigreturn_tramp(void);   /* implemented above in asm */

sighandler_t sigaction(int sig, sighandler_t handler) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SIGACTION), "b"(sig),
                        "c"(handler), "d"(sigreturn_tramp)
                      : "memory");
    return (sighandler_t)(uint32_t)ret;
}

sighandler_t signal(int sig, sighandler_t handler) {
    /* POSIX `signal()` is just a thin wrapper. Kept separate so user
     * code can use either name. */
    return sigaction(sig, handler);
}

/* ---------- Dynamic libc trampolines -------------------------------
 *
 * Session 35 moved the C library implementations out of libuser into
 * libc.bin, which the kernel's dyld layer maps into every new user
 * process at LIBC_BASE = 0x70000000. The first 16 bytes are a
 * magic/version header; the next 256 bytes are a fixed-order array
 * of function pointers (the EXPORT TABLE).
 *
 * Each function below is a thin trampoline that loads the right
 * pointer out of the table and calls into libc. The trampolines are
 * REAL functions (not macros) so taking their address still works
 * (e.g., `int (*p)(const char *) = strcmp` continues to compile).
 *
 * For variadic printf-family functions, we use va_list versions in
 * libc (vprintf/vsprintf/vsnprintf) and wrap them with va_start/end
 * here. That avoids needing GCC's __builtin_va_arg_pack to forward
 * the original variadic arglist through the indirect call. */

#define LIBC_BASE          0x70000000u
#define LIBC_TABLE_OFF     0x10u
#define LIBC_TABLE         ((void * const *)(LIBC_BASE + LIBC_TABLE_OFF))

/* Indices MUST agree with libc/libc.h's enum. */
#define LIBC_FN_LIBC_INFO    0
#define LIBC_FN_STRLEN       1
#define LIBC_FN_STRCMP       2
#define LIBC_FN_STRNCMP      3
#define LIBC_FN_STRCPY       4
#define LIBC_FN_STRNCPY      5
#define LIBC_FN_STRCAT       6
#define LIBC_FN_STRCHR       7
#define LIBC_FN_STRRCHR      8
#define LIBC_FN_STRSTR       9
#define LIBC_FN_MEMSET      10
#define LIBC_FN_MEMCPY      11
#define LIBC_FN_MEMMOVE     12
#define LIBC_FN_MEMCMP      13
#define LIBC_FN_MEMCHR      14
#define LIBC_FN_ATOI        20
#define LIBC_FN_ATOL        21
#define LIBC_FN_STRTOL      22
#define LIBC_FN_ABS         23
#define LIBC_FN_MALLOC      24
#define LIBC_FN_FREE        25
#define LIBC_FN_CALLOC      26
#define LIBC_FN_REALLOC     27
#define LIBC_FN_PUTCHAR     40
#define LIBC_FN_PUTS        41
#define LIBC_FN_VPRINTF     42
#define LIBC_FN_VSPRINTF    43
#define LIBC_FN_VSNPRINTF   44
#define LIBC_FN_MALLOC_BRK   50
#define LIBC_FN_MALLOC_USED  51
#define LIBC_FN_MALLOC_FREE_ 52
#define LIBC_FN_MALLOC_TOTAL 53

/* String — direct pass-through. */
size_t strlen(const char *s) {
    return ((size_t (*)(const char *))LIBC_TABLE[LIBC_FN_STRLEN])(s);
}
int strcmp(const char *a, const char *b) {
    return ((int (*)(const char *, const char *))LIBC_TABLE[LIBC_FN_STRCMP])(a, b);
}
int strncmp(const char *a, const char *b, size_t n) {
    return ((int (*)(const char *, const char *, size_t))
            LIBC_TABLE[LIBC_FN_STRNCMP])(a, b, n);
}
const char *strchr(const char *s, int c) {
    return ((const char *(*)(const char *, int))LIBC_TABLE[LIBC_FN_STRCHR])(s, c);
}

/* Memory. */
void *memset(void *p, int c, size_t n) {
    return ((void *(*)(void *, int, size_t))LIBC_TABLE[LIBC_FN_MEMSET])(p, c, n);
}
void *memcpy(void *d, const void *s, size_t n) {
    return ((void *(*)(void *, const void *, size_t))
            LIBC_TABLE[LIBC_FN_MEMCPY])(d, s, n);
}
int memcmp(const void *a, const void *b, size_t n) {
    return ((int (*)(const void *, const void *, size_t))
            LIBC_TABLE[LIBC_FN_MEMCMP])(a, b, n);
}

/* stdlib. */
int atoi(const char *s) {
    return ((int (*)(const char *))LIBC_TABLE[LIBC_FN_ATOI])(s);
}

/* Heap — trampoline through libc's malloc/free.  Each user process
 * has its own libc.bin .data so the heap state stays per-process. */
void *malloc(size_t size) {
    return ((void *(*)(size_t))LIBC_TABLE[LIBC_FN_MALLOC])(size);
}
void free(void *p) {
    ((void (*)(void *))LIBC_TABLE[LIBC_FN_FREE])(p);
}
uint32_t malloc_brk(void) {
    return ((uint32_t (*)(void))LIBC_TABLE[LIBC_FN_MALLOC_BRK])();
}
uint32_t malloc_total(void) {
    return ((uint32_t (*)(void))LIBC_TABLE[LIBC_FN_MALLOC_TOTAL])();
}
uint32_t malloc_used(void) {
    return ((uint32_t (*)(void))LIBC_TABLE[LIBC_FN_MALLOC_USED])();
}
uint32_t malloc_free_bytes(void) {
    return ((uint32_t (*)(void))LIBC_TABLE[LIBC_FN_MALLOC_FREE_])();
}

/* Stdio. putchar/puts forward straight into libc's putchar_/puts_. */
void putchar(char c) {
    ((void (*)(char))LIBC_TABLE[LIBC_FN_PUTCHAR])(c);
}
void puts(const char *s) {
    ((void (*)(const char *))LIBC_TABLE[LIBC_FN_PUTS])(s);
}

/* printf: shim wraps va_list and dispatches into libc's vprintf_. */
void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ((int (*)(const char *, va_list))LIBC_TABLE[LIBC_FN_VPRINTF])(fmt, args);
    va_end(args);
}

/* Non-zero-initialized marker: forces user.ld's .data section to be
 * emitted with CONTENTS (not as a NOBITS section the linker would
 * collapse). Without this, an init.c-style program with an
 * uninitialized `static char buf[2048]` would have buf living in a
 * .data region that objcopy strips from the flat binary, leaving
 * the kernel ELF loader to map only the .text+.rdata pages — and
 * the very first sys_read into buf would page-fault on the unmapped
 * tail half. The legacy libuser had an initialized global (g_brk =
 * HEAP_START_VA) that accidentally papered over this; we now keep
 * a single explicit marker.
 *
 * `used` keeps the symbol from being garbage-collected even though
 * nothing references it. */
__attribute__((used))
static uint32_t libuser_data_marker = 0xADBEEF35u;

/* sys_brk: only syscall left in libuser that's directly tied to the
 * heap. malloc itself lives in libc.bin (so the implementation is
 * shared across processes); each process's libc.data has its own
 * g_brk pointer. We expose sys_brk here for any user program that
 * wants to manage its own break independently of malloc — and for
 * the [t25] selftest's diagnostics. */
int sys_brk(int new_brk) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_BRK), "b"(new_brk)
                      : "memory");
    return ret;
}
