# Session 12 — argv, per-process file descriptors, real `cat` and `echo`

**Goal:** Make user programs feel like real Unix programs. Pass `argc`/`argv` to `main` through the standard SysV i386 process-startup convention. Give every task a per-process fd table with `0/1/2 = stdin/stdout/stderr`. Add `SYS_OPEN/READ/WRITE/CLOSE`. Ship `cat` and `echo` written in plain C against `libuser`.

End state of the milestone:

```
advent$ exec echo.elf hello world from ring 3
exec: pid=5  argc=6  entry=0x40000000  esp=0x40100fbc  (echo.elf)
hello world from ring 3
[user task pid=5 exited code=0]

advent$ exec cat.elf hello.txt
exec: pid=6  argc=2  entry=0x40000000  esp=0x40100fdc  (cat.elf)
Hello from a text file on AdventFS.
This was read by cat.elf via SYS_OPEN, SYS_READ, and SYS_CLOSE,
and written to stdout by SYS_WRITE_FD with fd=1.
Each call is a real ring 3 -> ring 0 -> ring 3 round trip through
the IDT[0x80] gate the kernel installed back in session 5.

advent$ exec cat.elf nope.txt
cat: nope.txt: cannot open
```

`echo` and `cat` are real C programs. They read their `argv`, open files via `SYS_OPEN`, walk a real fd table, and round-trip every read/write through the IDT[0x80] gate.

## What's in scope

In:
- argv setup on the user stack (SysV i386 process-startup layout)
- Per-process fd table in `struct task` (8 slots, 0/1/2 pre-wired)
- `SYS_OPEN`, `SYS_READ`, `SYS_WRITE_FD`, `SYS_CLOSE`
- libuser wrappers `sys_open` / `sys_read` / `sys_write` / `sys_close`
- `user/cat.c`, `user/echo.c` — C, no inline asm
- `fs/hello.txt` test data; mkfs.py support for non-ELF files

Out:
- `dup`/`dup2`/`pipe`
- File creation or write-back (FS is read-only)
- `lseek`
- `stat`/`fstat`
- `ioctl`
- Multiplexed I/O (`select`, `poll`)
- `argv[0]`-as-pathname-resolution (we just use `argv[0]` as-is)
- `envp` (would be a parallel pointer array right after argv)
- A real Unix-flavored standard library (we reimplement what we need)

## SysV i386 process startup, by the book

When the kernel `iret`s into a fresh user program, the i386 SysV ABI specifies what's at the top of the user stack:

```
high addr ──┬─────────────────────────────────────┐
            │ argv strings, NUL-terminated         │
            │ <align>                              │
            │ NULL                                 │  argv[argc] terminator
            │ argv[argc-1]      (pointer)          │
            │ ...                                  │
            │ argv[0]           (pointer)          │
ESP ──────► │ argc                                 │
low addr  ──┴─────────────────────────────────────┘
```

`_start` reads `argc` from `[esp]`, computes `argv = esp + 4`, and tail-calls into `main(argc, argv)` per the cdecl C convention. All inside ring 3, no syscalls — the layout is just bytes the kernel placed before `iret`.

Updated `user/start.S`:

```asm
_start:
    movl    (%esp), %eax            /* argc            */
    leal    4(%esp), %ebx           /* argv (= &esp[1]) */
    pushl   %ebx                    /* arg 2: argv     */
    pushl   %eax                    /* arg 1: argc     */
    call    _main
    pushl   %eax                    /* main's return val */
    call    _sys_exit
```

Backward-compat with old `int main(void)` programs: they ignore the args, the stack space they sit on doesn't get touched.

## elf_setup_args — the kernel side

[`kernel/elf.c`](../kernel/elf.c) gains a public `elf_setup_args(r, argc, argv)`. Caller has already run `elf_load`; `r` knows the user PD's CR3 and — crucially — the **physical address** of the freshly-allocated user stack page. The function packs argv onto that page using the kernel's identity map:

```c
void elf_setup_args(struct elf_load_result *r,
                    int argc, const char *const *argv) {
    uint8_t *kbase   = (uint8_t *)(uintptr_t)r->stack_phys;
    uint32_t cur_off = r->stack_size;
    uint32_t cur_va  = USER_STACK_VA + r->stack_size;
    uint32_t str_va[16];

    /* 1. strings, in reverse */
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        cur_off -= len; cur_va -= len;
        memcpy(kbase + cur_off, argv[i], len);
        str_va[i] = cur_va;
    }
    /* 2. align */
    cur_off &= ~3u; cur_va &= ~3u;
    /* 3. NULL */
    cur_off -= 4; cur_va -= 4;
    *(uint32_t *)(kbase + cur_off) = 0;
    /* 4. argv pointers */
    for (int i = argc - 1; i >= 0; i--) {
        cur_off -= 4; cur_va -= 4;
        *(uint32_t *)(kbase + cur_off) = str_va[i];
    }
    /* 5. argc */
    cur_off -= 4; cur_va -= 4;
    *(uint32_t *)(kbase + cur_off) = (uint32_t)argc;

    r->user_esp = cur_va;
}
```

The dual-pointer dance (`cur_off` to write through the kernel's identity map of the physical page; `cur_va` to capture the user-virtual address that `argv[i]` and ESP will hold once the user is running) is the whole "argv pointers reference string addresses in user VA space" idea condensed into one loop.

## The bug that bit immediately

First version: I added the new `_start` and called `elf_setup_args` from `cmd_exec` only. Boot crashed at `[boot] launched userspace shell (sh.elf) as pid 4`:

```
[!] CPU EXCEPTION 14: Page fault (err=0x4) at 1b:40000000  eflags=0x10202
    fault addr (CR2) = 0x40101000
    cause = page not present, read, user mode
```

`sh.elf`'s very first instruction was the new `movl (%esp), %eax`. Its ESP was `0x40101000` — one byte past the top of the mapped stack page. `sh.elf` was spawned by the *kernel boot path*, not by `cmd_exec`, so I'd never called `elf_setup_args` on it. The new `_start` then dereferenced unmapped memory.

Lesson: changing `_start`'s prologue is a contract change. **Every caller of `task_create_user` for an ELF that was linked with the new `start.S` has to set up argv first**, even if there are no arguments to pass. Fix is one line in kmain:

```c
const char *sh_argv[] = { "sh" };
elf_setup_args(&r, 1, sh_argv);
struct task *t = task_create_user(r.entry, r.user_esp, r.cr3, "sh");
```

`sh.elf` now sees `argc=1, argv[0]="sh"`, ignores both, runs as before. The fix surfaces a small architectural commitment too: from this session forward, "the kernel hands user programs argc/argv on the stack" is a rule, not an option.

## Per-process fd table

```c
#define TASK_MAX_FDS 8

enum { FD_FREE = 0, FD_STDIN, FD_STDOUT, FD_FS };

struct task_fd {
    int      kind;
    int      fs_idx;       /* iff kind == FD_FS */
    uint32_t offset;       /* iff kind == FD_FS */
};

struct task {
    ...
    struct task_fd fds[TASK_MAX_FDS];
};
```

Every `task_create` initializes:

```c
for (int i = 0; i < TASK_MAX_FDS; i++) t->fds[i].kind = FD_FREE;
t->fds[0].kind = FD_STDIN;
t->fds[1].kind = FD_STDOUT;
t->fds[2].kind = FD_STDOUT;     /* stderr → console too */
```

So freshly-spawned user tasks have stdin/stdout/stderr available immediately. fd 3+ are open slots.

This is the smallest possible fd-table model:
- No reference counting — close just frees the slot
- No file-table-vs-fd-table separation (Unix has both: fds in a per-process array, but each entry is a pointer into a global "open file table" with offset/refcount)
- No ownership transfer between processes (no fork/dup)
- No "permissions" check beyond `kind`

It's the *interface* that matters. Real Unix's fd tables look almost exactly like this from the user's POV.

## The four syscalls

```c
case SYS_OPEN: {
    /* copy name from user, look up in fs, allocate the lowest free
     * fd >= 3 in the calling task's fd table */
    ...
    t->fds[fd].kind   = FD_FS;
    t->fds[fd].fs_idx = fs_idx;
    t->fds[fd].offset = 0;
    ret = fd;
}
case SYS_READ: {
    if (kind == FD_STDIN) ret = kshell_read_line(buf, n);
    else if (kind == FD_FS) {
        int rd = fs_read(fs_idx, offset, buf, n);
        if (rd > 0) offset += rd;
        ret = rd;
    }
}
case SYS_WRITE_FD: {
    /* require kind == FD_STDOUT for now; raw kputc loop */
    for (int i = 0; i < n; i++) kputc(buf[i]);
    ret = n;
}
case SYS_CLOSE: {
    if (fd < 3 || fd >= TASK_MAX_FDS)  ret = -1;
    else                               { kind = FD_FREE; ret = 0; }
}
```

Two things worth highlighting:

**`SYS_READ` on stdin is a line-read.** A real `read(0, buf, n)` returns up to `n` bytes as soon as anything's available (or all of them if waiting); it's not line-buffered. Ours dispatches to `kshell_read_line` which blocks until a full line is typed. This is a simplification — our shell tasks read line-at-a-time anyway, so it works. A future `cat` reading from stdin (no args) would need `read` to be byte-oriented.

**The user's pointer is dereferenced directly.** `(char *)(uintptr_t)b` works because we're still on the user task's CR3 inside the syscall handler. This is the same trick session 11 used for `SYS_KCMD`. A defensive kernel would `copy_from_user(kbuf, ubuf, n)` after validating the address range; we trust user code.

## libuser wrappers + name collision

The library already had `int sys_write(char c)` from session 9 (single-char console write, wraps `SYS_WRITE = 1`). Now we want `int sys_write(int fd, const void *buf, int n)` — same name, different signature. C doesn't do overloading.

Renamed the legacy single-char one to `sys_putchar`:

```c
int sys_putchar(char c);                            /* wraps SYS_WRITE = 1 */
int sys_write  (int fd, const void *buf, int n);    /* wraps SYS_WRITE_FD = 12 */
```

`putchar()` (libuser-internal) now calls `sys_putchar`. The in-tree `.up1` / `.up2` programs from session 5 are inline-asm and reference the syscall by *number* via `XSTR(SYS_WRITE)` — we kept `SYS_WRITE = 1` as the single-char wire constant so they keep working untouched. The name `SYS_WRITE_FD = 12` is the new fd-flavored one.

## cat.c

```c
int main(int argc, char **argv) {
    if (argc < 2) {
        sys_write(2, "usage: cat <file> [...]\n", 24);
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i]);
        if (fd < 0) {
            sys_write(2, "cat: ", 5);
            sys_write(2, argv[i], (int)strlen(argv[i]));
            sys_write(2, ": cannot open\n", 14);
            continue;
        }
        char buf[256];
        int n;
        while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
            sys_write(1, buf, n);
        }
        sys_close(fd);
    }
    return 0;
}
```

The whole program. Real arg parsing, real fd lifecycle, real loop until `read` returns 0 (= EOF in our `fs_read` semantics). For `hello.txt` (274 bytes), the read loop runs **twice** — first call returns 256 bytes, second returns 18, third returns 0. Confirms `fs_read`'s offset advancement is correct.

## echo.c

```c
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) sys_write(1, " ", 1);
        sys_write(1, argv[i], (int)strlen(argv[i]));
    }
    sys_write(1, "\n", 1);
    return 0;
}
```

Even shorter. The interesting part is what's not there: no inline asm, no `call/pop` PIC trick, no naked function. The `argv[i]` strings live on the user stack at addresses the kernel computed; `strlen` walks the bytes; `sys_write` ferries them through INT 0x80.

## hello.txt

A six-line text file shipped in the FS image:

```
Hello from a text file on AdventFS.
This was read by cat.elf via SYS_OPEN, SYS_READ, and SYS_CLOSE,
and written to stdout by SYS_WRITE_FD with fd=1.
Each call is a real ring 3 -> ring 0 -> ring 3 round trip through
the IDT[0x80] gate the kernel installed back in session 5.
```

`mkfs.py` got a new `DATA_FILES` list that gets concatenated alongside the ELF programs, with raw bytes — no ELF wrapper. The kernel's `fs.c` doesn't care about the file's type; it just hands bytes through `fs_read`.

## Files added / modified

| File | Change |
|---|---|
| `kernel/task.{h,c}` | Per-process fd table; init in `task_create` |
| `kernel/elf.{h,c}` | `elf_load_result.stack_phys`/`.stack_size`; `elf_setup_args` |
| `kernel/syscall.{h,c}` | `SYS_OPEN/READ/WRITE_FD/CLOSE` dispatch |
| `kernel/shell.c` | `cmd_exec` tokenizes argv; calls `elf_setup_args` |
| `kernel/kernel.c` | Calls `elf_setup_args` for `sh.elf` boot path |
| `user/start.S` | Reads argc/argv from stack, pushes to `main` |
| `user/libuser.{h,c}` | `sys_open/read/write/close`; renamed legacy `sys_write` → `sys_putchar` |
| `user/cat.c`, `user/echo.c` | New programs |
| `fs/hello.txt` | New test data |
| `mkfs.py` | `DATA_FILES` for raw text inclusion |
| `build.sh` | `USER_PROGS+=(cat echo)` |

## Test trace

```
advent$ exec echo.elf hello world from ring 3
exec: pid=5  argc=6  entry=0x40000000  esp=0x40100fbc  (echo.elf)
hello world from ring 3
[user task pid=5 exited code=0]

advent$ exec cat.elf hello.txt
exec: pid=6  argc=2  entry=0x40000000  esp=0x40100fdc  (cat.elf)
Hello from a text file on AdventFS.
This was read by cat.elf via SYS_OPEN, SYS_READ, and SYS_CLOSE,
and written to stdout by SYS_WRITE_FD with fd=1.
Each call is a real ring 3 -> ring 0 -> ring 3 round trip through
the IDT[0x80] gate the kernel installed back in session 5.
[user task pid=6 exited code=0]

advent$ exec cat.elf nope.txt
cat: nope.txt: cannot open
[user task pid=7 exited code=0]

advent$ exec echo.elf one two three
one two three
[user task pid=8 exited code=0]
```

ESP for echo with 6 args = 0x40100fbc; for cat with 2 args = 0x40100fdc. Difference = 32 bytes. The 4-arg `echo` is in between (esp=0x40100fd0). Each program got the right number of argv slots and its strings live above ESP within the same stack page.

## Design decisions

**Stack-based argv (the SysV ABI), not register-based.** We could have crammed a small argv into ESI/EDI as syscall-style. Stack is the universal convention; `_start` is what real toolchains expect.

**16-arg cap.** Hard-coded `if (argc > 16) argc = 16` in both the kernel-side packer and the user shell tokenizer. Plenty for our scale; lets us use a stack-allocated `str_va[16]`.

**fd table is 8 slots.** 0/1/2 pre-wired plus 5 user-openable slots. A real OS would size based on `RLIMIT_NOFILE`. We rebuild the kernel if we want more.

**`SYS_WRITE_FD` only writes to stdout/stderr, not arbitrary file fds.** We have a read-only filesystem, so writing to an fd-3 (FS-backed) descriptor doesn't make sense. Returning `-1` for non-stdout fds keeps the failure mode clean.

**Stdin reads are line-oriented.** Wraps `kshell_read_line`. Means a `cat < /dev/null`-style pipeline doesn't work, but no user program in the tree wants that. The kernel's blocking-read-with-echo behavior is actually what you want for an interactive shell.

**Don't track `argc` separately from argv length.** `argv[argc]` is NULL by ABI. User code typically iterates while `argv[i] != NULL`; we provide the count anyway so trivially-correct loops are easy.

**`cat` opens each file separately.** Like real cat. `argv[1] argv[2] ...` are all opened, read, closed in order. Lets us pass multiple files with a single command.

## Pitfalls

1. **Changing `_start`'s contract** breaks every existing user program built without the matching `elf_setup_args` call. Add the call to **every** user-task spawn site, including kmain's boot path for `sh.elf`.
2. **The user stack page is mapped into two address spaces** simultaneously: user PD at `USER_STACK_VA`, kernel master PD at the page's physical address (identity-mapped). The kernel writes via the physical view; the user reads via the virtual view; both see the same bytes.
3. **`argv[i]` pointers must be USER VAs**, not kernel VAs. The pointer values you write into the argv pointer array have to be `USER_STACK_VA + offset_within_page`, not `stack_phys + offset_within_page`.
4. **Pad to 4-byte alignment** before writing the pointer table. x86 doesn't strictly require it for word reads, but compilers may emit aligned ops if they think the buffer is `int *`-aligned.
5. **Renaming `sys_write` requires updating libuser internally** (e.g., `putchar()` was calling the old single-char version). Search-and-replace shotgun, then build.
6. **Stdin reads via SYS_READ on fd 0** dispatch to `kshell_read_line`, which blocks. A program that wanted `read(0, &c, 1)` (one char at a time) would need a separate kernel path.
7. **`fs_read` returns 0 on EOF**, not `-1`. Loop terminates on `n <= 0`. Consistent with POSIX `read`.

## What might come next

`pipe(fd[2])` + `dup2` + a real way to chain `cat hello.txt | echo X` would change the shape of userspace meaningfully. Or a proper `lseek` for random-access reads. Or an in-RAM writable filesystem so user programs can actually create files. Or `argv[]` parsed into a real environment with `getenv` semantics. Many threads pulling on the same loose end — pick one and tug.
