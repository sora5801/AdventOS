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
#include "serial.h"
#include "dns.h"
#include "mmap.h"
#include "bcache.h"
#include "vfs.h"
#include "procfs.h"
#include "lapic.h"
#include "smp.h"
#include "vbe.h"
#include "fbcon.h"
#include "wm.h"
#include "ac97.h"
#include "bkl.h"
#include "blkdev.h"

/* Session 107 — Path C. Tracks the task that currently owns the
 * framebuffer (set by SYS_FB_MAP, cleared by SYS_FB_UNMAP or by
 * task-exit cleanup). While non-NULL, fbcon mutes its text painting
 * so the owner can paint pixels without being trampled. Only one
 * task may hold the FB at a time. */
struct task *g_fb_owner;

/* Count currently-live fd slots — used both by the session-71 max_fds
 * cap check and (someday) by procfs renderers. Slots 0/1/2 are always
 * counted as live; slot 3+ count if their kind != FD_FREE. */
static int count_live_fds(struct task *t) {
    int n = 0;
    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
        if (t->fds[fd].kind != FD_FREE) n++;
    }
    return n;
}

/* Allocate the lowest free fd >= 3 in the calling task's table.
 * Returns the fd index or -1 if the table is full, OR if a session-71
 * max_fds limit is set and we'd exceed it. */
static int alloc_fd(struct task *t) {
    if (t->max_fds && (uint32_t)count_live_fds(t) >= t->max_fds) {
        return -1;
    }
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
    /* Session 82 followup: scan TASK_MAX slots, not the literal 16.
     * Same class of bug as signal_send / signal_send_pgrp — the
     * literal predates session 50's bump from 16 to 32 and was
     * silently broken until a task allocated at slot 16+ couldn't
     * be found by pgid/sid syscalls. */
    for (uint32_t i = 0; i < TASK_MAX; i++) {
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
        case FD_9P: {
            /* Release the per-fd 9p inode slot so future opens don't
             * exhaust the pool.  9p fids themselves are short-lived
             * (each read does walk + open + clunk inline). */
            extern void virtio_9p_fd_close(int);
            virtio_9p_fd_close(e->obj_idx);
            break;
        }
        default: break;     /* FD_FS / FD_STDIN / FD_STDOUT have no refcount */
    }
    e->kind    = FD_FREE;
    e->obj_idx = -1;
    e->offset  = 0;
    e->flags   = 0;          /* session 62 — clear FD_FL_NONBLOCK */
}

#define USER_STR_MAX 256

/* Session 70: syscall-number → short name. Used by procfs when
 * dumping the sandbox denial ring so an agent can grep the file
 * for "SYS_OPEN" instead of decoding "sc=10" itself.
 *
 * Compiled as a literal lookup table (gcc folds the switch under
 * -O2 into a constant table); cheap and stable across boots. */
const char *syscall_name(unsigned num) {
    switch (num) {
        case SYS_WRITE:           return "SYS_WRITE";
        case SYS_GETPID:          return "SYS_GETPID";
        case SYS_EXIT:            return "SYS_EXIT";
        case SYS_YIELD:           return "SYS_YIELD";
        case SYS_WRITE_STR:       return "SYS_WRITE_STR";
        case SYS_SLEEP_MS:        return "SYS_SLEEP_MS";
        case SYS_TIME:            return "SYS_TIME";
        case SYS_READ_LINE:       return "SYS_READ_LINE";
        case SYS_OPEN:            return "SYS_OPEN";
        case SYS_READ:            return "SYS_READ";
        case SYS_WRITE_FD:        return "SYS_WRITE_FD";
        case SYS_CLOSE:           return "SYS_CLOSE";
        case SYS_SOCKET:          return "SYS_SOCKET";
        case SYS_BIND:            return "SYS_BIND";
        case SYS_LISTEN:          return "SYS_LISTEN";
        case SYS_ACCEPT:          return "SYS_ACCEPT";
        case SYS_FORK:            return "SYS_FORK";
        case SYS_EXEC:            return "SYS_EXEC";
        case SYS_WAIT:            return "SYS_WAIT";
        case SYS_PIPE:            return "SYS_PIPE";
        case SYS_DUP2:            return "SYS_DUP2";
        case SYS_OPEN_W:          return "SYS_OPEN_W";
        case SYS_KILL:            return "SYS_KILL";
        case SYS_SIGACTION:       return "SYS_SIGACTION";
        case SYS_SIGRETURN:       return "SYS_SIGRETURN";
        case SYS_BRK:             return "SYS_BRK";
        case SYS_TTY_SET_MODE:    return "SYS_TTY_SET_MODE";
        case SYS_TTY_GET_MODE:    return "SYS_TTY_GET_MODE";
        case SYS_TTY_INJECT:      return "SYS_TTY_INJECT";
        case SYS_FS_WRITE:        return "SYS_FS_WRITE";
        case SYS_SETPGID:         return "SYS_SETPGID";
        case SYS_GETPGID:         return "SYS_GETPGID";
        case SYS_SETSID:          return "SYS_SETSID";
        case SYS_GETSID:          return "SYS_GETSID";
        case SYS_KILLPG:          return "SYS_KILLPG";
        case SYS_TCSETPGRP:       return "SYS_TCSETPGRP";
        case SYS_TCGETPGRP:       return "SYS_TCGETPGRP";
        case SYS_DNS_RESOLVE:     return "SYS_DNS_RESOLVE";
        case SYS_FS_FREE_SECTORS: return "SYS_FS_FREE_SECTORS";
        case SYS_MMAP:            return "SYS_MMAP";
        case SYS_MUNMAP:          return "SYS_MUNMAP";
        case SYS_MKDIR:           return "SYS_MKDIR";
        case SYS_CHDIR:           return "SYS_CHDIR";
        case SYS_GETCWD:          return "SYS_GETCWD";
        case SYS_READDIR:         return "SYS_READDIR";
        case SYS_BCACHE_SYNC:     return "SYS_BCACHE_SYNC";
        case SYS_BCACHE_STATS:    return "SYS_BCACHE_STATS";
        case SYS_CONNECT:         return "SYS_CONNECT";
        case SYS_WAIT_NB:         return "SYS_WAIT_NB";
        case SYS_GETCPU:          return "SYS_GETCPU";
        case SYS_FBINFO:          return "SYS_FBINFO";
        case SYS_SMP_STATS:       return "SYS_SMP_STATS";
        case SYS_AUDIO_PLAY:      return "SYS_AUDIO_PLAY";
        case SYS_BLOCK_INFO:      return "SYS_BLOCK_INFO";
        case SYS_BLOCK_READ:      return "SYS_BLOCK_READ";
        case SYS_BLOCK_WRITE:     return "SYS_BLOCK_WRITE";
        case SYS_TTY_CURSOR:      return "SYS_TTY_CURSOR";
        case SYS_TTY_CLEAR:       return "SYS_TTY_CLEAR";
        case SYS_TTY_CLEAR_EOL:   return "SYS_TTY_CLEAR_EOL";
        case SYS_GETUID:          return "SYS_GETUID";
        case SYS_GETGID:          return "SYS_GETGID";
        case SYS_SETUID:          return "SYS_SETUID";
        case SYS_SETGID:          return "SYS_SETGID";
        case SYS_FS_OWNER:        return "SYS_FS_OWNER";
        case SYS_FS_MODE:         return "SYS_FS_MODE";
        case SYS_FS_SIZE:         return "SYS_FS_SIZE";
        case SYS_CHMOD:           return "SYS_CHMOD";
        case SYS_CHOWN:           return "SYS_CHOWN";
        case SYS_OPENPTY:         return "SYS_OPENPTY";
        case SYS_KBD_POLL:        return "SYS_KBD_POLL";
        case SYS_PTRACE:          return "SYS_PTRACE";
        case SYS_NTP_SYNC:        return "SYS_NTP_SYNC";
        case SYS_DNS_CACHE_STATS: return "SYS_DNS_CACHE_STATS";
        case SYS_DHCP_INFO:       return "SYS_DHCP_INFO";
        case SYS_FD_NB:           return "SYS_FD_NB";
        case SYS_SERIAL_INJECT:   return "SYS_SERIAL_INJECT";
        case SYS_SANDBOX_INSTALL: return "SYS_SANDBOX_INSTALL";
        case SYS_SETLIMIT:        return "SYS_SETLIMIT";
        case SYS_UNLINK:          return "SYS_UNLINK";
        case SYS_RMDIR:           return "SYS_RMDIR";
        case SYS_TTY_GET_CURSOR:  return "SYS_TTY_GET_CURSOR";
        case SYS_OPEN_A:          return "SYS_OPEN_A";
        default:                  return "SYS_???";
    }
}

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

    /* Session 70: syscall sandbox enforcement.
     *
     * If a policy is active on this task, check the allow-bitmap
     * before dispatching. Out-of-range syscall numbers (>= 128) are
     * also denied — those are unknown to us, can't be allowed by any
     * policy, and falling through to the default-case unknown-syscall
     * log would let an attacker enumerate the policy by timing.
     *
     * Denied path: bump the per-task counter, log once per N denials
     * for visibility (full firehose would be too noisy), set ret=-1,
     * skip the switch. The user-mode caller sees -1 with no error
     * propagation — same shape as any other "unsupported" syscall. */
    struct task *t_sb = task_current();
    if (t_sb && t_sb->sandbox_active) {
        int allowed = 0;
        if (num < (uint32_t)(SANDBOX_MASK_WORDS * 32)) {
            uint32_t word = t_sb->sandbox_mask[num / 32];
            allowed = (word >> (num % 32)) & 1u;
        }
        if (!allowed) {
            t_sb->sandbox_denials++;

            /* Record into the per-task ring for /proc/<pid>/sandbox.
             * Packed: high 16 = low 16 of PIT ticks (~11 min wrap),
             * low 16 = syscall number. */
            uint32_t tick_low = pit_ticks() & 0xFFFFu;
            uint32_t entry    = (tick_low << 16) | (num & 0xFFFFu);
            uint8_t  idx      = t_sb->sandbox_recent_head;
            t_sb->sandbox_recent[idx] = entry;
            t_sb->sandbox_recent_head =
                (uint8_t)((idx + 1) % SANDBOX_RECENT_N);

            /* Log first denial + every 16th after, so a tight loop
             * doesn't drown the serial line. /proc/<pid>/sandbox has
             * the full ring for finer-grained inspection. */
            if (t_sb->sandbox_denials == 1 ||
                (t_sb->sandbox_denials & 0xF) == 0) {
                kprintf("[sandbox] pid=%u denied syscall %u "
                        "(total denied: %u)\n",
                        (unsigned)t_sb->id, (unsigned)num,
                        (unsigned)t_sb->sandbox_denials);
            }
            r->eax = (uint32_t)-1;
            bkl_unlock();
            return;
        }
    }

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
            /* Session 60: returns the NTP-corrected epoch.  Bare RTC
             * is the default base; ntp_sync may have nudged it via
             * rtc_apply_correction. */
            ret = (int32_t)rtc_epoch_corrected();
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
        case SYS_OPEN_A: {
            /* Append-mode tmpfs open. Same as SYS_OPEN_W except an
             * existing tmpfile keeps its bytes — tmpfs_write always
             * appends, so writes after this open land at end-of-file
             * (POSIX `>>` semantics). */
            const char *uname = (const char *)(uintptr_t)a;
            char name[FS_NAME_MAX + 1];
            int  i;
            for (i = 0; i < FS_NAME_MAX && uname[i]; i++) name[i] = uname[i];
            name[i] = 0;

            struct task *t = task_current();
            int fd = alloc_fd(t);
            if (fd < 0) { ret = -1; break; }

            int tmp_idx = tmpfs_create_append(name);
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
                case FD_9P: {
                    /* virtio-9p file. Each read translates to a 9P
                     * Twalk + Tlopen + Tread + Tclunk round-trip, so
                     * reads are O(round-trips-per-syscall) — fine for
                     * small files, slow for big ones.  The path is
                     * stored in g_inodes[obj_idx] in virtio_9p.c. */
                    extern int virtio_9p_fd_read(int, uint32_t, void *, uint32_t);
                    int rd = virtio_9p_fd_read(e->obj_idx, e->offset,
                                               buf, (uint32_t)n);
                    if (rd > 0) e->offset += (uint32_t)rd;
                    ret = rd;
                    break;
                }
                case FD_SOCK:
                    /* Session 62 — honor FD_FL_NONBLOCK on the fd
                     * before falling into sock_read's blocking yield
                     * loop. The peek (sock_read_avail) is cheap and
                     * lets the WM pump dozens of client fds per
                     * frame without ever stalling. */
                    if (e->flags & FD_FL_NONBLOCK) {
                        int av = sock_read_avail(e->obj_idx);
                        if (av != 1) { ret = -1; break; }
                    }
                    ret = sock_read(e->obj_idx, buf, n);
                    break;
                case FD_PIPE_R:
                    /* Session 74 — pipe non-blocking, same shape as
                     * the FD_SOCK branch above. agentd's event loop
                     * polls dozens of job stdout/stderr pipes per
                     * tick; a blocking read here would freeze the
                     * whole daemon waiting on one quiet child. */
                    if (e->flags & FD_FL_NONBLOCK) {
                        int av = pipe_read_avail(e->obj_idx);
                        if (av != 1) { ret = -1; break; }
                    }
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

            /* Session 62 — non-blocking accept: if FD_FL_NONBLOCK is
             * set, peek the backlog and return -1 instead of blocking
             * when no connection is queued. The accepted child fd
             * does NOT inherit the flag — clients shouldn't be forced
             * into nonblock mode just because the listener was. */
            if (t->fds[fd].flags & FD_FL_NONBLOCK) {
                int av = sock_accept_avail(t->fds[fd].obj_idx);
                if (av != 1) { ret = -1; break; }
            }
            int conn_sock = sock_accept(t->fds[fd].obj_idx);
            if (conn_sock < 0) { ret = -1; break; }

            int conn_fd = alloc_fd(t);
            if (conn_fd < 0) { sock_close(conn_sock); ret = -1; break; }

            t->fds[conn_fd].kind    = FD_SOCK;
            t->fds[conn_fd].obj_idx = conn_sock;
            t->fds[conn_fd].flags   = 0;
            ret = conn_fd;
            break;
        }
        case SYS_FD_NB: {
            int fd = (int)a;
            int on = (int)b;
            struct task *t = task_current();
            if (fd < 0 || fd >= TASK_MAX_FDS)        { ret = -1; break; }
            if (t->fds[fd].kind == FD_FREE)          { ret = -1; break; }
            if (on) t->fds[fd].flags |=  FD_FL_NONBLOCK;
            else    t->fds[fd].flags &= ~FD_FL_NONBLOCK;
            ret = 0;
            break;
        }
        case SYS_SERIAL_INJECT: {
            /* Session 67: drive the same translate+inject pipeline the
             * COM1 IRQ uses, without an actual UART read. Lets [t49]
             * confirm a "serial byte" reaches sys_read on fd 0. */
            const char *bytes = (const char *)(uintptr_t)a;
            int         n     = (int)b;
            if (!bytes || n < 0 || n > 256)          { ret = -1; break; }
            serial_inject_bytes(bytes, n);
            ret = n;
            break;
        }
        case SYS_SANDBOX_INSTALL: {
            /* Session 70: install a syscall allow-bitmap.
             *
             * On first call, the supplied mask becomes the active
             * policy. On subsequent calls, the new mask is AND-ed
             * with the current — policies are monotonic and can
             * only get tighter. Always returns 0 if the pointer
             * was readable; -1 if not.
             *
             * The enforcement check at the top of syscall_dispatch
             * already gated entry here. If the current policy denies
             * SYS_SANDBOX_INSTALL, we never reached this case; that
             * is the "frozen policy" semantics. */
            const uint32_t *user_mask = (const uint32_t *)(uintptr_t)a;
            if (!user_mask)                          { ret = -1; break; }

            struct task *t = task_current();
            for (int i = 0; i < SANDBOX_MASK_WORDS; i++) {
                uint32_t w = user_mask[i];
                if (t->sandbox_active) {
                    t->sandbox_mask[i] &= w;       /* tighten only */
                } else {
                    t->sandbox_mask[i] = w;        /* first install */
                }
            }
            t->sandbox_active = 1;
            ret = 0;
            break;
        }
        case SYS_SETLIMIT: {
            /* Session 71: tighten resource caps for this task.
             *
             * Zero in any field means "don't touch this cap". A
             * non-zero value is MIN()d into the existing cap so caps
             * can only tighten — symmetric with the sandbox API. The
             * struct also exposes max_wall_ms (relative); we add the
             * current PIT tick count to get an absolute deadline that
             * survives reschedules cleanly. */
            const struct sys_limits *l = (const struct sys_limits *)(uintptr_t)a;
            if (!l)                                  { ret = -1; break; }

            struct task *t = task_current();

            if (l->max_rss_kb) {
                uint32_t want = l->max_rss_kb / 4;          /* kB → 4-KiB pages */
                if (want == 0) want = 1;                     /* never round to "no limit" */
                if (t->max_rss_pages == 0 || want < t->max_rss_pages) {
                    t->max_rss_pages = want;
                }
            }
            if (l->max_cpu_ms) {
                uint32_t want = l->max_cpu_ms / 10;          /* ms → 10ms ticks */
                if (want == 0) want = 1;
                if (t->max_cpu_ticks == 0 || want < t->max_cpu_ticks) {
                    t->max_cpu_ticks = want;
                }
            }
            if (l->max_fds) {
                if (t->max_fds == 0 || l->max_fds < t->max_fds) {
                    t->max_fds = l->max_fds;
                }
            }
            if (l->max_wall_ms) {
                uint32_t delta = l->max_wall_ms / 10;
                if (delta == 0) delta = 1;
                uint32_t want_deadline = pit_ticks() + delta;
                if (t->wall_deadline_ticks == 0 ||
                    want_deadline < t->wall_deadline_ticks) {
                    t->wall_deadline_ticks = want_deadline;
                }
            }
            ret = 0;
            break;
        }
        case SYS_UNLINK: {
            /* Session 73: remove a regular file.  Copies the user path
             * into a kernel-local buffer (we're still on the user CR3
             * so the deref is fine for the actual content, but the
             * fs_unlink call walks deep into fs.c and could touch
             * arbitrary state — buffering is the defensive option). */
            const char *upath = (const char *)(uintptr_t)a;
            if (!upath) { ret = -1; break; }
            char path[128];
            int  i;
            for (i = 0; i < (int)sizeof(path) - 1 && upath[i]; i++) {
                path[i] = upath[i];
            }
            path[i] = 0;
            /* /mnt/9p paths route to the 9p driver's Tunlinkat. */
            if (path[0] == '/' && path[1] == 'm' && path[2] == 'n' &&
                path[3] == 't' && path[4] == '/' && path[5] == '9' &&
                path[6] == 'p' && (path[7] == '/' || path[7] == 0))
            {
                extern int virtio_9p_unlink_path(const char *, int);
                /* +1 to skip past "/mnt/9p"; if path is "/mnt/9p"
                 * itself (no trailing slash) we'd pass "" which the
                 * 9p side rejects in split_parent_basename. */
                const char *rel = path[7] == '/' ? path + 8 : path + 7;
                ret = virtio_9p_unlink_path(rel, /*is_dir=*/0);
                break;
            }
            ret = fs_unlink(path);
            break;
        }
        case SYS_RMDIR: {
            /* Session 83: remove an empty directory. Same path-copy
             * defense as SYS_UNLINK above. fs_rmdir checks emptiness
             * and permissions; returns -1 on any failure. */
            const char *upath = (const char *)(uintptr_t)a;
            if (!upath) { ret = -1; break; }
            char path[128];
            int  i;
            for (i = 0; i < (int)sizeof(path) - 1 && upath[i]; i++) {
                path[i] = upath[i];
            }
            path[i] = 0;
            if (path[0] == '/' && path[1] == 'm' && path[2] == 'n' &&
                path[3] == 't' && path[4] == '/' && path[5] == '9' &&
                path[6] == 'p' && (path[7] == '/' || path[7] == 0))
            {
                extern int virtio_9p_unlink_path(const char *, int);
                const char *rel = path[7] == '/' ? path + 8 : path + 7;
                ret = virtio_9p_unlink_path(rel, /*is_dir=*/1);
                break;
            }
            ret = fs_rmdir(path);
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
        /* Session 107 — Path C resurrects the FB syscalls for
         * userspace graphics. */
        case SYS_FB_INFO: {
            const struct vbe_state *v = vbe_state();
            struct sys_fb_info *out = (struct sys_fb_info *)(uintptr_t)a;
            if (!out) { ret = -1; break; }
            if (!v || !v->enabled) {
                out->enabled = 0;
                out->width = out->height = out->pitch = 0;
                out->bpp = out->fb_size = 0;
                ret = -1;
                break;
            }
            out->enabled = 1;
            out->width   = v->width;
            out->height  = v->height;
            out->pitch   = v->pitch;
            out->bpp     = v->bpp;
            out->fb_size = v->fb_size;
            ret = 0;
            break;
        }
        case SYS_FB_MAP: {
            struct task *t = task_current();
            const struct vbe_state *v = vbe_state();
            if (!v || !v->enabled) { ret = -1; break; }
            if (g_fb_owner && g_fb_owner != t) {
                /* Another task already owns the FB. */
                ret = -1;
                break;
            }
            uintptr_t user_va = (uintptr_t)a & ~0xFFFu;   /* page-align down */
            if (user_va == 0 || user_va >= 0xC0000000u) {
                /* Reject NULL and the kernel-half. */
                ret = -1;
                break;
            }
            uintptr_t fb_phys = v->fb_phys & ~0xFFFu;
            uint32_t  fb_end  = (v->fb_phys + v->fb_size + 0xFFFu) & ~0xFFFu;
            uint32_t  n_pages = (fb_end - fb_phys) / PAGE_SIZE;
            uint32_t *user_pd = (uint32_t *)(uintptr_t)t->cr3;
            int map_ok = 1;
            for (uint32_t i = 0; i < n_pages; i++) {
                if (paging_map_in(user_pd,
                                  user_va + (uintptr_t)i * PAGE_SIZE,
                                  fb_phys + (uintptr_t)i * PAGE_SIZE,
                                  PTE_USER | PTE_WRITABLE) != 0) {
                    /* Roll back is fiddly; the calling task will
                     * exit on failure and PD destruction frees the
                     * partial mappings. */
                    map_ok = 0;
                    break;
                }
            }
            if (!map_ok) { ret = -1; break; }
            g_fb_owner = t;
            fbcon_set_enabled(0);     /* mute text painting */
            ret = 0;
            break;
        }
        case SYS_FB_UNMAP: {
            struct task *t = task_current();
            if (g_fb_owner != t) { ret = -1; break; }
            g_fb_owner = 0;
            fbcon_set_enabled(1);
            /* Don't unmap the pages — the task is typically exiting
             * (or wants to keep them mapped for later). Pages are
             * freed when the PD is destroyed at task exit. */
            ret = 0;
            break;
        }
        case SYS_MOUSE_POLL: {
            /* Session 109 — Path C: snapshot the accumulated mouse
             * state. Drain the i8042 first so the snapshot is fresh
             * even if no other path has been polling. */
            extern void keyboard_poll_once(void);
            extern void mouse_get_state(int *, int *, int *);
            keyboard_poll_once();
            struct sys_mouse_state *out = (struct sys_mouse_state *)(uintptr_t)a;
            if (!out) { ret = -1; break; }
            int x, y, btns;
            mouse_get_state(&x, &y, &btns);
            out->x       = x;
            out->y       = y;
            out->buttons = (uint32_t)btns;
            ret = 0;
            break;
        }
        case SYS_KBD_POLL: {
            /* Non-blocking keyboard read. Returns the next byte from
             * the input ring, or 0 if empty. Bypasses the cooked/raw
             * TTY dance — useful for CLI programs that want edge-
             * triggered input without the canonical-mode line-edit
             * machinery. */
            extern int  keyboard_has_char(void);
            extern char keyboard_getc(void);
            if (keyboard_has_char()) {
                ret = (uint8_t)keyboard_getc();
            } else {
                ret = 0;
            }
            break;
        }
        /* Session 112 — WM client protocol. See kernel/wm.c for
         * the per-syscall semantics; the dispatcher is just a thin
         * passthrough. */
        case SYS_WM_BIND:
            ret = wm_bind(task_current());
            break;
        case SYS_WM_CREATE:
            ret = a ? wm_create_window(task_current(),
                       (struct sys_wm_create *)(uintptr_t)a) : -1;
            break;
        case SYS_WM_POLL:
            ret = a ? wm_pop_message(task_current(),
                       (struct sys_wm_msg *)(uintptr_t)a) : -1;
            break;
        case SYS_WM_DESTROY:
            ret = wm_destroy_window(task_current(), (uint32_t)a);
            break;
        /* Session 113 — input routing.  PUSH is wmd-only, POLL is
         * the client side. */
        case SYS_WM_EVENT_PUSH:
            ret = b ? wm_push_event(task_current(), (uint32_t)a,
                                    (const struct sys_wm_event *)(uintptr_t)b) : -1;
            break;
        case SYS_WM_EVENT_POLL:
            ret = b ? wm_poll_event(task_current(), (uint32_t)a,
                                    (struct sys_wm_event *)(uintptr_t)b) : -1;
            break;
        case SYS_GETRANDOM: {
            extern int virtio_rng_get(void *, int);
            extern int virtio_rng_available(void);
            void *ubuf = (void *)(uintptr_t)a;
            int   n    = (int)b;
            if (!ubuf || (uintptr_t)ubuf >= 0xC0000000u) { ret = -1; break; }
            if (n <= 0 || n > 4096) { ret = -1; break; }
            if (virtio_rng_available()) {
                ret = virtio_rng_get(ubuf, n);
                break;
            }
            /* Fallback: weak entropy from PIT ticks + RTC. Adequate
             * for non-crypto uses (random colors, ids, etc.); callers
             * that need real entropy should check `sys_getrandom_secure`
             * (= -1 when no virtio-rng). */
            uint8_t *p = (uint8_t *)ubuf;
            uint32_t s = pit_ticks() ^ (uint32_t)rtc_epoch_corrected();
            for (int i = 0; i < n; i++) {
                /* xorshift32 — fine for weak random. */
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                p[i] = (uint8_t)s;
            }
            ret = -1;        /* signal weak/no-device path */
            break;
        }
        case SYS_VIRTIO_CONSOLE_WRITE: {
            extern int virtio_console_write(const void *, int);
            const void *ubuf = (const void *)(uintptr_t)a;
            int n = (int)b;
            if (!ubuf || (uintptr_t)ubuf >= 0xC0000000u) { ret = -1; break; }
            if (n <= 0 || n > 4096) { ret = -1; break; }
            ret = virtio_console_write(ubuf, n);
            break;
        }
        case SYS_VIRTIO_CONSOLE_READ: {
            extern int virtio_console_read(void *, int);
            void *ubuf = (void *)(uintptr_t)a;
            int n = (int)b;
            if (!ubuf || (uintptr_t)ubuf >= 0xC0000000u) { ret = -1; break; }
            if (n <= 0 || n > 4096) { ret = -1; break; }
            ret = virtio_console_read(ubuf, n);
            break;
        }
        case SYS_VIRTIO_BALLOON_STATS: {
            extern int virtio_balloon_get_stats(uint32_t out[4]);
            uint32_t *uout = (uint32_t *)(uintptr_t)a;
            if (!uout || (uintptr_t)uout >= 0xC0000000u) { ret = -1; break; }
            ret = virtio_balloon_get_stats(uout);
            break;
        }
        case SYS_PTRACE: {
            /* Multiplexed ptrace dispatch (session 57). See the op
             * comments in syscall.h for what each one does; the heavy
             * lifting lives in kernel/ptrace.c. */
            extern int  ptrace_dispatch(int op, uint32_t pid, void *args);
            ret = ptrace_dispatch((int)a, b, (void *)(uintptr_t)c);
            break;
        }
        case SYS_NTP_SYNC: {
            /* Driver-side of session 60 SNTP. Caller hands us the
             * server IP as a 4-byte array; we query, parse, and
             * apply the delta to the kernel clock.  Returns the
             * server's reported Unix epoch on success, -1 on
             * timeout / bad reply. */
            extern int ntp_sync(const struct ip_addr *, uint32_t *);
            const uint8_t *ip_bytes = (const uint8_t *)(uintptr_t)a;
            if (!ip_bytes) { ret = -1; break; }
            struct ip_addr server;
            for (int i = 0; i < 4; i++) server.b[i] = ip_bytes[i];
            uint32_t server_epoch = 0;
            if (ntp_sync(&server, &server_epoch) != 0) { ret = -1; break; }
            /* Apply correction: delta = server - local current.
             * SYS_TIME from this point onward returns the disciplined
             * value. */
            uint32_t before = rtc_epoch_corrected();
            int32_t  delta  = (int32_t)((int64_t)server_epoch - (int64_t)before);
            rtc_apply_correction(delta);
            ret = (int32_t)server_epoch;
            break;
        }
        case SYS_NTP_TEST_RESPONDER: {
            extern void ntp_test_responder(int, uint32_t);
            ntp_test_responder((int)a, (uint32_t)b);
            ret = 0;
            break;
        }
        case SYS_DNS_CACHE_STATS: {
            extern void dns_cache_stats(uint32_t out[4]);
            uint32_t *out = (uint32_t *)(uintptr_t)a;
            if (out) dns_cache_stats(out);
            ret = 0;
            break;
        }
        case SYS_DHCP_INFO: {
            extern void dhcp_get_info(struct sys_dhcp_info *out);
            struct sys_dhcp_info *out = (struct sys_dhcp_info *)(uintptr_t)a;
            if (out) dhcp_get_info(out);
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
             * each string into a kmalloc'd buf. Cap at 128 args so
             * shell glob expansions (e.g. `ls *.elf` against a /
             * with 70+ binaries) survive without truncation. The
             * bookkeeping cost is one kmalloc per arg, capped by
             * the pointer-array stack size (128 * 4 = 512 bytes). */
            #define EXEC_MAX_ARGS 128
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
        case SYS_MOUSE_STATE:
            /* Retired with the mouse driver — see syscall.h. */
            ret = 0;
            break;
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
        case SYS_TTY_GET_CURSOR: {
            /* Session 84: read back the console cursor. Returns the
             * row/col of the framebuffer console (which tracks the
             * VGA cursor in lockstep — both setters fire together).
             * Used by sh.c's line editor to anchor the prompt row
             * before doing redraws on mid-line edits. */
            extern void fbcon_get_cursor(int *out_row, int *out_col);
            int *uout = (int *)(uintptr_t)a;
            if (!uout) { ret = -1; break; }
            int row = 0, col = 0;
            fbcon_get_cursor(&row, &col);
            uout[0] = row;
            uout[1] = col;
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
        case SYS_FS_SIZE: {
            /* Session 81: byte count of a regular file. Used by ls's
             * JSONL mode to populate the `size` field. Returns -1 for
             * directories and non-existent paths. fs_size already
             * exists in fs.c — we just expose it via syscall. */
            const char *path = (const char *)(uintptr_t)a;
            if (!path) { ret = -1; break; }
            int idx = fs_open(path);
            if (idx < 0) { ret = -1; break; }
            ret = (int)fs_size(idx);
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
        case SYS_FB_MMAP:
            /* Retired with the WM. Userspace doesn't get raw FB access
             * any more — the kernel still owns the framebuffer for
             * fbcon, but only as a text-rendering backend for
             * kprintf / TTY. */
            ret = 0;
            break;
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
                 * any allocation fails OR session-71 max_rss_pages
                 * would be exceeded, roll back the pages we managed
                 * to map and report failure (return unchanged break). */
                uint32_t mapped_to = cur_pg;
                int ok = 1;
                for (uint32_t va = cur_pg; va < new_pg; va += 4096) {
                    /* Session 71 RSS cap check before alloc. */
                    if (t->max_rss_pages &&
                        t->cur_rss_pages >= t->max_rss_pages) {
                        ok = 0;
                        break;
                    }
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
                    t->cur_rss_pages++;     /* session 71 RSS count */
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
