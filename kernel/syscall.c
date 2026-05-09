#include "syscall.h"
#include "kprintf.h"
#include "task.h"
#include "pit.h"
#include "rtc.h"
#include "shell.h"
#include "fs.h"
#include "sock.h"
#include "string.h"
#include "kmalloc.h"

#define USER_STR_MAX 256

void syscall_dispatch(struct registers *r) {
    /* The 0xEE IDT gate cleared IF on entry, but several syscalls below
     * (SYS_SLEEP_MS, SYS_YIELD, SYS_EXIT->schedule, SYS_WRITE_STR over a
     * long buffer) need a live timer to make progress. Re-enable here;
     * iret at the tail of the common stub restores the user's saved
     * EFLAGS regardless. */
    __asm__ volatile ("sti");

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
            schedule();
            for (;;) __asm__ volatile ("hlt");
        }
        case SYS_YIELD: {
            schedule();
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
            /* pit_sleep `hlt`s until the deadline, so other tasks
             * (kernel and user) get scheduled in our place. */
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
            char name[FS_NAME_MAX + 1];
            int  i;
            for (i = 0; i < FS_NAME_MAX && uname[i]; i++) name[i] = uname[i];
            name[i] = 0;

            int fs_idx = fs_open(name);
            if (fs_idx < 0) { ret = -1; break; }

            struct task *t = task_current();
            int fd;
            for (fd = 3; fd < TASK_MAX_FDS; fd++) {
                if (t->fds[fd].kind == FD_FREE) break;
            }
            if (fd == TASK_MAX_FDS) { ret = -1; break; }

            t->fds[fd].kind   = FD_FS;
            t->fds[fd].fs_idx = fs_idx;
            t->fds[fd].offset = 0;
            ret = fd;
            break;
        }
        case SYS_READ: {
            int   fd  = (int)a;
            char *buf = (char *)(uintptr_t)b;
            int   n   = (int)c;

            struct task *t = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)        { ret = -1; break; }
            int kind = t->fds[fd].kind;
            if (kind == FD_FREE)                     { ret = -1; break; }

            if (kind == FD_STDIN) {
                ret = kshell_read_line(buf, n);
            } else if (kind == FD_FS) {
                int rd = fs_read(t->fds[fd].fs_idx,
                                 t->fds[fd].offset, buf, (uint32_t)n);
                if (rd > 0) t->fds[fd].offset += (uint32_t)rd;
                ret = rd;
            } else if (kind == FD_SOCK) {
                ret = sock_read(t->fds[fd].sock_idx, buf, n);
            } else {
                ret = -1;
            }
            break;
        }
        case SYS_WRITE_FD: {
            int         fd  = (int)a;
            const char *buf = (const char *)(uintptr_t)b;
            int         n   = (int)c;

            struct task *t = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)         { ret = -1; break; }
            int kind = t->fds[fd].kind;

            if (kind == FD_STDOUT) {
                for (int i = 0; i < n; i++) kputc(buf[i]);
                ret = n;
            } else if (kind == FD_SOCK) {
                ret = sock_write(t->fds[fd].sock_idx, buf, n);
            } else {
                ret = -1;
            }
            break;
        }
        case SYS_CLOSE: {
            int fd = (int)a;
            struct task *t = task_current();
            if (fd < 3 || fd >= TASK_MAX_FDS)         { ret = -1; break; }
            if (t->fds[fd].kind == FD_FREE)           { ret = -1; break; }
            if (t->fds[fd].kind == FD_SOCK) {
                sock_close(t->fds[fd].sock_idx);
            }
            t->fds[fd].kind     = FD_FREE;
            t->fds[fd].sock_idx = -1;
            ret = 0;
            break;
        }
        case SYS_SOCKET: {
            int sock_idx = sock_create();
            if (sock_idx < 0) { ret = -1; break; }

            struct task *t = task_current();
            int fd;
            for (fd = 3; fd < TASK_MAX_FDS; fd++) {
                if (t->fds[fd].kind == FD_FREE) break;
            }
            if (fd == TASK_MAX_FDS) { sock_close(sock_idx); ret = -1; break; }

            t->fds[fd].kind     = FD_SOCK;
            t->fds[fd].sock_idx = sock_idx;
            ret = fd;
            break;
        }
        case SYS_BIND: {
            int fd   = (int)a;
            int port = (int)b;
            struct task *t = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)         { ret = -1; break; }
            if (t->fds[fd].kind != FD_SOCK)           { ret = -1; break; }
            ret = sock_bind(t->fds[fd].sock_idx, (uint16_t)port);
            break;
        }
        case SYS_LISTEN: {
            int fd = (int)a;
            (void)b;                                 /* backlog ignored */
            struct task *t = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)         { ret = -1; break; }
            if (t->fds[fd].kind != FD_SOCK)           { ret = -1; break; }
            ret = sock_listen(t->fds[fd].sock_idx);
            break;
        }
        case SYS_ACCEPT: {
            int fd = (int)a;
            struct task *t = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)         { ret = -1; break; }
            if (t->fds[fd].kind != FD_SOCK)           { ret = -1; break; }

            int conn_sock = sock_accept(t->fds[fd].sock_idx);
            if (conn_sock < 0) { ret = -1; break; }

            int conn_fd;
            for (conn_fd = 3; conn_fd < TASK_MAX_FDS; conn_fd++) {
                if (t->fds[conn_fd].kind == FD_FREE) break;
            }
            if (conn_fd == TASK_MAX_FDS) {
                sock_close(conn_sock);
                ret = -1; break;
            }
            t->fds[conn_fd].kind     = FD_SOCK;
            t->fds[conn_fd].sock_idx = conn_sock;
            ret = conn_fd;
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
            int  *uout = (int *)(uintptr_t)a;
            int   exit_code = 0;
            int   pid = task_wait_current(uout ? &exit_code : NULL);
            if (pid > 0 && uout) *uout = exit_code;
            ret = pid;
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
}
