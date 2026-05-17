#ifndef ADVENTOS_SYSCALL_H
#define ADVENTOS_SYSCALL_H

#include "../include/types.h"
#include "isr.h"

/* Syscall numbers — kept tiny for the demo. */
#define SYS_WRITE      1   /* (eax=1, ebx=ch)         -> writes one char        */
#define SYS_GETPID     2   /* (eax=2)                 -> pid in eax             */
#define SYS_EXIT       3   /* (eax=3, ebx=code)       -> never returns          */
#define SYS_YIELD      4   /* (eax=4)                 -> calls schedule()       */
#define SYS_WRITE_STR  5   /* (eax=5, ebx=ptr)        -> writes a NUL-term str  */
#define SYS_SLEEP_MS   6   /* (eax=6, ebx=ms)         -> sleeps + reschedules   */
#define SYS_TIME       7   /* (eax=7)                 -> UNIX epoch in eax      */
#define SYS_READ_LINE  8   /* (eax=8, ebx=buf, ecx=n) -> read a line, len in eax*/
/* SYS_KCMD = 9 retired in session 14 — userspace shell uses fork/exec/wait now. */
#define SYS_OPEN       10  /* (eax=10, ebx=name)         -> fd or -1            */
#define SYS_READ       11  /* (eax=11, ebx=fd, ecx=buf, edx=n) -> bytes or 0/-1 */
#define SYS_WRITE_FD   12  /* (eax=12, ebx=fd, ecx=buf, edx=n) -> bytes or -1   */
#define SYS_CLOSE      13  /* (eax=13, ebx=fd)           -> 0 or -1             */
#define SYS_SOCKET     14  /* (eax=14)                   -> fd or -1            */
#define SYS_BIND       15  /* (eax=15, ebx=fd, ecx=port) -> 0 or -1             */
#define SYS_LISTEN     16  /* (eax=16, ebx=fd, ecx=back) -> 0 or -1             */
#define SYS_ACCEPT     17  /* (eax=17, ebx=fd)           -> conn fd or -1       */
#define SYS_FORK       18  /* (eax=18)                   -> child pid (parent), 0 (child), -1 err */
#define SYS_EXEC       19  /* (eax=19, ebx=path, ecx=argv) -> -1 on err, no return on success */
#define SYS_WAIT       20  /* (eax=20, ebx=&exit_code)   -> child pid or -1 if no children */
#define SYS_PIPE       21  /* (eax=21, ebx=&fds[2])      -> 0 / -1; fds[0]=read end fds[1]=write end */
#define SYS_DUP2       22  /* (eax=22, ebx=oldfd, ecx=newfd) -> newfd or -1 */
#define SYS_OPEN_W     23  /* (eax=23, ebx=name)         -> tmpfs fd (truncated) or -1 */
#define SYS_KILL       24  /* (eax=24, ebx=pid, ecx=sig) -> 0 or -1 */
#define SYS_SIGACTION  25  /* (eax=25, ebx=sig, ecx=handler, edx=tramp) -> previous handler */
#define SYS_SIGRETURN  26  /* (eax=26)                   -> never returns to caller; restores ctx */
#define SYS_BRK        27  /* (eax=27, ebx=new_brk_or_0) -> current/new heap_brk; 0 means "query" */
#define SYS_TTY_SET_MODE 28 /* (eax=28, ebx=flags)        -> previous flags */
#define SYS_TTY_GET_MODE 29 /* (eax=29)                   -> current flags */
#define SYS_TTY_INJECT   30 /* (eax=30, ebx=ptr, ecx=n)   -> bytes pushed (test helper) */
#define SYS_FS_WRITE     31 /* (eax=31, ebx=name, ecx=buf, edx=n) -> 0 / -1; persists to disk */
#define SYS_SETPGID      32 /* (eax=32, ebx=pid, ecx=pgid) -> 0 / -1; pid=0 = self, pgid=0 = pid */
#define SYS_GETPGID      33 /* (eax=33, ebx=pid)           -> pgid or -1; pid=0 = self */
#define SYS_SETSID       34 /* (eax=34)                    -> new sid (= pid) or -1 */
#define SYS_GETSID       35 /* (eax=35, ebx=pid)           -> sid or -1; pid=0 = self */
#define SYS_KILLPG       36 /* (eax=36, ebx=pgid, ecx=sig) -> 0 / -1 */
#define SYS_TCSETPGRP    37 /* (eax=37, ebx=fd, ecx=pgid)  -> 0 / -1; sets the foreground pgrp */
#define SYS_TCGETPGRP    38 /* (eax=38, ebx=fd)            -> current foreground pgrp */
#define SYS_DNS_RESOLVE  39 /* (eax=39, ebx=name, ecx=ip[4]) -> 0 / -1; blocks until DNS reply or timeout */
#define SYS_FS_FREE_SECTORS 40 /* (eax=40)                  -> count of unallocated FS sectors */
#define SYS_MMAP         41 /* (eax=41, ebx=fd, ecx=offset, edx=length) -> user VA or 0  */
#define SYS_MUNMAP       42 /* (eax=42, ebx=addr, ecx=length)            -> 0 or -1     */
#define SYS_MKDIR        43 /* (eax=43, ebx=path)                        -> 0 or -1     */
#define SYS_CHDIR        44 /* (eax=44, ebx=path)                        -> 0 or -1     */
#define SYS_GETCWD       45 /* (eax=45, ebx=buf, ecx=cap)                -> bytes or -1 */
#define SYS_READDIR      46 /* (eax=46, ebx=dir_path, ecx=&iter, edx=name_buf16) -> idx or -1 */
#define SYS_BCACHE_SYNC  47 /* (eax=47)                                  -> blocks flushed */
#define SYS_BCACHE_STATS 48 /* (eax=48, ebx=uint32_t out[5])             -> 0 or -1     */
#define SYS_CONNECT      49 /* (eax=49, ebx=fd, ecx=ip[4], edx=port)     -> 0 or -1     */
#define SYS_WAIT_NB      50 /* (eax=50, ebx=&exit_code) -> pid or 0 (no zombie ready) or -1 (no children) */
#define SYS_GETCPU       51 /* (eax=51) -> LAPIC ID of the CPU handling this syscall (= the running CPU) */
#define SYS_FBINFO       52 /* (eax=52, ebx=uint32_t out[4]) -> 1 if fbcon enabled, 0 otherwise.
                              When enabled, fills out[]: width, height, bpp, pitch. */
#define SYS_SMP_STATS    53 /* (eax=53, ebx=uint32_t out[8]) -> N CPUs.
                              Fills out[i] with LAPIC-timer tick count on CPU i.
                              Lets [t22] verify APs are actually scheduling. */
/* SYS_MOUSE_STATE (54) and SYS_FB_MMAP (55) — retired; the WM and
 * mouse driver were removed when AdventOS narrowed its target audience
 * to developers and AI agents who only need a CLI. The slot numbers
 * stay allocated for ABI stability but the dispatcher returns -1. */
#define SYS_MOUSE_STATE  54
#define SYS_FB_MMAP      55
#define SYS_AUDIO_PLAY   56 /* (eax=56, ebx=ptr, ecx=n_bytes) -> bytes accepted,
                              or -1 if no AC97 codec. n_bytes must be a
                              multiple of 4 (one stereo 16-bit sample). PCM
                              format: 16-bit signed little-endian stereo
                              at 48 kHz. Buffer is COPIED into kernel
                              staging — caller can reuse on return. */

#define SYS_BLOCK_INFO   57 /* (eax=57, ebx=dev_idx, ecx=struct sys_block_info *)
                              -> 0 on success, -1 if dev_idx out of range.
                              Fills *out with block_size + n_blocks + name. */
#define SYS_BLOCK_READ   58 /* (eax=58, ebx=struct sys_block_args *) -> 0 / -1.
                              Reads n_blocks blocks starting at lba into buf. */
#define SYS_BLOCK_WRITE  59 /* (eax=59, ebx=struct sys_block_args *) -> 0 / -1.
                              Writes n_blocks blocks from buf to lba. */

#define SYS_TTY_CURSOR    60 /* (eax=60, ebx=row, ecx=col) -> 0. Places the
                                console cursor at (row, col) in both the VGA
                                text grid and (if active) the framebuffer
                                console. Subsequent puts continue from there. */
#define SYS_TTY_CLEAR     61 /* (eax=61) -> 0. Clears the whole console + homes
                                the cursor. */
#define SYS_TTY_CLEAR_EOL 62 /* (eax=62) -> 0. Clears from cursor to end of
                                line. Cursor position unchanged. */

#define SYS_GETUID        63 /* (eax=63) -> current task's uid (0 = root). */
#define SYS_GETGID        64 /* (eax=64) -> current task's gid. */
#define SYS_SETUID        65 /* (eax=65, ebx=uid) -> 0 on success, -1 if not
                                privileged. uid 0 (root) may set any uid;
                                others may only setuid to their own uid (no-op). */
#define SYS_SETGID        66 /* (eax=66, ebx=gid) -> 0 / -1. Same rules. */

#define SYS_FS_OWNER      67 /* (eax=67, ebx=path) -> (uid << 16) | gid for the
                                file at `path`, or -1 if it doesn't exist. */
#define SYS_FS_MODE       68 /* (eax=68, ebx=path) -> low 9 bits = rwxrwxrwx,
                                or -1 if the file doesn't exist. */
#define SYS_CHMOD         69 /* (eax=69, ebx=path, ecx=mode) -> 0 / -1.
                                Only the file's owner or root may chmod. */
#define SYS_CHOWN         70 /* (eax=70, ebx=path, ecx=uid, edx=gid) -> 0 / -1.
                                Only root may chown. */
#define SYS_OPENPTY       71 /* (eax=71, ebx=int fds[2]) -> 0 / -1.
                                fds[0] = master, fds[1] = slave. Refs both
                                start at 1 (one fd per end). Session 52. */

/* ---- Session 57: GUI + window manager support ---- */

/* SYS_FB_TAKEOVER (72) — retired with the WM. Stays as a no-op slot. */
#define SYS_FB_TAKEOVER   72
#define SYS_KBD_POLL      73 /* (eax=73) -> next ASCII key from the kbd
                                ring, or 0 if empty. Non-blocking — kept
                                because a CLI program can still want
                                edge-triggered keyboard polling. */
/* SYS_MOUSE_INJECT (74) — retired with the mouse driver. */
#define SYS_MOUSE_INJECT  74

/* ---- Session 57: ptrace-based debugger support ---- */

#define SYS_PTRACE        75 /* (eax=75, ebx=op, ecx=pid, edx=&struct
                                ptrace_args) -> op-specific return.
                                The operation set + arg encoding lives in
                                struct ptrace_args below. Single multiplexed
                                syscall to avoid burning seven slots. */

/* ---- Session 60: SNTP + DNS cache + DHCP introspection ---- */

#define SYS_NTP_SYNC      76 /* (eax=76, ebx=ip[4]) -> Unix epoch reported
                                by the NTP server, or -1 on timeout / bad
                                reply.  Also calls rtc_apply_correction
                                so subsequent SYS_TIME returns the
                                disciplined time. */

#define SYS_NTP_TEST_RESPONDER 77 /* (eax=77, ebx=on, ecx=epoch) -> 0.
                                Test-only — register a kernel-side UDP-123
                                responder that replies with `epoch` as the
                                Transmit Timestamp.  Lets [t43] do a pure-
                                loopback NTP round-trip without needing a
                                public-internet server. on=0 unregisters. */

#define SYS_DNS_CACHE_STATS 78 /* (eax=78, ebx=uint32 out[4]) -> 0.
                                Fills out[]: lookups, hits, misses,
                                live entries.  Used by [t43] to verify
                                the TTL-cache from session 60. */

#define SYS_DHCP_INFO     79 /* (eax=79, ebx=struct sys_dhcp_info *) -> 0.
                                Snapshot of the DHCP lease for
                                introspection: assigned IP, gateway,
                                netmask, dns server, lease length, time
                                acquired. */

/* ---- Session 62: out-of-process apps over IPC ---- */

#define SYS_FD_NB         80 /* (eax=80, ebx=fd, ecx=on) -> 0 / -1.
                                When on=1, sets FD_FL_NONBLOCK on the
                                given fd in the task's fd table. SYS_ACCEPT
                                / SYS_READ then return -1 immediately on
                                empty instead of blocking. Lets the WM
                                pump many client connections from a single
                                60-fps event loop. */

/* ---- Session 67: serial keyboard input ---- */

#define SYS_SERIAL_INJECT 81 /* (eax=81, ebx=const char *bytes, ecx=int n)
                              * -> n on success, -1 on bad pointer / n<0.
                              * Pushes `n` bytes through the same
                              * translate+keyboard_inject pipeline that
                              * the COM1 RX IRQ uses, so the [t49] selftest
                              * can exercise the serial-input path without
                              * needing a host-side terminal feeding bytes
                              * into the UART. Pure-side-channel — no real
                              * UART activity. */

/* ---- Session 70: syscall sandbox ----
 *
 * Per-task syscall allow-list. SYS_SANDBOX_INSTALL takes a 4-word
 * bitmap; bit `i` controls syscall number `i`. A set bit means the
 * syscall is permitted. Once a policy is installed, the new mask
 * is AND-ed with the current one — policies are sticky and can
 * only get tighter. The mask is inherited verbatim across fork
 * and preserved across exec; there is no kernel-side mechanism to
 * loosen a sandbox once installed.
 *
 * SYS_SANDBOX_INSTALL is itself gated by the current mask. A
 * sensible policy should keep its bit set so the task can ratchet
 * further; clearing it freezes the policy permanently. SYS_EXIT,
 * SYS_WRITE, SYS_WRITE_STR, and SYS_GETPID should also stay set in
 * most policies — the rest is application-dependent.
 *
 * Denied syscalls return -1 with no side effect, and bump the
 * task's `sandbox_denials` counter (see procfs / SYS_SANDBOX_INFO
 * — TODO). A future enhancement may add an audit ring buffer with
 * pid + syscall_no + epoch entries for forensic analysis.
 */
#define SYS_SANDBOX_INSTALL 82  /* (eax=82, ebx=const uint32_t mask[4])
                                 * -> 0 on success, -1 on bad pointer.
                                 * Mask is AND-ed with current policy. */

/* Bitmap layout: 4 32-bit words = 128 syscall slots. We have ~82 in
 * use today (highest is SYS_SANDBOX_INSTALL = 82), leaves headroom. */
#define SANDBOX_MASK_WORDS  4

/* ---- Session 71: per-task resource limits ----
 *
 * Companion to the syscall sandbox. The sandbox stops a task from
 * talking to the system; the limits stop it from consuming it. A
 * compute-policy tool that mallocs(1 << 30) or spins forever needs
 * a kernel-side cap, not just an allow-list.
 *
 * Each task carries four caps (RSS, CPU time, fd count, wall-clock
 * deadline) and matching usage counters. Caps are zero by default
 * (no limit). SYS_SETLIMIT tightens — zero in a request field means
 * "leave this cap alone", non-zero MIN()s into the current value.
 * That makes it ergonomic to set only the cap you care about.
 *
 * Enforcement sites:
 *   - max_rss_pages : pmm_alloc in SYS_BRK + mmap_handle_fault
 *   - max_cpu_ticks : PIT IRQ accumulates cur_cpu_ticks, posts
 *                     SIGKILL when cur >= cap
 *   - max_fds       : alloc_fd refuses past N live entries
 *   - wall_deadline : PIT IRQ checks pit_ticks() >= deadline, SIGKILL
 *
 * Overrun on the IRQ side flags pending SIGKILL; the existing
 * signal_check_and_deliver tail of irq_handler delivers it on the
 * iret-to-ring-3 path. */
#define SYS_SETLIMIT 83        /* (eax=83, ebx=const struct sys_limits *)
                                * -> 0 on success, -1 on bad pointer. */

struct sys_limits {
    uint32_t max_rss_kb;       /* kernel converts to pages internally */
    uint32_t max_cpu_ms;       /* kernel converts to PIT ticks (÷ 10) */
    uint32_t max_fds;
    uint32_t max_wall_ms;      /* relative; kernel adds pit_ticks() */
};

/* ---- Session 73: filesystem unlink ---- */
#define SYS_UNLINK        84   /* (eax=84, ebx=const char *path) -> 0 / -1
                                * Removes a regular file. Refuses
                                * directories. Honors session-47
                                * permission checks (owner or root).
                                * Refuses to unlink a file that any
                                * task currently has open. */

/* ---- Session 81: filesystem entry size for structured-pipeline ls ---- */
#define SYS_FS_SIZE       85   /* (eax=85, ebx=const char *path) -> size in
                                * bytes, or -1 if the file doesn't exist /
                                * isn't a regular file. Used by ls's JSONL
                                * mode to emit a `size` field per entry. */

/* ---- Session 83 (path-A coreutils): rmdir for empty directories ---- */
#define SYS_RMDIR         86   /* (eax=86, ebx=const char *path) -> 0 / -1.
                                * Mirrors SYS_UNLINK but for FS_TYPE_DIR
                                * entries; refuses non-empty dirs (any
                                * child entry with parent_dir == this idx
                                * blocks removal — POSIX ENOTEMPTY analog).
                                * Same owner-or-root permission model. */


/* User/kernel ABI for the SYS_BLOCK_* calls. */
struct sys_block_info {
    uint32_t block_size;
    uint32_t n_blocks;
    char     name[16];
};
struct sys_block_args {
    uint32_t dev_idx;
    uint32_t lba;
    uint32_t n_blocks;
    void    *buf;
};

/* ---- ptrace ABI (session 57) ----
 *
 * The classic POSIX ptrace is a single syscall multiplexed by `op`. We
 * follow the same shape so the userspace debugger reads naturally.
 *
 * For ops that need register state or a byte buffer, the third arg
 * is a pointer to struct ptrace_args below — its `data` field carries
 * either a `struct ptrace_regs *` or a `(void *, size_t)` buffer pair
 * depending on op. `addr` is the target VA in the tracee's address
 * space (for PEEK/POKE) or 0 (otherwise).
 *
 * Op semantics (kernel side):
 *   - TRACEME:   the calling task marks itself traced by its parent.
 *                The very next SIGTRAP-class event (entry to a
 *                future exec'd binary, or an INT3) will stop the
 *                task and notify the tracer via SIGCHLD.
 *   - ATTACH:    parent declares "I am the tracer of pid". The tracee
 *                is sent SIGSTOP so the tracer can inspect from a
 *                quiesced state.
 *   - DETACH:    clear the trace relationship; if stopped, continue.
 *   - PEEKDATA:  read `size` bytes from tracee's VA `addr` into the
 *                tracer's `buf`. Honors the tracee's user PD — i.e.
 *                we walk the page table and copy from the right CR3.
 *   - POKEDATA:  symmetric write; used by the debugger to plant 0xCC
 *                breakpoints and to write back the original byte on
 *                continue.
 *   - GETREGS:   copy the tracee's saved ring-3 register frame into
 *                tracer's `regs`. Only valid when tracee is STOPPED.
 *   - SETREGS:   write tracer's `regs` back into the tracee's saved
 *                frame. The next iret-to-ring3 picks up the new EIP/
 *                ESP/EFLAGS, so this is how we "rewind" EIP after an
 *                INT3 hit (the trap moves EIP one past the 0xCC byte;
 *                the debugger has to put it back at the breakpoint
 *                address so the original instruction re-executes).
 *   - CONT:      resume the tracee. Same as SIGCONT but doesn't queue
 *                a delivery — just flips state back to READY.
 *   - STEP:      set EFLAGS.TF=1 in the saved frame and CONT. After
 *                one instruction the CPU raises #DB (vector 1) and
 *                we stop again. */

#define PTRACE_TRACEME    0
#define PTRACE_ATTACH     1
#define PTRACE_DETACH     2
#define PTRACE_PEEKDATA   3
#define PTRACE_POKEDATA   4
#define PTRACE_GETREGS    5
#define PTRACE_SETREGS    6
#define PTRACE_CONT       7
#define PTRACE_STEP       8
/* PTRACE_WAIT: block until the tracee changes state. Returns:
 *   >0  : tracee stopped — value is the trap signal (e.g. SIGTRAP=5)
 *   0   : tracee exited normally — caller should sys_wait to reap
 *   -1  : pid isn't traced by us
 * Implemented as a polling loop on the tracee's traced_stopped /
 * state flags, with sys_sleep_ms-style yields between checks. */
#define PTRACE_WAIT       9

/* Register snapshot the tracer reads/writes. Layout MUST match the
 * subset of `struct registers` (kernel/isr.h) that we expose — the
 * extra kernel-only fields (segment selectors, error codes) are
 * not surfaced because user-mode debuggers don't need them. */
struct ptrace_regs {
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp;
    uint32_t eip, esp;
    uint32_t eflags;
};

struct ptrace_args {
    uint32_t       addr;       /* tracee VA for PEEK/POKE; ignored otherwise */
    uint32_t       size;       /* PEEK/POKE byte count */
    void          *buf;        /* PEEK/POKE caller buffer (in tracer's VA) */
    struct ptrace_regs *regs;  /* GETREGS / SETREGS */
};

/* SYS_DHCP_INFO output (session 60). */
struct sys_dhcp_info {
    uint8_t  ip[4];            /* our assigned IP */
    uint8_t  netmask[4];
    uint8_t  gateway[4];
    uint8_t  dns_server[4];
    uint32_t lease_seconds;    /* lease length the server granted */
    uint32_t acquired_epoch;   /* sys_time() at DHCP ACK */
    uint32_t t1_renew_at;      /* sys_time() value at which we'll renew (= acquired + lease/2) */
    int      have_lease;       /* 1 if everything above is meaningful */
};

void syscall_dispatch(struct registers *r);

/* Session 70: short name for a syscall number ("SYS_OPEN", "SYS_WRITE",
 * etc.), or "SYS_???" if the number is unknown. Used by procfs to
 * render the sandbox denial ring buffer. Returns a stable const
 * string — no caller-side free. */
const char *syscall_name(unsigned num);

#endif
