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
#define SYS_MOUSE_STATE  54 /* (eax=54, ebx=int32_t out[4]) -> 1 if mouse alive.
                              Fills out[]: x, y, buttons, packets_count.
                              Returns 0 if PS/2 mouse not detected. */
#define SYS_FB_MMAP      55 /* (eax=55) -> user VA mapped to the linear FB,
                              or 0 if VBE/fbcon disabled. The mapping is
                              RW user; munmap via SYS_MUNMAP(va, fb_size).
                              fb_size = pitch * height — read via SYS_FBINFO. */
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

void syscall_dispatch(struct registers *r);

#endif
