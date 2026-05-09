# Session 15 — Pipes, redirection, and the libuser stdout cleanup

**Goal:** Make `cmd1 | cmd2` and `cmd > file` actually work in the userspace shell. Mechanically: add `SYS_PIPE` (returns two fds wired to a kernel ring buffer), add `SYS_DUP2`, teach the shell to parse `|` and `>`, then perform the canonical Unix `fork → dup2 → close → exec` dance for each pipeline stage.

The user explicitly flagged the structural cleanup that pipes force: libuser's `putchar` / `puts` / `printf` had been calling the legacy single-char `SYS_WRITE` and the NUL-terminated `SYS_WRITE_STR` — neither of which routes through the per-process fd table. After this session they all go through `sys_write(1, buf, n)` so that `dup2(pipe_w, 1)` actually causes a child's printf to land in the pipe instead of the console.

End state — a self-test the kernel runs at boot:

```
=== sh selftest: fork / exec / wait / pipe / > ===
[t1] forktest:
  child : pid=6  marker=0xbabe  (was 0xCAFE in parent)
  parent: pid=5  marker=0xcafe  child_pid=6  reaped=6  exit=42
[t2] fork + exec hello.elf:
Hello from C in ring 3! ...
  parent waited: pid=7  exit=0
[t3] pipe: echo hello world | cat
hello world from a pipe
  pipeline rc=0
[t4] redirect: echo line1 > tmp.txt ; cat tmp.txt
line1 written via redirect
  cat rc=0
[t5] pipeline + redirect: echo a b c | cat > tmp2.txt ; cat tmp2.txt
a b c
=== selftest done ===
```

The "hello world from a pipe" line in t3 was written by `echo` (pid 8) to its stdout, traveled through a kernel ring buffer, and was read by `cat` (pid 9) from its stdin — which `cat` then wrote to the console. t4 + t5 prove the same machinery cleanly redirects through an in-RAM tmpfs file. `httpd.elf` keeps serving curl on :80 throughout.

## What's in scope

In:
- `kernel/pipe.{h,c}` — fixed pool of pipes, each an SPSC ring buffer with separate read/write refcounts
- `kernel/tmpfs.{h,c}` — in-RAM writable filesystem; the `>` target lives here and is reachable by `cat name` afterwards
- `SYS_PIPE = 21`, `SYS_DUP2 = 22`, `SYS_OPEN_W = 23`
- New fd kinds: `FD_PIPE_R`, `FD_PIPE_W`, `FD_TMPFS`
- `struct task_fd` collapsed from `(kind, fs_idx, sock_idx, offset)` to `(kind, obj_idx, offset)` — the structural cleanup that makes dup2 a memcpy
- libuser `putchar` / `puts` / `printf` route through `sys_write(1, ...)`
- libuser wrappers `sys_pipe` / `sys_dup2` / `sys_open_w`
- `cat` extended to read from stdin when `argc < 2`
- Shell parser handles `|` and `>`; `run_pipeline` does fork + dup2 + close + exec for each stage and waits for all children
- Refcount-aware close on fork copy, dup2, and task exit (so a child's pipe-write fd produces EOF when the child exits without an explicit close)

Out:
- `<` (stdin redirect) — no demand from the milestone, plumbing is symmetric to `>` but a half-day to add
- `>>` append, `2>` stderr redirect, `&>`, `|&`
- Multiple fds dup'd onto each other inside one stage (`dup2(fd, 5)` etc) — works at the kernel level, but the parser doesn't expose it
- Pipeline-as-builtin (e.g. `forktest | cat`) — builtins still run inline in the shell process, no fork
- Background jobs (`&`)
- Job control / signals
- A real on-disk writable filesystem; tmpfs is RAM-only and resets at reboot
- `unlink` for tmpfs (files leak when the slot is reused-by-name or until reboot)
- Refcounted sockets — sockets still share by index without explicit refs (fork's existing best-effort behavior is preserved)

## Architecture: three pieces snap together

The shell's pipeline dance only works if three layers all agree on what an fd means:

1. **The kernel's fd table** — has to expose pipes / redirect-files as first-class fds so `read`/`write`/`close` route the same way regardless of what's behind them.
2. **dup2** — has to atomically replace one fd's contents with another's, bumping the underlying object's refcount so nothing dangles.
3. **libuser's stdout path** — every `printf` has to go through `sys_write(1, ...)` so that fd 1 is the actual indirection point. Otherwise dup2(pipe_w, 1) is a lie.

The session's structural cleanup ties all three together by collapsing the fd entry to `(kind, obj_idx, offset)`.

## struct task_fd, refactored

Before:

```c
struct task_fd {
    int      kind;
    int      fs_idx;       /* iff kind == FD_FS    */
    uint32_t offset;       /* iff kind == FD_FS    */
    int      sock_idx;     /* iff kind == FD_SOCK  */
};
```

Three "iff kind == X" fields, with new ones (`pipe_idx`, `tmpfs_idx`) about to land. After:

```c
struct task_fd {
    int      kind;
    int      obj_idx;      /* fs_idx / sock_idx / pipe_idx / tmpfs_idx */
    uint32_t offset;       /* used by FD_FS, FD_TMPFS                  */
};
```

One discriminator + one index. The kind tells you which per-kind table `obj_idx` indexes into. `dup2` becomes literally `t->fds[newfd] = t->fds[oldfd]; bump_ref(...);` — no per-kind branching for the copy itself.

The kernel fd-kind enum grows by three:

```c
enum {
    FD_FREE = 0,
    FD_STDIN, FD_STDOUT,
    FD_FS, FD_SOCK,
    FD_PIPE_R, FD_PIPE_W,
    FD_TMPFS,
};
```

Every place that switched on `kind` (SYS_READ / SYS_WRITE_FD / SYS_CLOSE / SYS_BIND/LISTEN/ACCEPT) gets cases for the new kinds. SYS_OPEN now does fs lookup → tmpfs lookup fallback; SYS_OPEN_W goes straight to tmpfs.

## Pipes

```c
struct pipe {
    int               in_use;
    volatile int      read_refs;
    volatile int      write_refs;

    volatile uint32_t head;            /* producer side */
    volatile uint32_t tail;            /* consumer side */
    uint8_t           buf[PIPE_BUF_SZ];
};
```

Same SPSC-ring discipline as session 13's sockets: the producer and consumer each own one of `head`/`tail`, both are 32-bit aligned (atomic on x86), and `volatile` keeps the compiler from hoisting the busy-wait check.

Per-end refcounts are the new bit. They give us cleanly-defined termination conditions:

- `pipe_read` blocks while `head == tail` AND `write_refs > 0`. When the last writer closes its write end, `write_refs` reaches 0 and the next iteration falls through; `pipe_read` returns 0 → EOF.
- `pipe_write` spins yielding while the ring is full AND `read_refs > 0`. If the reader has gone away, the next attempt returns -1 immediately rather than blocking forever.

`fork()` copies the parent's fd table by `memcpy`, so refcounts have to be bumped for every entry pointing at a pipe (or tmpfs). Otherwise the child's fork-inherited fd would silently turn into a dangling reference the moment the parent closed its own.

```c
for (int i = 0; i < TASK_MAX_FDS; i++) {
    child->fds[i] = parent->fds[i];
    switch (parent->fds[i].kind) {
        case FD_PIPE_R: pipe_inc_read (parent->fds[i].obj_idx); break;
        case FD_PIPE_W: pipe_inc_write(parent->fds[i].obj_idx); break;
        case FD_TMPFS:  tmpfs_inc_ref (parent->fds[i].obj_idx); break;
        default: break;
    }
}
```

`dup2` does the same per-kind bump after the entry copy.

## tmpfs (the > target)

`>` redirects a child's stdout to a file. AdventOS's only on-disk fs is read-only, so we need a writable backing store. tmpfs is the smallest one I could justify: a fixed pool of 16 in-RAM files, kmalloc-backed buffers that double on overflow, name lookups via linear scan.

The whole interface is six functions:

```c
int  tmpfs_open  (const char *name);    /* lookup + bump refs   */
int  tmpfs_create(const char *name);    /* create or truncate   */
void tmpfs_close (int idx);              /* drop a reference     */
void tmpfs_inc_ref(int idx);             /* fork / dup2 helper   */
int  tmpfs_read  (int idx, uint32_t off, void *buf, uint32_t n);
int  tmpfs_write (int idx, const void *buf, uint32_t n);  /* appends */
```

A subtlety: data persists past `refs == 0`. That's a deliberate departure from POSIX (where unlink-while-open keeps the inode alive only until the last close, then frees it; never-unlinked files keep their data) so that `echo hi > foo; cat foo` round-trips even though the redirect closes its tmpfs fd between the two commands. Real Unix would handle this via "file exists by name in a directory" — we don't have a directory, so we keep the entry around by name. There's no `unlink` syscall; tmpfs slots only get reclaimed at reboot.

`SYS_OPEN` falls through to tmpfs after the on-disk lookup misses, so `cat foo` in the shell uses the same `sys_open` path regardless of where `foo` lives. The fd it returns has `kind = FD_TMPFS` and the `SYS_READ` switch routes there.

## SYS_PIPE / SYS_DUP2 / SYS_OPEN_W

```c
case SYS_PIPE: {
    int *ufds = (int *)a;
    struct task *t = task_current();

    int rfd = alloc_fd(t);
    if (rfd < 0) { ret = -1; break; }
    /* Tentatively claim rfd so alloc_fd doesn't return it again. */
    t->fds[rfd].kind = FD_PIPE_R;
    int wfd = alloc_fd(t);
    if (wfd < 0) { t->fds[rfd].kind = FD_FREE; ret = -1; break; }

    int p = pipe_new();
    if (p < 0) { t->fds[rfd].kind = FD_FREE; ret = -1; break; }

    t->fds[rfd].kind = FD_PIPE_R; t->fds[rfd].obj_idx = p;
    t->fds[wfd].kind = FD_PIPE_W; t->fds[wfd].obj_idx = p;
    ufds[0] = rfd;
    ufds[1] = wfd;
    ret = 0;
}
```

The "tentatively claim" step exists because `alloc_fd` returns the lowest free slot — call it twice in a row without claiming and you'd get the same fd both times. Simplest fix: stamp `kind = FD_PIPE_R` on the first slot before asking for the second, then fix up `obj_idx` once the pipe is allocated.

`SYS_DUP2` is the smallest case in the dispatcher:

```c
case SYS_DUP2: {
    int oldfd = (int)a, newfd = (int)b;
    struct task *t = task_current();
    if (...validate...) { ret = -1; break; }
    if (oldfd == newfd) { ret = newfd; break; }

    /* Drop newfd's existing reference if any. POSIX dup2 is
     * "atomic close+dup" — newfd must never be briefly invalid. */
    if (t->fds[newfd].kind != FD_FREE) release_fd(&t->fds[newfd]);

    t->fds[newfd] = t->fds[oldfd];

    switch (t->fds[oldfd].kind) {
        case FD_PIPE_R:  pipe_inc_read (t->fds[oldfd].obj_idx); break;
        case FD_PIPE_W:  pipe_inc_write(t->fds[oldfd].obj_idx); break;
        case FD_TMPFS:   tmpfs_inc_ref (t->fds[oldfd].obj_idx); break;
        default: break;
    }
    ret = newfd;
}
```

The whole point of the struct-refactor is right there: the entry copy is one assignment.

## libuser stdout: the cleanup that pipes forced

Before this session, libuser's stdout went through two legacy syscalls:

```c
void putchar(char c)     { sys_putchar(c); }     /* SYS_WRITE = 1     */
void puts(const char *s) { sys_write_str(s); }   /* SYS_WRITE_STR = 5 */
```

Both of those land in the kernel as a `kputc` loop that writes to VGA + serial directly, *without consulting the calling task's fd table*. So even if the shell did `dup2(pipe_w, 1)` before `exec("echo")`, echo's `printf` would still have gone to the console and the pipeline would print twice (once on the console, once after cat).

After:

```c
void putchar(char c)     { sys_write(1, &c, 1); }
void puts(const char *s) {
    int n = 0; while (s[n]) n++;
    sys_write(1, s, n);
}
```

`sys_write(1, ...)` = `SYS_WRITE_FD` with fd=1, which dispatches on `t->fds[1].kind`. For a freshly-spawned task, fd 1 is `FD_STDOUT` and the dispatch goes to `kputc`. After `dup2(pipe_w, 1)`, fd 1 is `FD_PIPE_W` and the dispatch goes to `pipe_write`. The user-mode call site is unchanged.

The legacy `SYS_WRITE = 1` and `SYS_WRITE_STR = 5` syscalls are still there — the `.up1`/`.up2` asm demos from session 5 use `SYS_WRITE` directly via inline asm with `XSTR(SYS_WRITE)`. Removing them would break those programs without buying anything; leaving them just costs a few unused syscall numbers.

## The shell's pipeline dance

`run_pipeline` is the bulk of the new shell code. The structure is the canonical Unix one:

```c
int n = pl->nstages;

/* 1. allocate n-1 pipes up front */
int pipes[PIPELINE_MAX - 1][2];
for (int i = 0; i < n - 1; i++) sys_pipe(pipes[i]);

/* 2. open the > target if any (parent holds a reference) */
int outfd = pl->outfile ? sys_open_w(pl->outfile) : -1;

/* 3. fork one child per stage */
int pids[PIPELINE_MAX];
for (int i = 0; i < n; i++) {
    int pid = sys_fork();
    if (pid == 0) {
        /* Child stage i:
         *   - if i > 0   : dup the prev pipe's read end to fd 0
         *   - if i < n-1 : dup this pipe's write end to fd 1
         *   - if i==n-1 and outfile: dup outfd to fd 1
         *   - close ALL pipe fds + outfd we no longer need
         *   - exec
         */
        if (i > 0)            sys_dup2(pipes[i - 1][0], 0);
        if (i < n - 1)        sys_dup2(pipes[i][1],     1);
        else if (outfd >= 0)  sys_dup2(outfd,           1);

        for (int k = 0; k < n - 1; k++) {
            sys_close(pipes[k][0]);
            sys_close(pipes[k][1]);
        }
        if (outfd >= 0) sys_close(outfd);

        sys_exec(path, argv);
        sys_exit(127);
    }
    pids[i] = pid;
}

/* 4. parent: drop ALL pipe references so writers can EOF */
for (int i = 0; i < n - 1; i++) {
    sys_close(pipes[i][0]);
    sys_close(pipes[i][1]);
}
if (outfd >= 0) sys_close(outfd);

/* 5. wait for every child */
for (int waited = 0; waited < n; waited++) {
    int code; sys_wait(&code);
}
```

The only line worth re-reading is step 4. If the parent kept the read end of `pipes[0]` open after forking the children, then `cat` (stage 1) would never see EOF on its stdin, because `read_refs` would still count the parent's reference. The child writing to `pipes[0]` (echo, stage 0) could exit, drop its `write_refs` to zero, and `pipe_read` in cat would unblock and return 0 — *only because* nobody else holds the write end. The parent's read-end reference doesn't block EOF directly, but it does count toward `read_refs`, which means writers wouldn't see EPIPE if they wrote to a "closed" pipe. Same idea: the parent has to let go of both ends of every pipe so the children can detect each other's exits.

## task_exit: close all fds

Pipes need EOF to fire when a child exits without explicitly closing its write end. Otherwise a `printf("hi"); return 0;` style program in a pipeline leaves the reader hanging forever.

```c
static void close_all_fds(struct task *t) {
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        struct task_fd *e = &t->fds[i];
        switch (e->kind) {
            case FD_SOCK:   sock_close      (e->obj_idx); break;
            case FD_PIPE_R: pipe_close_read (e->obj_idx); break;
            case FD_PIPE_W: pipe_close_write(e->obj_idx); break;
            case FD_TMPFS:  tmpfs_close     (e->obj_idx); break;
            default: break;
        }
        e->kind = FD_FREE;
        e->obj_idx = -1;
        e->offset = 0;
    }
}

void task_exit_current(int exit_code) {
    struct task *t = g_current;
    t->exit_code = exit_code;
    if (t->is_user) close_all_fds(t);
    /* ... wake parent + go ZOMBIE ... */
}
```

Before this change, a child that did `printf("...");` and let main return would leave `pipe_w` open (the child's fd table got reaped later by the kernel reaper, which freed memory but didn't decrement pipe refs). The reader would block forever in `pipe_read`. Adding the explicit close in `task_exit_current` fixes it for the normal-exit case.

## Tokenizer and the `|` / `>` handling

The shell tokenizer used to split only on whitespace. Now it also produces standalone tokens for `|` and `>`, sourced from per-operator static buffers (so the operator character can be NUL-terminated independently of the surrounding line):

```c
if (*p == '|' || *p == '>') {
    char saved = *p;
    *p++ = 0;
    static char pipe_tok[2] = {'|', 0};
    static char gt_tok  [2] = {'>', 0};
    tokens[n++] = (saved == '|') ? pipe_tok : gt_tok;
    continue;
}
```

The parser then walks the token array, splitting at `|` boundaries:

```c
for (int j = 0; j < ntok; j++) {
    char *t = tokens[j];
    if (t[0] == '|' && t[1] == 0) {
        pl->stages[pl->nstages].argv = &tokens[start];
        pl->stages[pl->nstages].argc = j - start;
        tokens[j] = 0;            /* terminate the slice for exec()'s argv */
        pl->nstages++;
        start = j + 1;
    } else if (t[0] == '>' && t[1] == 0) {
        /* Last stage; rest of the tokens are the > target. */
        ...
        pl->outfile = tokens[j + 1];
        return 0;
    }
}
```

The clobbering of `tokens[j] = 0` is the trick that lets each stage's `argv` be a pointer into the same big `tokens[]` array — the `0` slot at the end of the stage doubles as the NULL terminator that `exec()` expects.

## The bug that bit: static-buffer-in-discarded-bss

First test pass produced this:

```
[t3] pipe: echo hello world | cat
[!] CPU EXCEPTION 14: Page fault (err=0x6) at 1b:40000581  eflags=0x10202
    fault addr (CR2) = 0x00000000
    cause = page not present, write, user mode
```

Disassembling around `0x40000581` showed `mov %al, 0(%edx)` storing through edx — the inlined `resolve_program` writing `.elf` into its `static char buf[64]`. CR2=0 meant edx was 0.

Why was the buffer at address 0?

Looking at the symbol table:

```
$ objdump -t user/_obj/sh.elf | grep buf
[ 14](sec  0)(fl 0x00)(ty    0)(scl   3) (nx 0) 0x00000000 _buf.0
```

`sec 0` = "no section." `_buf.0` (the mangled name for the function-static) was discarded by `user.ld`'s `/DISCARD/ : { *(.bss*) ... }`. Function-static-array-without-initializer is implicit-zero-init, which `mingw32 GCC` puts in `.bss`, even though `-fno-zero-initialized-in-bss` is on the command line — that flag turns out to apply only to file-scope statics, not block-scope ones. The linker discards `.bss`, the symbol gets resolved at address 0, and the next byte-store faults.

We can't safely keep `.bss` (no startup zeroes it; the kernel ELF loader's `memset(page, 0, PAGE_SIZE)` covers fresh pages but the layout of where buf lands isn't guaranteed in our user.ld). Two clean fixes:

1. Add a `.bss` zeroing pass to `_start` (would let any function-local static work).
2. Force the buffer into `.data` by giving it a non-zero initializer.

I went with (2) for the one offender because it's the smaller blast radius:

```c
static char buf[64] = ".";
```

That single dot is enough to push the variable into `.data` (which has explicit content from the file). The first iteration of `resolve_program` overwrites it anyway. A future session that adds more user programs with `.bss` needs is the right time to do (1).

## Files added / modified

| File | Change |
|---|---|
| `kernel/pipe.{h,c}` | New. SPSC ring + per-end refcounts |
| `kernel/tmpfs.{h,c}` | New. In-RAM writable files for `>` |
| `kernel/task.{h,c}` | `struct task_fd` collapsed to `(kind, obj_idx, offset)`; FD_PIPE_R/W/FD_TMPFS kinds; fork bumps refs; exit closes all fds |
| `kernel/syscall.{h,c}` | SYS_PIPE/DUP2/OPEN_W; refactored fd dispatch with `alloc_fd` + `release_fd` helpers; tmpfs fallback in SYS_OPEN |
| `kernel/kernel.c` | `pipe_init()` + `tmpfs_init()` calls |
| `user/libuser.{h,c}` | putchar/puts/printf route through `sys_write(1, ...)`; sys_pipe/dup2/open_w wrappers |
| `user/sh.c` | Tokenizer treats `\|` and `>` as standalone tokens; `parse_pipeline`; `run_pipeline` does the fork+dup2+close+exec dance; selftest covers t3/t4/t5 |
| `user/cat.c` | Reads from stdin when `argc < 2`, so it slots into pipelines |

## Test trace

Boot with `selftest` arg passes all five tests:

```
[t3] pipe: echo hello world | cat
[user task pid=8 exited code=0]   ← echo
hello world from a pipe            ← cat printed it
[user task pid=9 exited code=0]   ← cat
  pipeline rc=0

[t4] redirect: echo line1 > tmp.txt ; cat tmp.txt
[user task pid=10 exited code=0]
  redirect rc=0  (now reading it back)
line1 written via redirect
[user task pid=11 exited code=0]
  cat rc=0

[t5] pipeline + redirect: echo a b c | cat > tmp2.txt ; cat tmp2.txt
[user task pid=12 exited code=0]
[user task pid=13 exited code=0]
  rc=0
a b c
[user task pid=14 exited code=0]
```

`curl http://localhost:8080/` still returns 200 / 317 bytes — `httpd.elf`'s `sock_accept` loop is unaffected by the new states.

## Design decisions

**Per-end refcounts on pipes, not whole-pipe refcounts.** Whole-pipe ref would tell us when nobody owns the pipe, but EOF semantics need to know specifically when the last *writer* closed (so readers stop blocking). Two counters, one per end, gives us both for one extra int per pipe.

**SPSC ring, not a linked list of buffers.** Same shape as `sock_accept`'s ring, same trade-off (single-producer / single-consumer assumption holds for `pipe(2)` because there's exactly one read-side and one write-side fd kind). The 4 KiB fixed buffer is enough that small programs rarely fill it.

**`>` writes to an in-RAM tmpfs, not a real file.** The on-disk AdventFS is read-only and adding write support is a half-session of its own. tmpfs is ~120 lines and round-trips cleanly within a single boot, which is enough to demonstrate the redirect plumbing.

**tmpfs data persists past `refs == 0`.** The alternative (free on last close) breaks `echo hi > foo; cat foo` because the redirect-fd closes between the two commands. Real Unix sidesteps this with directory entries holding implicit references; we just keep the data.

**One unified `obj_idx` instead of separate `fs_idx`/`sock_idx`/`pipe_idx`/`tmpfs_idx`.** dup2 becomes one assignment. The cost is that you can't tell at a glance which table an entry references without consulting `kind`, but every code path that touches the entry already has to switch on `kind` anyway.

**libuser routes stdout through `sys_write(1, ...)`, not `SYS_WRITE`/`SYS_WRITE_STR`.** Forced by the dup2-must-redirect requirement. Legacy syscalls stay because the in-tree `.up1`/`.up2` asm demos still use them by number.

**Pipeline parser handles `\|` and `>` only.** `<` and `>>` would be near-trivial to add but the milestone doesn't ask. The one-pass parser is small enough to read top to bottom.

**Builtins still run inline in the shell, not as pipeline stages.** `forktest | cat` doesn't work — the shell's builtin dispatch happens before pipeline parsing. POSIX shells handle this with sub-shell forks for builtins that appear in pipelines, which is more machinery than the milestone needs.

**`run_pipeline` returns the LAST stage's exit code.** Standard `$?` semantics. We don't bother with `pipefail` (return non-zero if any stage failed).

**Pipes are statically allocated (PIPE_MAX = 8).** Same as sockets. No fragmentation, indices stable for the lifetime of the boot, easy to reason about.

## Pitfalls

1. **The `static char buf[64]` static-in-discarded-.bss bug.** Documented above. `-fno-zero-initialized-in-bss` doesn't catch block-scope statics on mingw32; the linker discards `.bss`; the symbol resolves to address 0. Either zero `.bss` at startup or give every static-local an explicit non-zero initializer.
2. **Parent must close every pipe end after forking.** If it holds even one read or write end open, the children's EOF semantics break. The shell's `run_pipeline` does this; less disciplined code wouldn't.
3. **`task_exit` must close all fds.** Otherwise a child that exits without explicit `close()` leaves pipe writers ref-counted at >0 and the reader blocks forever.
4. **`fork()` must bump refcounts on copied fd entries.** Pipes especially — without it, the child's inherited pipe fd becomes a dangling reference the moment the parent closes.
5. **`alloc_fd` returns the lowest free slot.** Calling it twice without claiming the first returns the same fd both times. SYS_PIPE has to stamp `kind = FD_PIPE_R` on the first slot before allocating the second.
6. **`SYS_DUP2` must `release_fd(newfd)` before overwriting.** Otherwise the previous resource leaks (refcount never decremented).
7. **The volatile on `head`/`tail`/`*_refs` is load-bearing.** Same warning as session 13's sockets — drop it and the busy-yield loop hoists out of the polling check, freezing forever.
8. **Pipe writes can lose bytes if the reader closes mid-write.** Documented: `pipe_write` returns -1 if `read_refs == 0` and we haven't written anything; partial-success returns the partial count. Real Unix would deliver SIGPIPE; we just propagate -1.
9. **tmpfs files leak slots.** No `unlink`. Hit the 16-file limit and you're stuck until reboot.
10. **The pipeline parser is positional.** `>` only attaches to the last stage; `cmd > foo | cat` doesn't redirect inside the pipeline. POSIX shells parse redirects per-stage; we don't.
11. **Builtins-in-pipelines silently no-op.** The shell dispatches to a builtin if `toks[0]` matches one — pipelines past that point are ignored.

## What might come next

`<` would round out the basic redirection set. `>>` would add append. A real `unlink` syscall would free tmpfs slots. Stderr redirection (`2>`) needs the dup2 to work on fd 2. Background jobs (`&`) need the shell to skip the wait, plus a job table. Signals (`SIGPIPE`, `SIGINT`) would let pipelines react sanely to broken readers and Ctrl-C. Each is a couple of hundred lines on the same machinery this session laid down.
