#include "syscall.h"
#include "kprintf.h"
#include "task.h"
#include "pit.h"
#include "rtc.h"
#include "shell.h"
#include "fs.h"
#include "sock.h"
#include "string.h"

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
            task_current()->state = TASK_STATE_DEAD;
            /* schedule() picks a non-DEAD task. We never return here:
             * the dead task's kernel stack still has our return frame
             * but nothing will ever pop it. */
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
        case SYS_KCMD: {
            /* Snapshot the user string into kernel memory because
             * kshell_run_line modifies it (NUL-terminates the verb). */
            const char *p = (const char *)(uintptr_t)a;
            char buf[256];
            int  i;
            for (i = 0; i < (int)sizeof(buf) - 1 && p[i]; i++) buf[i] = p[i];
            buf[i] = 0;
            kshell_run_line(buf);
            ret = 0;
            break;
        }
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
        default:
            kprintf("[unknown syscall %u from pid %u]\n",
                    (unsigned)num, (unsigned)task_current()->id);
            ret = -1;
            break;
    }

    /* Stash return value where iret will restore it as EAX. */
    r->eax = (uint32_t)ret;
}
