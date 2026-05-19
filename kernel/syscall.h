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
/* SYS_FB_TAKEOVER (72) and SYS_MOUSE_INJECT (74) were retired with
 * the original WM/mouse code. Session 107 (Path C) reclaimed both
 * slots — see the SYS_FB_INFO / SYS_FB_MAP defines further down. */
#define SYS_KBD_POLL      73 /* (eax=73) -> next ASCII key from the kbd
                                ring, or 0 if empty. Non-blocking — kept
                                because a CLI program can still want
                                edge-triggered keyboard polling. */

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

/* ---- Session 84 (path-A shell line-editor): cursor query ---- */
#define SYS_TTY_GET_CURSOR 87  /* (eax=87, ebx=int out[2]) -> 0.
                                * Writes (row, col) of the current
                                * console cursor into out[0], out[1].
                                * Pairs with SYS_TTY_CURSOR (= 60) which
                                * is the setter. Used by sh.c's line
                                * editor to anchor the prompt row before
                                * doing in-place redraws for left/right
                                * cursor movement and Ctrl-A/E/W/U/K. */

/* ---- Session 107: Path C — userspace framebuffer access ---- */

/* Replaces the retired GUI/WM slots 72 + 74. The owner-tracking and
 * fbcon-mute behavior is implemented in syscall.c::sys_fb_* and
 * fbcon.c::fbcon_is_muted. Only one task may hold the framebuffer at
 * a time; ownership is released automatically on task exit. */
#define SYS_FB_INFO       72   /* (eax=72, ebx=struct sys_fb_info *)
                                * fills the struct with framebuffer
                                * geometry. Returns 0 / -1 (FB disabled). */
#define SYS_FB_MAP        74   /* (eax=74, ebx=user_va) -> 0 / -1.
                                * Maps the FB into the calling task's
                                * address space at `user_va` and marks
                                * the task as FB owner. fbcon mutes
                                * its text painting while owned. */
#define SYS_FB_UNMAP      88   /* (eax=88) -> 0 / -1. Releases the
                                * FB so another task can take it (or
                                * so fbcon can resume painting).
                                * Auto-called on task exit. */

#define SYS_MOUSE_POLL    89   /* (eax=89, ebx=struct sys_mouse_state *)
                                * Reads the current accumulated mouse
                                * state. Returns 0 on success, -1 if
                                * the mouse driver isn't ready.
                                * Session 109. */

/* Session 112 — WM client protocol. The window manager daemon (wmd)
 * calls SYS_WM_BIND once to register as the compositor.  Clients
 * call SYS_WM_CREATE to allocate a shared-memory pixel surface;
 * those pages get mapped into BOTH the client and wmd at distinct
 * VAs, so the client paints and wmd reads zero-copy.  Wmd polls
 * SYS_WM_POLL each frame to drain "new window" and "destroyed
 * window" events. */
#define SYS_WM_BIND       90   /* (eax=90) -> 0 / -1 already-bound.   */
#define SYS_WM_CREATE     91   /* (eax=91, ebx=struct sys_wm_create*) */
#define SYS_WM_POLL       92   /* (eax=92, ebx=struct sys_wm_msg*)
                                * -> 1 if msg was returned, 0 if no
                                *    pending event, -1 if caller isn't
                                *    the bound WM. */
#define SYS_WM_DESTROY    93   /* (eax=93, ebx=window_id) -> 0/-1     */

/* Session 113 — input routing.  The WM determines which window the
 * mouse cursor / keyboard focus belongs to, then pushes events into
 * that window's per-slot event queue with SYS_WM_EVENT_PUSH.  The
 * client drains its own queue with SYS_WM_EVENT_POLL.  See
 * struct sys_wm_event below for the per-event payload. */
#define SYS_WM_EVENT_PUSH 94   /* (eax=94, ebx=window_id, ecx=struct sys_wm_event*)
                                * Wmd-only.  -> 0 on success, -1 if
                                * caller isn't the bound WM or window
                                * doesn't exist. */
#define SYS_WM_EVENT_POLL 95   /* (eax=95, ebx=window_id, ecx=struct sys_wm_event*)
                                * Client-only.  -> 1 if event returned,
                                * 0 if queue empty, -1 if window isn't
                                * owned by the calling task. */

#define SYS_GETRANDOM     96   /* (eax=96, ebx=buf, ecx=len) -> bytes
                                * read, or -1 if no virtio-rng device.
                                * On no-device, falls back to a PIT-tick
                                * pseudo-random (NOT cryptographically
                                * secure — caller should branch on
                                * sys_getrandom returning -1 if they need
                                * real entropy and refuse to proceed). */

#define SYS_VIRTIO_CONSOLE_WRITE  97 /* (eax=97, ebx=buf, ecx=n) -> bytes
                                      * sent, or -1 if no virtio-console.
                                      * Writes to the FIRST virtio-console
                                      * port (port 0 on the FIRST virtio-
                                      * serial controller). */

#define SYS_VIRTIO_CONSOLE_READ   98 /* (eax=98, ebx=buf, ecx=n) -> bytes
                                      * read (>=0), -1 if no device.
                                      * Non-blocking: returns 0 if no
                                      * bytes available right now. */

#define SYS_VIRTIO_BALLOON_STATS  99 /* (eax=99, ebx=uint32_t out[4]) ->
                                      * 0 on success, -1 if no balloon
                                      * device. out: [actual_pages,
                                      * target_pages, num_pages_freed,
                                      * num_pages_reclaimed]. */

/* Session 135 — Alt+Tab routing.  USB-HID posts on Alt+Tab via
 * the in-kernel `wm_post_alttab` helper.  Wmd polls via this
 * syscall once per frame.  Returns 1 if at least one Alt+Tab
 * press is pending (and decrements the counter), else 0. */
#define SYS_WM_POLL_ALTTAB       100

#define SYS_RENAME      101 /* (eax=101, ebx=old_path, ecx=new_path) ->
                             * 0 / -1.  For paths under /mnt/9p the
                             * driver issues an atomic 9P Trenameat;
                             * for AdventFS paths the kernel does NOT
                             * implement rename (caller falls back to
                             * the userspace copy+unlink in mv.c). */

/* Session 136 — global single-slot clipboard.  Any task can write
 * (last-writer-wins) and read.  Max payload 4096 bytes; kernel
 * kmalloc's a single buffer.  See clipboard.h. */
#define SYS_CLIPBOARD_SET  102  /* (eax=102, ebx=buf, ecx=len)  -> 0 / -1 */
#define SYS_CLIPBOARD_GET  103  /* (eax=103, ebx=buf, ecx=cap)  -> stored_len or 0 */

#define SYS_OPEN_A      104 /* (eax=104, ebx=name) -> tmpfs fd (preserves
                             * existing content) or -1. Drives shell `>>`
                             * append redirect. tmpfs_write always appends
                             * to end-of-file, so opening without truncate
                             * is enough to make `>>` work. Slots 100-103
                             * already taken (above). */

/* Session 143 — toast-notification channel.  Apps post short status
 * text ("saved /tmp/foo (123 B)") to a small kernel ring; wmd drains
 * one entry per frame and pops up a fading toast in the bottom-right.
 * Decoupled from SYS_WM_EVENT_POLL because the ring lives outside the
 * per-window event queues — any task can push without owning a window. */
#define SYS_WM_NOTIFY       105 /* (eax=105, ebx=text, ecx=len)  -> 0 / -1 */
#define SYS_WM_POLL_NOTIFY  106 /* (eax=106, ebx=buf, ecx=cap)   -> bytes or 0 */

/* Session 147 — workspace / virtual-desktop switch channel.  USB-HID
 * intercepts Alt+1..4 and posts here; wmd polls once per frame.
 * Poll returns 0..3 (workspace index) or -1 if no request pending. */
#define SYS_WM_POLL_WORKSPACE  107 /* (eax=107) -> 0..3 or -1 */

struct sys_fb_info {
    uint32_t  enabled;       /* 1 if a VBE framebuffer is available */
    uint32_t  width;         /* pixels */
    uint32_t  height;
    uint32_t  pitch;         /* bytes per scanline */
    uint32_t  bpp;           /* 16, 24, or 32 */
    uint32_t  fb_size;       /* total bytes (pitch * height) */
};

struct sys_mouse_state {
    int32_t   x;             /* absolute X within FB, clamped */
    int32_t   y;             /* absolute Y within FB, clamped */
    uint32_t  buttons;       /* bit 0 = left, bit 1 = right, bit 2 = middle */
};

/* Session 112 — WM client ABI.
 *
 *   SYS_WM_CREATE: caller writes (title, w, h) before the call; the
 *     kernel fills in `id` and `pixels_va`. The pixel surface is
 *     w*h*4 bytes of packed 0x00RRGGBB pixels (pitch = w*4). The
 *     kernel allocates pages out of pmm and maps them into the
 *     caller at `pixels_va`; the same physical pages are mapped
 *     into wmd's address space and delivered to wmd via the next
 *     SYS_WM_POLL.
 *
 *   SYS_WM_POLL: wmd-only. Drains one queued message.
 *     msg.op == 1 : new window. Fields are valid: id, owner_pid, w,
 *                   h, wmd_va (where wmd should read pixels), title.
 *     msg.op == 2 : destroyed. Only id and owner_pid are valid; wmd
 *                   should drop the window from its list.
 *
 *   SYS_WM_DESTROY: caller passes the window_id it owns. Pages are
 *     unmapped from both PDs and freed; a destroy message is queued
 *     for wmd. */
struct sys_wm_create {
    char        title[32];   /* in:  ASCII, NUL-terminated or truncated */
    uint32_t    w;           /* in:  surface width in pixels  */
    uint32_t    h;           /* in:  surface height in pixels */
    uint32_t    id;          /* out: window id (>= 1)         */
    uint32_t    pixels_va;   /* out: client-side surface VA   */
};

struct sys_wm_msg {
    uint32_t    op;          /* 1 = open, 2 = destroy */
    uint32_t    id;
    uint32_t    owner_pid;
    uint32_t    w;
    uint32_t    h;
    uint32_t    wmd_va;      /* read surface here (op=1 only)  */
    char        title[32];
};

/* Session 113 — per-event payload for SYS_WM_EVENT_PUSH/POLL.
 *
 *   type = 1 (WM_EV_MOUSE_MOVE)   x,y valid in client surface coords
 *   type = 2 (WM_EV_MOUSE_PRESS)  x,y valid; button bit set
 *   type = 3 (WM_EV_MOUSE_RELEASE) x,y valid; button bit set
 *   type = 4 (WM_EV_KEY)          keycode valid (raw ASCII or scancode)
 *   type = 5 (WM_EV_FOCUS)        no fields; the window just got focus
 *   type = 6 (WM_EV_UNFOCUS)      no fields; the window just lost focus
 *   type = 7 (WM_EV_CLOSE)        the WM is requesting the client to
 *                                 destroy this window (e.g. WM exit)
 *
 * Coordinates are translated by wmd to be local to the client
 * surface (origin at top-left of the surface, not the screen). */
#define WM_EV_MOUSE_MOVE      1u
#define WM_EV_MOUSE_PRESS     2u
#define WM_EV_MOUSE_RELEASE   3u
#define WM_EV_KEY             4u
/* Session 117 — FOCUS / UNFOCUS are now click-driven: they fire
 * when the window becomes (or stops being) the click-focused window
 * — the one that receives keyboard input.  Hover crossing events
 * use the new HOVER_ENTER / HOVER_LEAVE types below. */
#define WM_EV_FOCUS           5u
#define WM_EV_UNFOCUS         6u
#define WM_EV_CLOSE           7u
#define WM_EV_HOVER_ENTER     8u
#define WM_EV_HOVER_LEAVE     9u

#define WM_BUTTON_LEFT        0x01u
#define WM_BUTTON_RIGHT       0x02u
#define WM_BUTTON_MIDDLE      0x04u

struct sys_wm_event {
    uint32_t    type;
    int32_t     x;           /* mouse-event: client-surface-local */
    int32_t     y;
    uint32_t    button;      /* mouse press/release: bitmask */
    uint32_t    keycode;     /* key event: ASCII or scancode */
};


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
