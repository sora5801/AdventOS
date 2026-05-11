#include "syscall.h"
#include "kprintf.h"
#include "task.h"
#include "pit.h"
#include "rtc.h"
#include "shell.h"
#include "fs.h"
#include "sock.h"
#include "pipe.h"
#include "pty.h"
#include "tmpfs.h"
#include "signal.h"
#include "string.h"
#include "kmalloc.h"
#include "paging.h"
#include "pmm.h"
#include "tty.h"
#include "dns.h"
#include "mmap.h"
#include "bcache.h"
#include "vfs.h"
#include "procfs.h"
#include "lapic.h"
#include "smp.h"
#include "vbe.h"
#include "mouse.h"
#include "ac97.h"
#include "bkl.h"
#include "blkdev.h"

/* Allocate the lowest free fd >= 3 in the calling task's table.
 * Returns the fd index or -1 if the table is full. */
static int alloc_fd(struct task *t) {
    for (int fd = 3; fd < TASK_MAX_FDS; fd++) {
        if (t->fds[fd].kind == FD_FREE) return fd;
    }
    return -1;
}

/* Resolve a pid to a struct task pointer, or NULL if no live task
 * matches. pid == 0 means "the calling task". Used by job-control
 * syscalls (setpgid / getpgid / getsid). */
static struct task *find_task_by_pid(uint32_t pid) {
    if (pid == 0) return task_current();
    for (uint32_t i = 0; i < 16; i++) {
        struct task *t = task_at(i);
        if (t && t->id == pid) return t;
    }
    return NULL;
}

/* Drop a per-fd reference to whatever resource backs `e`. Pipes and
 * tmpfs files use proper refcounting; sockets close immediately
 * (refcount discipline is local to sock.c and adequate for our
 * fork/exec paths today). After the call the caller is expected to
 * mark e->kind = FD_FREE itself. */
static void release_fd(struct task_fd *e) {
    switch (e->kind) {
        case FD_SOCK:    sock_close       (e->obj_idx); break;
        case FD_PIPE_R:  pipe_close_read  (e->obj_idx); break;
        case FD_PIPE_W:  pipe_close_write (e->obj_idx); break;
        case FD_TMPFS:   tmpfs_close      (e->obj_idx); break;
        case FD_PTY_M:   pty_close_master (e->obj_idx); break;
        case FD_PTY_S:   pty_close_slave  (e->obj_idx); break;
        default: break;     /* FD_FS / FD_STDIN / FD_STDOUT have no refcount */
    }
}

#define USER_STR_MAX 256

void syscall_dispatch(struct registers *r) {
    /* The 0xEE IDT gate cleared IF on entry, but several syscalls below
     * (SYS_SLEEP_MS, SYS_YIELD, SYS_EXIT->schedule, SYS_WRITE_STR over a
     * long buffer) need a live timer to make progress. Re-enable here;
     * iret at the tail of the common stub restores the user's saved
     * EFLAGS regardless. */
    __asm__ volatile ("sti");

    /* === Big Kernel Lock (session 38) === Acquired before reading
     * the syscall args so concurrent syscalls from different CPUs
     * serialize on the kernel side. The lock is RELEASED by:
     *   - the SYS_SIGRETURN early-return path (jumps over the
     *     bkl_unlock at the end of this function)
     *   - explicit drop+retake around blocking yields inside
     *     specific syscall implementations (SYS_WAIT, SYS_SLEEP_MS,
     *     SYS_YIELD, SYS_ACCEPT, SYS_READ on a socket, ...)
     *
     * This is the simplest correct serializer. Every syscall body
     * runs as if single-CPU; user-mode execution between syscalls
     * is fully parallel. */
    bkl_lock();

    uint32_t num = r->eax;
    uint32_t a   = r->ebx;
    uint32_t b   = r->ecx;
    uint32_t c   = r->edx;
    int32_t  ret = -1;

    (void)b;
    (void)c;

    switch (num) {
        case SYS_WRITE: {
            kputc((char)a);
            ret = 0;
            break;
        }
        case SYS_GETPID: {
            ret = (int32_t)task_current()->id;
            break;
        }
        case SYS_EXIT: {
            kprintf("\n[user task pid=%u exited code=%d]\n",
                    (unsigned)task_current()->id, (int)a);
            /* Records exit_code, wakes our parent if it's waiting on
             * us, and demotes us to ZOMBIE (parent will harvest via
             * sys_wait) or DEAD (kernel reaper will free). */
            task_exit_current((int)a);
            /* schedule() releases the CPU forever — drop the BKL
             * so other CPUs can keep doing kernel work. */
            bkl_unlock();
            schedule();
            for (;;) __asm__ volatile ("hlt");
        }
        case SYS_YIELD: {
            /* task_yield drops/retakes the BKL itself when held. */
            task_yield();
            ret = 0;
            break;
        }
        case SYS_WRITE_STR: {
            /* Read up to USER_STR_MAX bytes from the user buffer. We're
             * still on the user task's CR3, so the user pointer is
             * directly dereferenceable. A real OS would validate the
             * range and copy_from_user instead. */
            const char *p = (const char *)(uintptr_t)a;
            for (int i = 0; i < USER_STR_MAX && p[i]; i++) {
                kputc(p[i]);
            }
            ret = 0;
            break;
        }
        case SYS_SLEEP_MS: {
            /* pit_sleep itself drops/retakes the BKL while it
             * hlt-waits. Other CPUs can serve syscalls during
             * the sleep. */
            pit_sleep(a);
            ret = 0;
            break;
        }
        case SYS_TIME: {
            struct rtc_time t;
            rtc_read(&t);
            ret = (int32_t)rtc_to_epoch(&t);
            break;
        }
        case SYS_READ_LINE: {
            /* User pointer is dereferenceable directly because we're
             * still on the user task's CR3 in this syscall handler. */
            char *user_buf = (char *)(uintptr_t)a;
            int   cap      = (int)b;
            if (cap > 1024) cap = 1024;
            ret = kshell_read_line(user_buf, cap);
            break;
        }
        /* SYS_KCMD removed in session 14 — userspace shell uses
         * fork/exec/wait now. The case is intentionally absent so
         * an old binary calling SYS_KCMD = 9 falls through to the
         * default handler and gets -1 + an "unknown syscall" log. */
        case SYS_OPEN: {
            const char *uname = (const char *)(uintptr_t)a;
            char name[128];
            int  i;
            for (i = 0; i < (int)sizeof(name) - 1 && uname[i]; i++) name[i] = uname[i];
            name[i] = 0;

            struct task *t = task_current();
            int fd = alloc_fd(t);
            if (fd < 0) { ret = -1; break; }

            /* VFS dispatch: rootfs catches absolute and cwd-relative
             * paths into /; procfs catches /proc/...; the result's
             * .kind tells us which fd flavor to install. */
            struct vfs_inode ino;
            if (vfs_open(name, &ino) >= 0) {
                /* Session 48: enforce read permission on disk-backed
                 * entries. Other vfs kinds (procfs, tmpfs) skip the
                 * check — procfs is informational and unprotected,
                 * tmpfs files are per-process. */
                if (ino.kind == FD_FS && fs_check_perm(ino.obj_idx, FS_PERM_R) == 0) {
                    ret = -1;
                    break;
                }
                t->fds[fd].kind    = ino.kind;
                t->fds[fd].obj_idx = ino.obj_idx;
                t->fds[fd].offset  = 0;
                t->fds[fd].fs_data = ino.fs_data;
                ret = fd;
                break;
            }
            /* Fall back to tmpfs (the in-RAM scratchpad backing `>`). */
            int tmp_idx = tmpfs_open(name);
            if (tmp_idx >= 0) {
                t->fds[fd].kind    = FD_TMPFS;
                t->fds[fd].obj_idx = tmp_idx;
                t->fds[fd].offset  = 0;
                t->fds[fd].fs_data = 0;
                ret = fd;
                break;
            }
            ret = -1;
            break;
        }
        case SYS_OPEN_W: {
            const char *uname = (const char *)(uintptr_t)a;
            char name[FS_NAME_MAX + 1];
            int  i;
            for (i = 0; i < FS_NAME_MAX && uname[i]; i++) name[i] = uname[i];
            name[i] = 0;

            struct task *t = task_current();
            int fd = alloc_fd(t);
            if (fd < 0) { ret = -1; break; }

            int tmp_idx = tmpfs_create(name);
            if (tmp_idx < 0) { ret = -1; break; }

            t->fds[fd].kind    = FD_TMPFS;
            t->fds[fd].obj_idx = tmp_idx;
            t->fds[fd].offset  = 0;
            ret = fd;
            break;
        }
        case SYS_READ: {
            int   fd  = (int)a;
            char *buf = (char *)(uintptr_t)b;
            int   n   = (int)c;

            struct task *t = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)        { ret = -1; break; }
            struct task_fd *e = &t->fds[fd];
            switch (e->kind) {
                case FD_STDIN:
                    /* Mode-aware: canonical or raw, depending on
                     * tty state. SYS_READ_LINE bypasses this and
                     * always uses kshell_read_line. */
                    ret = tty_read(buf, n);
                    break;
                case FD_FS: {
                    /* fs_data routes to the right instance — boot fs
                     * (NULL) or a USB / additional mount. */
                    int rd = fs_read_at(e->fs_data, e->obj_idx, e->offset,
                                        buf, (uint32_t)n);
                    if (rd > 0) e->offset += (uint32_t)rd;
                    ret = rd;
                    break;
                }
                case FD_TMPFS: {
                    int rd = tmpfs_read(e->obj_idx, e->offset,
                                        buf, (uint32_t)n);
                    if (rd > 0) e->offset += (uint32_t)rd;
                    ret = rd;
                    break;
                }
                case FD_PROCFS: {
                    /* /proc files are synthesized fresh on every
                     * read. obj_idx packs (kind, pid). */
                    int rd = procfs_read_by_id(e->obj_idx, e->offset,
                                               buf, (uint32_t)n);
                    if (rd > 0) e->offset += (uint32_t)rd;
                    ret = rd;
                    break;
                }
                case FD_SOCK:
                    ret = sock_read(e->obj_idx, buf, n);
                    break;
                case FD_PIPE_R:
                    ret = pipe_read(e->obj_idx, buf, n);
                    break;
                case FD_PTY_M:
                    ret = pty_master_read(e->obj_idx, buf, n);
                    break;
                case FD_PTY_S:
                    ret = pty_slave_read(e->obj_idx, buf, n);
                    break;
                default:
                    ret = -1;
                    break;
            }
            break;
        }
        case SYS_WRITE_FD: {
            int         fd  = (int)a;
            const char *buf = (const char *)(uintptr_t)b;
            int         n   = (int)c;

            struct task *t = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)         { ret = -1; break; }
            struct task_fd *e = &t->fds[fd];
            switch (e->kind) {
                case FD_STDOUT:
                    for (int i = 0; i < n; i++) kputc(buf[i]);
                    ret = n;
                    break;
                case FD_SOCK:
                    ret = sock_write(e->obj_idx, buf, n);
                    break;
                case FD_PIPE_W:
                    ret = pipe_write(e->obj_idx, buf, n);
                    break;
                case FD_TMPFS:
                    ret = tmpfs_write(e->obj_idx, buf, (uint32_t)n);
                    break;
                case FD_PTY_M:
                    ret = pty_master_write(e->obj_idx, buf, n);
                    break;
                case FD_PTY_S:
                    ret = pty_slave_write(e->obj_idx, buf, n);
                    break;
                default:
                    ret = -1;
                    break;
            }
            break;
        }
        case SYS_CLOSE: {
            int fd = (int)a;
            struct task *t = task_current();
            if (fd < 3 || fd >= TASK_MAX_FDS)         { ret = -1; break; }
            if (t->fds[fd].kind == FD_FREE)           { ret = -1; break; }
            release_fd(&t->fds[fd]);
            t->fds[fd].kind    = FD_FREE;
            t->fds[fd].obj_idx = -1;
            t->fds[fd].offset  = 0;
            ret = 0;
            break;
        }
        case SYS_SOCKET: {
            int sock_idx = sock_create();
            if (sock_idx < 0) { ret = -1; break; }

            struct task *t = task_current();
            int fd = alloc_fd(t);
            if (fd < 0) { sock_close(sock_idx); ret = -1; break; }

            t->fds[fd].kind    = FD_SOCK;
            t->fds[fd].obj_idx = sock_idx;
            ret = fd;
            break;
        }
        case SYS_BIND: {
            int fd   = (int)a;
            int port = (int)b;
            struct task *t = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)         { ret = -1; break; }
            if (t->fds[fd].kind != FD_SOCK)           { ret = -1; break; }
            ret = sock_bind(t->fds[fd].obj_idx, (uint16_t)port);
            break;
        }
        case SYS_LISTEN: {
            int fd      = (int)a;
            int backlog = (int)b;
            struct task *t = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)         { ret = -1; break; }
            if (t->fds[fd].kind != FD_SOCK)           { ret = -1; break; }
            ret = sock_listen(t->fds[fd].obj_idx, backlog);
            break;
        }
        case SYS_CONNECT: {
            int            fd   = (int)a;
            const uint8_t *uip  = (const uint8_t *)(uintptr_t)b;
            int            port = (int)c;
            struct task   *t    = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)         { ret = -1; break; }
            if (t->fds[fd].kind != FD_SOCK)           { ret = -1; break; }
            if (!uip)                                 { ret = -1; break; }
            uint8_t ip[4];
            for (int i = 0; i < 4; i++) ip[i] = uip[i];
            ret = sock_connect(t->fds[fd].obj_idx, ip, (uint16_t)port);
            break;
        }
        case SYS_ACCEPT: {
            int fd = (int)a;
            struct task *t = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)         { ret = -1; break; }
            if (t->fds[fd].kind != FD_SOCK)           { ret = -1; break; }

            int conn_sock = sock_accept(t->fds[fd].obj_idx);
            if (conn_sock < 0) { ret = -1; break; }

            int conn_fd = alloc_fd(t);
            if (conn_fd < 0) { sock_close(conn_sock); ret = -1; break; }

            t->fds[conn_fd].kind    = FD_SOCK;
            t->fds[conn_fd].obj_idx = conn_sock;
            ret = conn_fd;
            break;
        }
        case SYS_PIPE: {
            int *ufds = (int *)(uintptr_t)a;
            struct task *t = task_current();

            int rfd = alloc_fd(t);
            if (rfd < 0) { ret = -1; break; }
            /* Tentatively claim rfd so alloc_fd doesn't return it
             * again for wfd. */
            t->fds[rfd].kind    = FD_PIPE_R;
            t->fds[rfd].obj_idx = -1;
            int wfd = alloc_fd(t);
            if (wfd < 0) {
                t->fds[rfd].kind = FD_FREE;
                ret = -1; break;
            }

            int p = pipe_new();
            if (p < 0) {
                t->fds[rfd].kind = FD_FREE;
                ret = -1; break;
            }

            t->fds[rfd].kind    = FD_PIPE_R;
            t->fds[rfd].obj_idx = p;
            t->fds[wfd].kind    = FD_PIPE_W;
            t->fds[wfd].obj_idx = p;
            ufds[0] = rfd;
            ufds[1] = wfd;
            ret = 0;
            break;
        }
        case SYS_OPENPTY: {
            /* Same shape as SYS_PIPE — claim two fd slots, then a pty
             * object, install both, write the fd ints back to user. */
            int *ufds = (int *)(uintptr_t)a;
            struct task *t = task_current();

            int mfd = alloc_fd(t);
            if (mfd < 0) { ret = -1; break; }
            t->fds[mfd].kind    = FD_PTY_M;
            t->fds[mfd].obj_idx = -1;
            int sfd = alloc_fd(t);
            if (sfd < 0) {
                t->fds[mfd].kind = FD_FREE;
                ret = -1; break;
            }
            int p = pty_new();
            if (p < 0) {
                t->fds[mfd].kind = FD_FREE;
                t->fds[sfd].kind = FD_FREE;
                ret = -1; break;
            }
            t->fds[mfd].kind    = FD_PTY_M;
            t->fds[mfd].obj_idx = p;
            t->fds[sfd].kind    = FD_PTY_S;
            t->fds[sfd].obj_idx = p;
            ufds[0] = mfd;
            ufds[1] = sfd;
            ret = 0;
            break;
        }
        case SYS_DUP2: {
            int oldfd = (int)a;
            int newfd = (int)b;
            struct task *t = task_current();
            if (oldfd < 0 || oldfd >= TASK_MAX_FDS)   { ret = -1; break; }
            if (newfd < 0 || newfd >= TASK_MAX_FDS)   { ret = -1; break; }
            if (t->fds[oldfd].kind == FD_FREE)        { ret = -1; break; }
            if (oldfd == newfd) { ret = newfd; break; }

            /* Drop newfd's existing reference (if any) before
             * overwriting. POSIX dup2 is "atomic close+dup" — same
             * end-state, no fd ever briefly unallocated. */
            if (t->fds[newfd].kind != FD_FREE) {
                release_fd(&t->fds[newfd]);
            }

            t->fds[newfd] = t->fds[oldfd];

            /* Bump the underlying object's refcount so close(oldfd)
             * doesn't free what newfd is still pointing at. */
            switch (t->fds[oldfd].kind) {
                case FD_PIPE_R:  pipe_inc_read  (t->fds[oldfd].obj_idx); break;
                case FD_PIPE_W:  pipe_inc_write (t->fds[oldfd].obj_idx); break;
                case FD_TMPFS:   tmpfs_inc_ref  (t->fds[oldfd].obj_idx); break;
                case FD_PTY_M:   pty_inc_master (t->fds[oldfd].obj_idx); break;
                case FD_PTY_S:   pty_inc_slave  (t->fds[oldfd].obj_idx); break;
                /* Sockets and FS handles share via memcpy: there's no
                 * per-fd refcount today. fork()'s own dup-the-table
                 * behavior has the same semantics. */
                default: break;
            }
            ret = newfd;
            break;
        }
        case SYS_FORK: {
            struct task *child = task_fork(r);
            if (!child) { ret = -1; break; }
            /* Parent path: return child's pid. The child's "return" is
             * synthesized inside task_fork — it'll iret to the same
             * user EIP with EAX=0 the next time it's scheduled. */
            ret = (int32_t)child->id;
            break;
        }
        case SYS_EXEC: {
            /* Snapshot path + argv into kernel memory BEFORE we touch
             * the user PD (task_exec_inplace will free it). After that
             * point, every user pointer is invalid. */
            const char  *upath = (const char *)(uintptr_t)a;
            const char **uargv = (const char **)(uintptr_t)b;

            char path[64];
            int i;
            for (i = 0; i < (int)sizeof(path) - 1 && upath[i]; i++) path[i] = upath[i];
            path[i] = 0;

            /* Walk uargv (NULL-terminated user pointer array), copy
             * each string into a kmalloc'd buf. Cap at 16 args. */
            #define EXEC_MAX_ARGS 16
            char *argv_kbufs[EXEC_MAX_ARGS] = {0};
            int   argc = 0;
            if (uargv) {
                for (argc = 0; argc < EXEC_MAX_ARGS && uargv[argc]; argc++) {
                    const char *s = uargv[argc];
                    int len; for (len = 0; s[len]; len++) {}
                    char *kb = (char *)kmalloc((size_t)len + 1);
                    if (!kb) {
                        for (int j = 0; j < argc; j++) kfree(argv_kbufs[j]);
                        ret = -1;
                        goto exec_done;
                    }
                    for (int j = 0; j < len; j++) kb[j] = s[j];
                    kb[len] = 0;
                    argv_kbufs[argc] = kb;
                }
            }
            if (argc == 0) {
                /* No argv passed — synthesize argv[0] = path so _start
                 * always sees at least one arg. */
                int len; for (len = 0; path[len]; len++) {}
                char *kb = (char *)kmalloc((size_t)len + 1);
                if (!kb) { ret = -1; goto exec_done; }
                for (int j = 0; j < len; j++) kb[j] = path[j];
                kb[len] = 0;
                argv_kbufs[0] = kb;
                argc = 1;
            }

            int err = task_exec_inplace(r, path, argc,
                                        (const char *const *)argv_kbufs);

            /* The strings have been copied onto the new user stack by
             * elf_setup_args; we own the kernel copies. Free them
             * regardless of success/failure. */
            for (int j = 0; j < argc; j++) kfree(argv_kbufs[j]);

            if (err != 0) ret = -1;
            else          ret = 0;
            /* On success r->eip/useresp have been rewritten — when the
             * isr_common_stub iret runs we land in the new program.
             * _start's first instruction overwrites eax with argc, so
             * the dispatcher's `r->eax = ret` below is harmless. */
        exec_done:
            break;
        }
        case SYS_WAIT: {
            /* task_wait_current may set state=BLOCKED_ON_CHILD and
             * schedule(). When BLOCKED, the calling task isn't
             * running ANYWHERE, so it can't release the BKL — that
             * would block every other CPU's kernel work until the
             * child exits, possibly forever. Drop BKL before the
             * blocking wait, re-take after wakeup. */
            int  *uout = (int *)(uintptr_t)a;
            int   exit_code = 0;
            bkl_unlock();
            int   pid = task_wait_current(uout ? &exit_code : NULL);
            bkl_lock();
            if (pid > 0 && uout) *uout = exit_code;
            ret = pid;
            break;
        }
        case SYS_WAIT_NB: {
            int  *uout = (int *)(uintptr_t)a;
            int   exit_code = 0;
            int   pid = task_waitpid_nb_current(uout ? &exit_code : NULL);
            if (pid > 0 && uout) *uout = exit_code;
            ret = pid;
            break;
        }
        case SYS_GETCPU: {
            ret = (int32_t)lapic_id();
            break;
        }
        case SYS_SMP_STATS: {
            /* Layout of out[8]:
             *   out[0..3]  = LAPIC-timer tick count for cpu 0..3
             *   out[4..7]  = non-idle dispatch count for cpu 0..3
             * Returns N (number of valid CPUs). Used by [t22] to
             * verify APs are actually scheduling work. */
            uint32_t *uout = (uint32_t *)(uintptr_t)a;
            if (!uout || (uintptr_t)uout >= 0xC0000000u) { ret = -1; break; }
            extern volatile uint32_t g_lapic_tick_count[8];
            extern volatile uint32_t g_cpu_dispatch[8];
            int n = smp_cpu_count();
            if (n > 4) n = 4;
            for (int i = 0; i < n; i++) {
                uout[i]     = g_lapic_tick_count[i];
                uout[i + 4] = g_cpu_dispatch[i];
            }
            for (int i = n; i < 4; i++) { uout[i] = 0; uout[i + 4] = 0; }
            ret = n;
            break;
        }
        case SYS_MOUSE_STATE: {
            int32_t *uout = (int32_t *)(uintptr_t)a;
            if (!uout || (uintptr_t)uout >= 0xC0000000u) { ret = -1; break; }
            struct mouse_state ms;
            mouse_get_state(&ms);
            uout[0] = ms.x;
            uout[1] = ms.y;
            uout[2] = (int32_t)ms.buttons;
            uout[3] = (int32_t)ms.packets;
            /* "Alive" means we've ever seen a packet OR cursor position
             * is non-default. The init centers the cursor, so a freshly
             * booted system without mouse activity reports alive=1
             * because mouse_init sets x/y to mid-screen. That's fine —
             * the GUI just polls and draws. */
            ret = 1;
            break;
        }
        case SYS_AUDIO_PLAY: {
            const void *upcm = (const void *)(uintptr_t)a;
            int         n    = (int)b;
            if (!upcm || (uintptr_t)upcm >= 0xC0000000u) { ret = -1; break; }
            if (n <= 0 || n > (1 << 20)) { ret = -1; break; }
            /* ac97_play copies — userspace ptr deref happens here, in
             * the active (user) PD context. */
            ret = ac97_play(upcm, n);
            break;
        }
        case SYS_BLOCK_INFO: {
            uint32_t idx = a;
            struct sys_block_info *uout = (struct sys_block_info *)(uintptr_t)b;
            struct blkdev *d = blkdev_get((int)idx);
            if (!d || !uout) { ret = -1; break; }
            uout->block_size = d->block_size;
            uout->n_blocks   = d->n_blocks;
            for (int i = 0; i < 16; i++) uout->name[i] = d->name[i];
            ret = 0;
            break;
        }
        case SYS_BLOCK_READ:
        case SYS_BLOCK_WRITE: {
            const struct sys_block_args *ua =
                (const struct sys_block_args *)(uintptr_t)a;
            if (!ua) { ret = -1; break; }
            uint32_t idx = ua->dev_idx;
            uint32_t lba = ua->lba;
            uint32_t n   = ua->n_blocks;
            void    *ubuf = ua->buf;
            struct blkdev *d = blkdev_get((int)idx);
            if (!d || !ubuf || n == 0 || n > 32) { ret = -1; break; }
            if (lba + n > d->n_blocks)         { ret = -1; break; }
            if ((uintptr_t)ubuf >= 0xC0000000u){ ret = -1; break; }

            /* Bounce through a kmalloc'd kernel buffer. The USB MSC
             * driver's underlying UHCI bulk transfer DMAs by physical
             * address, and user pages don't have phys==virt the way
             * kernel pages do. ATA goes through PIO and would tolerate
             * the user pointer directly, but using one path keeps the
             * code uniform. */
            uint32_t bytes = n * d->block_size;
            void *kbuf = kmalloc(bytes);
            if (!kbuf) { ret = -1; break; }

            if (r->eax == SYS_BLOCK_READ) {
                ret = d->read(d, lba, n, kbuf);
                if (ret == 0) {
                    uint8_t *src = (uint8_t *)kbuf;
                    uint8_t *dst = (uint8_t *)ubuf;
                    for (uint32_t i = 0; i < bytes; i++) dst[i] = src[i];
                }
            } else {
                const uint8_t *src = (const uint8_t *)ubuf;
                uint8_t *dst = (uint8_t *)kbuf;
                for (uint32_t i = 0; i < bytes; i++) dst[i] = src[i];
                ret = d->write(d, lba, n, kbuf);
            }
            kfree(kbuf);
            break;
        }
        case SYS_TTY_CURSOR: {
            /* Place the console cursor at (row=a, col=b) in BOTH the
             * VGA text grid AND the framebuffer console. Used by the
             * vi-style editor (session 46) to redraw in place without
             * scrolling everything up. */
            extern void vga_set_cursor(int, int);
            extern void fbcon_set_cursor(int, int);
            vga_set_cursor((int)a, (int)b);
            fbcon_set_cursor((int)a, (int)b);
            ret = 0;
            break;
        }
        case SYS_TTY_CLEAR: {
            extern void vga_clear(void);
            extern void fbcon_clear(void);
            vga_clear();
            fbcon_clear();
            ret = 0;
            break;
        }
        case SYS_TTY_CLEAR_EOL: {
            extern void vga_clear_to_eol(void);
            extern void fbcon_clear_to_eol(void);
            vga_clear_to_eol();
            fbcon_clear_to_eol();
            ret = 0;
            break;
        }
        case SYS_GETUID: {
            ret = (int)task_current()->uid;
            break;
        }
        case SYS_GETGID: {
            ret = (int)task_current()->gid;
            break;
        }
        case SYS_SETUID: {
            struct task *t = task_current();
            uint32_t target = a;
            if (target > 0xFFFFu) { ret = -1; break; }
            if (t->uid == 0) {
                /* root: may set any uid. */
                t->uid = (uint16_t)target;
                ret = 0;
            } else if ((uint16_t)target == t->uid) {
                /* No-op for non-root setting current uid. */
                ret = 0;
            } else {
                ret = -1;
            }
            break;
        }
        case SYS_SETGID: {
            struct task *t = task_current();
            uint32_t target = a;
            if (target > 0xFFFFu) { ret = -1; break; }
            if (t->uid == 0) {
                t->gid = (uint16_t)target;
                ret = 0;
            } else if ((uint16_t)target == t->gid) {
                ret = 0;
            } else {
                ret = -1;
            }
            break;
        }
        case SYS_FS_OWNER: {
            const char *path = (const char *)(uintptr_t)a;
            if (!path) { ret = -1; break; }
            int idx = fs_open(path);
            if (idx < 0) { ret = -1; break; }
            int uid = fs_entry_uid(idx);
            int gid = fs_entry_gid(idx);
            if (uid < 0 || gid < 0) { ret = -1; break; }
            ret = (uid << 16) | (gid & 0xFFFF);
            break;
        }
        case SYS_FS_MODE: {
            const char *path = (const char *)(uintptr_t)a;
            if (!path) { ret = -1; break; }
            int idx = fs_open(path);
            if (idx < 0) { ret = -1; break; }
            ret = fs_entry_mode(idx);
            break;
        }
        case SYS_CHMOD: {
            /* Only the file's owner or root may chmod. */
            const char *path = (const char *)(uintptr_t)a;
            uint32_t mode = b;
            if (!path) { ret = -1; break; }
            int idx = fs_open(path);
            if (idx < 0) { ret = -1; break; }
            struct task *cur = task_current();
            int owner = fs_entry_uid(idx);
            if (cur->uid != 0 && cur->uid != (uint16_t)owner) {
                ret = -1;
                break;
            }
            ret = fs_chmod_idx(idx, (uint16_t)mode);
            break;
        }
        case SYS_CHOWN: {
            /* Only root may chown. */
            const char *path = (const char *)(uintptr_t)a;
            uint32_t new_uid = b;
            uint32_t new_gid = c;
            if (!path) { ret = -1; break; }
            struct task *cur = task_current();
            if (cur->uid != 0) { ret = -1; break; }
            int idx = fs_open(path);
            if (idx < 0) { ret = -1; break; }
            ret = fs_chown_idx(idx, (uint16_t)new_uid, (uint16_t)new_gid);
            break;
        }
        case SYS_FB_MMAP: {
            /* Map the framebuffer's physical pages into the calling
             * task's PD with USER+WRITABLE flags, at a fresh user VA
             * carved out of the mmap window. Returns the VA, or 0
             * if VBE/fbcon isn't enabled.
             *
             * Unlike file-backed mmap (which lazy-loads via #PF on
             * first touch), the FB mapping is eager — we install
             * every page right now with a real physical backing
             * (the FB itself, not zero-filled pages). The FB pages
             * are at fb_phys (typically 0xFD000000 on QEMU std-vga)
             * and were already mapped into the kernel PD by
             * vbe_init; we add a parallel USER-flagged mapping in
             * the user PD pointing at the same physical bytes. */
            const struct vbe_state *v = vbe_state();
            if (!v->enabled) { ret = 0; break; }

            struct task *t = task_current();
            uint32_t fb_size = v->fb_size;
            uint32_t aligned = (fb_size + 0xFFFu) & ~0xFFFu;
            if (t->mmap_brk + aligned > USER_MMAP_MAX) { ret = 0; break; }

            uint32_t va_start = t->mmap_brk;
            uint32_t va_end   = va_start + aligned;
            uint32_t fb_phys  = v->fb_phys & ~0xFFFu;

            /* Eager-map every FB page. Use paging_map_in (per-PD)
             * with PTE_USER + PTE_WRITABLE. The physical pages are
             * the same MMIO region the kernel maps in vbe_init —
             * we're just adding USER access for the calling
             * process. */
            for (uint32_t va = va_start; va < va_end; va += 0x1000) {
                uint32_t phys = fb_phys + (va - va_start);
                if (paging_map_in((uint32_t *)(uintptr_t)t->cr3,
                                  va, phys,
                                  PTE_USER | PTE_WRITABLE) != 0) {
                    /* Roll back — unmap what we mapped, restore brk. */
                    for (uint32_t v2 = va_start; v2 < va; v2 += 0x1000) {
                        uint32_t *pd = (uint32_t *)(uintptr_t)t->cr3;
                        uint32_t pdi = v2 >> 22;
                        if (pd[pdi] & 1) {
                            uint32_t *pt = (uint32_t *)(uintptr_t)
                                           (pd[pdi] & ~0xFFFu);
                            pt[(v2 >> 12) & 0x3FF] = 0;
                        }
                    }
                    ret = 0;
                    goto fb_mmap_done;
                }
                /* Active PD — flush TLB on this CPU. paging_map_in
                 * does this for the kernel master PD only. */
                __asm__ volatile ("invlpg (%0)" :: "r"(va) : "memory");
            }

            t->mmap_brk = va_end;
            /* Track in the mmap region table so SYS_MUNMAP can find
             * it. fs_idx = -1 marks it as device-backed (no file
             * source). The lazy-load path in mmap_handle_fault
             * checks fs_idx and skips reads for negative indices
             * — but we've already eagerly mapped, so no fault
             * should ever land here. */
            for (int i = 0; i < TASK_MMAP_MAX; i++) {
                if (!t->mmaps[i].in_use) {
                    t->mmaps[i].in_use      = 1;
                    t->mmaps[i].va_start    = va_start;
                    t->mmaps[i].va_end      = va_end;
                    t->mmaps[i].fs_idx      = -1;
                    t->mmaps[i].file_offset = 0;
                    t->mmaps[i].file_length = 0;
                    break;
                }
            }
            ret = (int32_t)va_start;
        fb_mmap_done:
            break;
        }
        case SYS_FBINFO: {
            /* Report VBE/fbcon status to userspace. ebx is a writable
             * uint32_t out[4]; we fill it with width/height/bpp/pitch
             * if fbcon is enabled. Return value is the enabled flag.
             * Cheap user-pointer sanity check: must be non-NULL and
             * lie below KERNEL_BASE. */
            uint32_t *uout = (uint32_t *)(uintptr_t)a;
            if (!uout || (uintptr_t)uout >= 0xC0000000u) {
                ret = -1;
                break;
            }
            const struct vbe_state *v = vbe_state();
            if (!v->enabled) {
                uout[0] = uout[1] = uout[2] = uout[3] = 0;
                ret = 0;
            } else {
                uout[0] = v->width;
                uout[1] = v->height;
                uout[2] = v->bpp;
                uout[3] = v->pitch;
                ret = 1;
            }
            break;
        }
        case SYS_KILL: {
            ret = signal_send((uint32_t)a, (int)b);
            break;
        }
        case SYS_SIGACTION: {
            void *prev = signal_install((int)a, (void *)(uintptr_t)b,
                                        (void *)(uintptr_t)c);
            ret = (int32_t)(uintptr_t)prev;
            break;
        }
        case SYS_SIGRETURN: {
            /* Restores the pre-signal register frame onto our `r`.
             * The dispatcher tail's `r->eax = ret` would clobber the
             * eax we just restored, so early-return here and skip
             * both the eax write AND the signal-delivery hook
             * (we just FINISHED a delivery; recursing would be
             * incorrect). */
            signal_sigreturn(r);
            bkl_unlock();
            return;
        }
        case SYS_TTY_SET_MODE: {
            uint32_t prev = tty_get_mode();
            tty_set_mode((uint32_t)a);
            ret = (int32_t)prev;
            break;
        }
        case SYS_TTY_GET_MODE: {
            ret = (int32_t)tty_get_mode();
            break;
        }
        case SYS_TTY_INJECT: {
            const char *p = (const char *)(uintptr_t)a;
            int len = (int)b;
            if (len < 0)         { ret = -1; break; }
            if (len > 256)       len = 256;
            /* Snapshot into kernel space — avoids the user
             * potentially racing the inject against an unmap. */
            char buf[256];
            for (int i = 0; i < len; i++) buf[i] = p[i];
            ret = tty_inject(buf, len);
            break;
        }
        case SYS_SETPGID: {
            uint32_t pid  = (uint32_t)a;
            uint32_t pgid = (uint32_t)b;
            struct task *t = find_task_by_pid(pid);
            if (!t) { ret = -1; break; }
            /* pgid == 0 means "use pid". Real POSIX has restrictions
             * (must be in the same session, can't move out of session
             * leader, etc.) — we don't enforce them. */
            if (pgid == 0) pgid = t->id;
            t->pgid = pgid;
            ret = 0;
            break;
        }
        case SYS_GETPGID: {
            struct task *t = find_task_by_pid((uint32_t)a);
            ret = t ? (int32_t)t->pgid : -1;
            break;
        }
        case SYS_SETSID: {
            /* Self only. Becomes session + pgrp leader (sid = pgid =
             * pid). POSIX rejects this if the caller is already a
             * pgrp leader; we don't bother — the only realistic
             * caller is the shell at startup. */
            struct task *t = task_current();
            t->sid  = t->id;
            t->pgid = t->id;
            ret = (int32_t)t->sid;
            break;
        }
        case SYS_GETSID: {
            struct task *t = find_task_by_pid((uint32_t)a);
            ret = t ? (int32_t)t->sid : -1;
            break;
        }
        case SYS_KILLPG: {
            ret = signal_send_pgrp((uint32_t)a, (int)b);
            break;
        }
        case SYS_TCSETPGRP: {
            /* For the console TTY (fd 0 / FD_STDIN), set the global
             * tty fg_pgrp. For a pty slave fd, set THAT pty's per-pty
             * fg_pgrp — which is what pty_master_write consults for
             * Ctrl-C/Z/\ signal delivery (session 56). */
            int fd = (int)a;
            uint32_t pgid = (uint32_t)b;
            struct task *t = task_current();
            if (fd >= 0 && fd < TASK_MAX_FDS &&
                t->fds[fd].kind == FD_PTY_S) {
                pty_set_fg_pgrp(t->fds[fd].obj_idx, (int)pgid);
            } else {
                tty_set_fg_pgrp(pgid);
            }
            ret = 0;
            break;
        }
        case SYS_TCGETPGRP: {
            int fd = (int)a;
            struct task *t = task_current();
            if (fd >= 0 && fd < TASK_MAX_FDS &&
                t->fds[fd].kind == FD_PTY_S) {
                ret = pty_get_fg_pgrp(t->fds[fd].obj_idx);
            } else {
                ret = (int32_t)tty_get_fg_pgrp();
            }
            break;
        }
        case SYS_FS_FREE_SECTORS: {
            ret = (int32_t)fs_free_sectors();
            break;
        }
        case SYS_MMAP: {
            int      fd  = (int)a;
            uint32_t off = b;
            uint32_t len = c;
            struct task *t = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)        { ret = 0; break; }
            if (t->fds[fd].kind != FD_FS)            { ret = 0; break; }
            void *va = mmap_register((uint32_t)t->fds[fd].obj_idx,
                                     off, len);
            ret = (int32_t)(uintptr_t)va;
            break;
        }
        case SYS_MUNMAP: {
            uint32_t addr = a;
            uint32_t len  = b;
            ret = mmap_unregister(addr, len);
            break;
        }
        case SYS_MKDIR: {
            const char *upath = (const char *)(uintptr_t)a;
            char path[128];
            int  i;
            for (i = 0; i < (int)sizeof(path) - 1 && upath[i]; i++) path[i] = upath[i];
            path[i] = 0;
            int rc = vfs_mkdir(path);
            ret = (rc < 0) ? -1 : 0;
            break;
        }
        case SYS_CHDIR: {
            const char *upath = (const char *)(uintptr_t)a;
            char path[128];
            int  i;
            for (i = 0; i < (int)sizeof(path) - 1 && upath[i]; i++) path[i] = upath[i];
            path[i] = 0;

            /* "/" means root; anything else has to resolve to an
             * existing directory entry. */
            int target;
            if (path[0] == '/' && path[1] == 0) {
                target = -1;     /* sentinel for ROOT */
            } else {
                target = fs_open(path);
                if (target < 0) { ret = -1; break; }
                if (fs_entry_type(target) != FS_TYPE_DIR) {
                    ret = -1; break;
                }
            }
            task_current()->cwd_dir = (target < 0) ? FS_DIR_ROOT
                                                   : (uint8_t)target;
            ret = 0;
            break;
        }
        case SYS_GETCWD: {
            char *ubuf = (char *)(uintptr_t)a;
            int   cap  = (int)b;
            if (cap <= 0) { ret = -1; break; }

            /* Walk parent pointers from cwd to root, collecting
             * names; then emit them in reverse order with leading
             * slashes. */
            uint8_t cur = task_current()->cwd_dir;
            if (cur == FS_DIR_ROOT) {
                if (cap < 2) { ret = -1; break; }
                ubuf[0] = '/'; ubuf[1] = 0;
                ret = 1;
                break;
            }
            int idxs[16];
            int n = 0;
            while (cur != FS_DIR_ROOT && n < 16) {
                idxs[n++] = cur;
                int p = fs_entry_parent(cur);
                if (p < 0) { n = -1; break; }
                cur = (uint8_t)p;
            }
            if (n < 0) { ret = -1; break; }

            int off = 0;
            for (int j = n - 1; j >= 0; j--) {
                if (off + 1 >= cap) { ret = -1; goto getcwd_done; }
                ubuf[off++] = '/';
                const char *nm = fs_name(idxs[j]);
                for (int k = 0; k < FS_NAME_MAX && nm[k]; k++) {
                    if (off + 1 >= cap) { ret = -1; goto getcwd_done; }
                    ubuf[off++] = nm[k];
                }
            }
            ubuf[off] = 0;
            ret = off;
        getcwd_done:
            break;
        }
        case SYS_READDIR: {
            const char *udir = (const char *)(uintptr_t)a;
            int        *uiter = (int *)(uintptr_t)b;
            char       *uname = (char *)(uintptr_t)c;

            char path[128];
            int  i;
            for (i = 0; i < (int)sizeof(path) - 1 && udir[i]; i++) path[i] = udir[i];
            path[i] = 0;

            /* VFS dispatch handles rootfs entries, /proc files, and
             * synthesizes mount-point names when listing /. */
            int it = uiter ? *uiter : 0;
            int next = vfs_readdir(path, &it, uname);
            if (uiter) *uiter = it;
            ret = next;
            break;
        }
        case SYS_BCACHE_SYNC: {
            ret = (int32_t)bcache_sync();
            break;
        }
        case SYS_BCACHE_STATS: {
            uint32_t *uout = (uint32_t *)(uintptr_t)a;
            if (!uout) { ret = -1; break; }
            uout[0] = bcache_hits();
            uout[1] = bcache_misses();
            uout[2] = bcache_logical_writes();
            uout[3] = bcache_disk_writes();
            uout[4] = bcache_dirty();
            ret = 0;
            break;
        }
        case SYS_DNS_RESOLVE: {
            const char *uname = (const char *)(uintptr_t)a;
            uint8_t    *uout  = (uint8_t *)(uintptr_t)b;
            char name[64];
            int  i;
            for (i = 0; i < (int)sizeof(name) - 1 && uname[i]; i++) name[i] = uname[i];
            name[i] = 0;
            struct ip_addr ip;
            int rc = dns_resolve(name, &ip);
            if (rc == 0 && uout) {
                uout[0] = ip.b[0]; uout[1] = ip.b[1];
                uout[2] = ip.b[2]; uout[3] = ip.b[3];
            }
            ret = rc;
            break;
        }
        case SYS_FS_WRITE: {
            /* Snapshot the path into kernel space so a long-ish
             * write that needs many ata_write_sector calls can't be
             * tripped up by user-side state changes mid-call. The
             * data buffer stays user-side: we read it sector-by-
             * sector via vfs_write_all, all on the user task's CR3. */
            const char *uname = (const char *)(uintptr_t)a;
            const void *udata = (const void *)(uintptr_t)b;
            uint32_t    n     = c;
            char path[128];
            int  i;
            for (i = 0; i < (int)sizeof(path) - 1 && uname[i]; i++) path[i] = uname[i];
            path[i] = 0;
            /* Session 48: permission enforcement on overwrites. If the
             * file already exists, require write permission. New-file
             * creation skips the check — there's no enforceable
             * "parent directory write" concept on a flat-ish FS. */
            int existing = fs_open(path);
            if (existing >= 0 && fs_check_perm(existing, FS_PERM_W) == 0) {
                ret = -1;
                break;
            }
            ret = vfs_write_all(path, udata, n);
            break;
        }
        case SYS_BRK: {
            /* Linux-style brk(2): brk(0) returns current break;
             * brk(new) attempts to set the break to `new`. Returns
             * the actual new break, which equals `new` on success
             * or the unchanged break on failure (out-of-range or
             * out-of-memory). The libuser malloc reads the return
             * value and treats != requested as failure. */
            uint32_t want = a;
            struct task *t = task_current();

            if (want == 0) { ret = (int32_t)t->heap_brk; break; }

            if (want < USER_HEAP_START || want > USER_HEAP_MAX) {
                ret = (int32_t)t->heap_brk;
                break;
            }

            uint32_t cur_pg = (t->heap_brk + 0xFFFu) & ~0xFFFu;
            uint32_t new_pg = (want        + 0xFFFu) & ~0xFFFu;

            if (new_pg > cur_pg) {
                /* Grow: map fresh pages from cur_pg to new_pg. If
                 * any allocation fails, roll back the pages we
                 * managed to map and report failure (return
                 * unchanged break). */
                uint32_t mapped_to = cur_pg;
                int ok = 1;
                for (uint32_t va = cur_pg; va < new_pg; va += 4096) {
                    void *page = pmm_alloc_page();
                    if (!page) { ok = 0; break; }
                    /* Zero the page so the user heap doesn't expose
                     * a previous owner's bytes. */
                    for (int i = 0; i < 4096 / 4; i++) {
                        ((uint32_t *)page)[i] = 0;
                    }
                    if (paging_map_in((uint32_t *)(uintptr_t)t->cr3,
                                      va, (uintptr_t)page,
                                      PTE_USER | PTE_WRITABLE) != 0) {
                        pmm_free_page(page);
                        ok = 0;
                        break;
                    }
                    mapped_to = va + 4096;
                }
                if (!ok) {
                    /* Roll back. */
                    for (uint32_t va = cur_pg; va < mapped_to; va += 4096) {
                        /* paging_map_in installed it on the user PD;
                         * unmapping cleanly without a paging_unmap_in
                         * helper is fiddly. The PD is going to be
                         * destroyed anyway when this process exits;
                         * the simplest sound behavior is to leave
                         * the partial mapping in place and tell the
                         * caller "no, I couldn't honor your request"
                         * by returning the unchanged break. The
                         * partial mapping is harmless — the user
                         * malloc won't touch addresses beyond the
                         * break it observes. */
                        (void)va;
                    }
                    ret = (int32_t)t->heap_brk;
                    break;
                }
            }
            /* Shrink case (new_pg < cur_pg) intentionally not
             * implemented yet — we have no paging_unmap_in helper.
             * heap_brk is just bumped down; the pages stay mapped
             * until the process exits. Real brk would unmap. */

            t->heap_brk = want;
            ret = (int32_t)t->heap_brk;
            break;
        }
        default:
            kprintf("[unknown syscall %u from pid %u]\n",
                    (unsigned)num, (unsigned)task_current()->id);
            ret = -1;
            break;
    }

    /* Stash return value where iret will restore it as EAX. */
    r->eax = (uint32_t)ret;

    /* Signal delivery happens at the very tail of the syscall, just
     * before isr_common_stub's iret. If a signal is pending, this
     * mutates r->eip / r->useresp so the iret enters the user
     * handler instead of returning to the post-INT $0x80 instruction.
     * The handler eventually trampolines into SYS_SIGRETURN, which
     * restores the original frame. */
    signal_check_and_deliver(r);

    /* Release the BKL last — signal delivery touches per-task signal
     * tables, also kernel-shared state. */
    bkl_unlock();
}
