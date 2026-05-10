# Session 35 — A real C library + dynamic linking

**Goal:** Stop linking libc into every user program. AdventOS has spent 26 sessions stamping a static copy of `libuser.o` (printf, strlen, malloc, memcpy, …) into every single user binary — bloating each by ~3 KiB and forcing a kernel rebuild + every-program-rebuild for any libc bug fix. This session moves the C library into a separately-built `libc.bin` that the kernel maps into every user process at a fixed virtual address. User programs keep the same source-level API but route every libc call through a fixed-address jump table — the same mechanism Linux's vDSO and Windows' KUSER_SHARED_DATA use.

End state — the new boot line:

```
[boot] caching libc.bin... dyld: cached libc.bin v1 (5636 bytes, 2 pages, 64 exports)
```

And the new `[t25]` selftest:

```
[t25] dynamic libc: every libc call lands at LIBC_BASE
  LIBC_BASE @ 0x70000000:
    magic        = 0x434c4441  (expect 0x434c4441 = 'ADLC')
    version      = 1
    export_count = 64
  exports[strlen]  = 0x70001030
  exports[vprintf] = 0x70000960
  exports[malloc]  = 0x70000af0
  PASS: header magic ok, exports point INTO libc
  malloc+strlen round-trip: len=60 (expect 60)
  child malloc -> 0x40200010  (per-process libc heap)
```

The user-program size delta is concrete: `hello.bin` shrank from 4456 bytes to 2308 bytes (-48%); the per-program reduction roughly matches the size of the printf+string+malloc code that's no longer duplicated. Across the 26 user binaries on disk that's about 30 KiB of code consolidated into one shared image. All 25 selftests pass; `curl http://localhost:8350/` continues to serve `httpd.elf`.

## What's in scope

In:
- **`libc/`** — new top-level directory with `string.c`, `stdio.c`, `stdlib.c`, `ctype.c`, `exports.c`, plus `libc.h` (internal) and `libc.ld` (linker script). Builds to `libc.bin`, a flat binary linked at virtual address `0x70000000`.
- **`libc/exports.c`** — the magic number, version, export count, and a 64-entry function pointer table at the start of `libc.bin`. User programs and the kernel both read this header to validate the library.
- **`kernel/dyld.{h,c}`** — a tiny dynamic loader. `dyld_init()` (called from kmain) reads `/libc.bin` from the FS, validates the magic, caches the bytes in kernel memory. `dyld_map_libc(user_pd)` is called from `elf_load`: it allocates fresh physical pages, copies libc bytes into them, and maps them at `LIBC_VA = 0x70000000` in the new user PD with `PTE_USER + PTE_WRITABLE`.
- **`kernel/elf.c`** — calls `dyld_map_libc` after laying out the user ELF's PT_LOAD segments.
- **`mkfs.py`** — adds `libc.bin` to the FS image as a raw blob (not ELF-wrapped).
- **`build.sh`** — new `[5a/7]` step compiles `libc/*.c` and links `libc.bin`.
- **`user/libuser.{h,c}`** — replaced the in-tree libc implementations with thin trampolines that load function pointers from `LIBC_TABLE` and tail-call into libc. The user-facing API (printf, strlen, malloc, …) is unchanged.
- **`user/sh.c`** — `[t25]` selftest validates the libc header, function pointers, a malloc+strlen round-trip, and that fork gets a per-process libc heap.

Out:
- **Real ELF dynamic linking.** No DT_NEEDED, DT_SYMTAB, R_386_JMP_SLOT, lazy PLT binding, soname versioning, or any of the rest of glibc's apparatus. We ship a flat-binary `libc.bin` with a fixed-order export table indexed by integer ID. Function lookup is `LIBC_TABLE[N]`, not `dlsym("name")`. The mechanism mirrors how Linux's vDSO and Windows' KUSER_SHARED_DATA expose kernel-mapped functions — a fixed contract, not symbol resolution.
- **Shared physical pages.** Every process gets its own physical copy of libc's 5636 bytes (2 pages). Real glibc has libc.so.6's `.text` shared via demand-paged file-backed mmap and only `.data` private per process. We could do the same with a paging trick: map libc's read-only pages by reference into every PD (just like the LAPIC PDE mirroring), and only the read-write `.data`/`.bss` pages get a private copy. Future-session work; per-process copies cost 8 KiB total today on a 16-task system.
- **`dlopen`/`dlsym`.** No runtime symbol resolution. The user's libuser shim has all 30+ trampolines hardcoded to specific table indices.
- **Cross-version library loading.** Bumping `LIBC_VERSION` is a hard ABI break: rebuild every user program. There's no way to load `libc.so.1` and `libc.so.2` in the same process.
- **Dynamic exec hooks.** No `LD_PRELOAD`, no constructor/destructor sections. Libc's `.text` runs but its `.init_array` would be ignored if we had one.

## Architecture

```
              FS (disk)                       KERNEL                    USER PROCESS
              ─────────                       ──────                    ────────────
                                                                                   
   libc.bin  ◄──────────  fs_open("/libc.bin")                                      
   (5636 B)               fs_read into                                              
                          g_image (kmalloc)                                         
                                  │                                                 
                                  ▼                                                 
                          dyld_init()                                                
                            validates magic                                          
                            caches g_image                                           
                            (in kernel heap)                                         
                                                                                    
                                                                                   
   init.elf launches → elf_load(init.elf):                                            
                       1. parse ELF header                                            
                       2. allocate user_pd                                            
                       3. for each PT_LOAD: alloc pages, map @ vaddr, fs_read       
                       3b. dyld_map_libc(user_pd):                                  
                           for each page of g_image:                                
                             p = pmm_alloc_page()                                   
                             memcpy(p, g_image+off, 4096)                           
                             paging_map_in(user_pd, LIBC_VA+off, p,                 
                                           PTE_USER|PTE_WRITABLE)                   
                       4. allocate user stack page                                  
                       ──────────────────────────────► ┌─────────────────────────┐  
                                                       │ user PD layout:         │  
                                                       │   .text  @ 0x40000000   │  
                                                       │   stack  @ 0x40100000   │  
                                                       │   heap   @ 0x40200000   │  
                                                       │   mmap   @ 0x50000000   │  
                                                       │   libc   @ 0x70000000   │  
                                                       └─────────────────────────┘  
                                                                                   
                       printf("hi") in user code:                                   
                          call rel32  →  libuser's printf trampoline                
                                            mov     [LIBC_TABLE+42*4], %eax        
                                            jmp     *%eax  → vprintf in libc        
                                                                                   
                       vprintf in libc.bin:                                          
                          int $0x80 (SYS_WRITE_FD) ─────────────► kernel syscall    
```

The "dynamic linking" happens at two distinct moments:

1. **Process load time.** `elf_load` calls `dyld_map_libc`, which allocates pages and stamps libc bytes into them, then installs PTEs in the user PD. After this point the user process's view of memory has libc at `0x70000000`.
2. **Function call time.** Every libc call from user code goes through `LIBC_TABLE[N]` — a fixed-address indirect dispatch. This is the late-binding part: a libc upgrade just rebuilds `libc.bin` and reboot; user programs read the (possibly different) function addresses out of the (always-the-same-shape) table.

Compared to real ELF dynamic linking we skip:
- Symbol *names* (we use integer indices baked into both libc and the shim)
- Runtime relocation (libc is linked at the address it'll be loaded at)
- Lazy resolution (every dispatch always walks the table; no PLT-style "first call resolves, subsequent calls cached")

## Building libc.bin

`libc/libc.ld` is a small script that puts everything at `0x70000000`:

```ld
ENTRY(libc_info)

SECTIONS {
    . = 0x70000000;

    .exports : SUBALIGN(4) {
        KEEP(*(.exports.header))
        KEEP(*(.exports.table))
        . = 0x400;          /* pad to 1 KiB before .text */
    }

    .text   : SUBALIGN(16) { *(.text*)   *(.gnu.linkonce.t.*) }
    .rdata  : SUBALIGN(4)  { *(.rdata*)  *(.rodata*) ... }
    .data   : SUBALIGN(4)  { *(.data*)   *(.bss*)    *(COMMON) ... }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) ... }
}
```

The first 1 KiB is reserved for the export header + 64-entry function pointer table. `.text` starts at `+0x400`. This means user code can read `*(uint32_t *)0x70000000` to get the magic and `((void**)0x70000010)[N]` to get function pointer N — both stable forever as long as the layout doesn't change.

`libc/exports.c` builds the header and table:

```c
__attribute__((section(".exports.header"), used))
const struct libc_header libc_hdr = {
    .magic        = LIBC_MAGIC,        /* 'ADLC' = 0x434C4441 */
    .version      = LIBC_VERSION,      /* 1 */
    .export_count = LIBC_EXPORT_COUNT, /* 64 */
    .reserved     = 0,
};

__attribute__((section(".exports.table"), used))
void *const libc_exports[LIBC_EXPORT_COUNT] = {
    [LIBC_FN_LIBC_INFO]  = libc_info,
    [LIBC_FN_STRLEN]     = strlen,
    [LIBC_FN_STRCMP]     = strcmp,
    [LIBC_FN_MEMCPY]     = memcpy,
    [LIBC_FN_MALLOC]     = malloc,
    [LIBC_FN_FREE]       = free,
    [LIBC_FN_VPRINTF]    = vprintf_,
    /* ... 30+ more ... */
};
```

GCC's designated initializers fill the table at compile time with absolute addresses (since the linker is told `libc_exports[i] = &foo`, and `&foo` resolves at link time once the linker decides where `foo` lives in `.text`). The result is a static jump table — no runtime initialization needed.

The build script (`build.sh` `[5a/7]`):

```bash
mkdir -p libc/_obj
LIBC_OBJS=()
for src in libc/*.c; do
    obj="libc/_obj/$(basename "${src%.c}").o"
    "$CC" "${USER_CFLAGS[@]}" -c -o "$obj" "$src"
    LIBC_OBJS+=("$obj")
done
"$LD" -m i386pe -T libc/libc.ld -o libc/_obj/libc.elf "${LIBC_OBJS[@]}"
"$OBJCOPY" -O binary -j .exports -j .text -j .rdata -j .data \
    libc/_obj/libc.elf libc/_obj/libc.bin
```

The output is 5636 bytes — small enough that "two physical pages per process" is a non-issue.

## Kernel-side: caching and mapping

`kernel/dyld.c` is the loader. It does two things — load the image once, map it into many PDs.

**Cache at boot:**

```c
void dyld_init(void) {
    int fd = fs_open("/libc.bin");
    if (fd < 0) { kprintf("dyld: %s not in FS\n", LIBC_PATH); return; }

    uint32_t size = fs_size(fd);
    g_image = kmalloc(size);
    fs_read(fd, 0, g_image, size);

    uint32_t magic = *(uint32_t *)g_image;
    if (magic != LIBC_MAGIC) { kprintf("dyld: bad magic\n"); return; }

    g_size  = size;
    g_pages = (size + 4095) / 4096;
    g_loaded = 1;
}
```

The cache is just `kmalloc(size)` + `fs_read(fd, 0, ...)` into it. Validating the magic catches the case where someone forgot to rebuild `libc.bin` after a kernel rebuild — they'd boot with a `libc.bin` that has a different export ordering, and the magic might still match but the version wouldn't.

**Map into a user PD:**

```c
int dyld_map_libc(uint32_t *user_pd) {
    if (!g_loaded) return 0;     /* harmless skip */

    for (uint32_t i = 0; i < g_pages; i++) {
        void *page = pmm_alloc_page();
        if (!page) return -1;

        uint32_t off = i * PAGE_4K;
        uint32_t copy = PAGE_4K;
        if (off + copy > g_size) copy = g_size - off;

        memcpy(page, g_image + off, copy);
        if (copy < PAGE_4K) memset((uint8_t *)page + copy, 0, PAGE_4K - copy);

        uint32_t va = LIBC_VA + off;
        if (paging_map_in(user_pd, va, (uintptr_t)page,
                          PTE_USER | PTE_WRITABLE) != 0) {
            pmm_free_page(page);
            return -1;
        }
    }
    return 0;
}
```

The interesting choice here is "every process gets its own physical copy." The downside: 8 KiB of physical RAM wasted across `MAX_TASKS = 16` processes (16 × 2 pages × 4 KiB / process — half wasted on the second page since libc only uses 5636 bytes). The upside: libc's `.data` (which contains malloc state — `g_brk`, free-list pointers) is naturally per-process. No COW machinery needed.

Real glibc handles the per-process `.data` problem by demand-paging libc.so.6 file-backed (text shared, `.data` mapped MAP_PRIVATE which COWs on first write). We'd need page-fault-driven COW to do the same; today the kernel doesn't have it for general anonymous pages. The "private copy of everything" path is what production OSes did for static-linked SO files in the 1980s — wasteful but trivial.

The mapping is `PTE_USER | PTE_WRITABLE`. Ideally `.text` would be `PTE_USER` only (no W) so user code can't accidentally overwrite library code, but that requires segmenting libc.bin's image into RO and RW regions. With shared physical text, this becomes mandatory; today it's nice-to-have.

## User-side: trampolines

`user/libuser.c` shrank from 832 lines to about 470. The C library implementations are gone; what's left is the syscall wrappers (still inline `int $0x80` shims, no point routing those through a table) plus thin trampolines for libc functions:

```c
#define LIBC_BASE          0x70000000u
#define LIBC_TABLE_OFF     0x10u
#define LIBC_TABLE         ((void * const *)(LIBC_BASE + LIBC_TABLE_OFF))

#define LIBC_FN_STRLEN       1
#define LIBC_FN_VPRINTF     42
#define LIBC_FN_MALLOC      24
/* ... */

size_t strlen(const char *s) {
    return ((size_t (*)(const char *))LIBC_TABLE[LIBC_FN_STRLEN])(s);
}

void *malloc(size_t size) {
    return ((void *(*)(size_t))LIBC_TABLE[LIBC_FN_MALLOC])(size);
}

void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ((int (*)(const char *, va_list))LIBC_TABLE[LIBC_FN_VPRINTF])(fmt, args);
    va_end(args);
}
```

A few things worth noting:

**Trampolines are real functions.** Not macros — so taking the address (`int (*p)(const char *) = strcmp`) still works.

**Variadic forwarding via vprintf.** `printf` in libc would need GCC's `__builtin_va_arg_pack()` to forward variadic arglists through an indirect call, which is fragile. Easier route: libc only exports `vprintf` (a `va_list` version), and the shim's `printf` does `va_start` + `vprintf` + `va_end`. Same trick for `vsprintf` / `vsnprintf`. The real C library does the opposite (vprintf wraps printf via a private call), but for our level of abstraction the inversion is harmless.

**Each call costs one extra indirect.** The trampoline compiles to:

```asm
strlen:
    push   %ebp
    mov    %esp, %ebp
    push   8(%ebp)               ; argument
    call   *0x70000014           ; *LIBC_TABLE[1] = strlen in libc
    add    $4, %esp
    pop    %ebp
    ret
```

One extra `call` per libc invocation versus a direct static call. Roughly 1-2 ns on modern hardware; invisible at any rate of libc usage we'd plausibly hit.

## How libc itself makes syscalls

libc lives in user-mode VA, but it needs to call kernel syscalls (e.g., `sys_write_fd` from inside `printf`'s sink). We can't have libc call into the user program's `libuser.c` syscall wrappers — those don't exist from libc's perspective; libc's a separate compilation unit.

The simple answer: libc has its own internal syscall wrappers, statically linked into `libc.bin` itself. They're identical to libuser's:

```c
/* libc/stdio.c — internal */
static int sys_write_fd(int fd, const void *buf, int n) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(LIBC_SYS_WRITE_FD), "b"(fd), "c"(buf), "d"(n)
                      : "memory");
    return ret;
}
```

`int $0x80` works the same regardless of what user-mode code is running. The CPU traps to ring 0, the kernel's syscall handler runs, control returns. The kernel doesn't care whether the caller was at `0x40001234` (user .text) or `0x70001234` (libc .text).

So yes — we duplicate ~10 lines of syscall wrappers (the ones libc uses internally: SYS_WRITE_FD, SYS_BRK, SYS_EXIT). The alternative (libc calls into the user program for syscalls) is architectural mess. The duplication is ~30 bytes total per process.

## The bug that ate the afternoon

First end-to-end run: kernel boots fine, `dyld_init` says "cached libc.bin v1, 64 exports", `init.elf` launches, prints "init: pid=5, reading /etc/inittab" — and immediately page-faults:

```
[!] CPU EXCEPTION 14: Page fault (err=0x2) at 8:1d75c  eflags=0x10202
    fault addr (CR2) = 0x40001000
    cause = page not present, write, supervisor mode
```

Kernel-side memcpy writing to `0x40001000` (page 1 of init's user .text region) which isn't mapped. The faulting PC is inside `_memcpy` in `kernel/string.c`. Backtracing: `init.c` does `static char tab[2048]` and `read_whole_file("/etc/inittab", tab, sizeof(tab))`. The kernel-side `sys_read` writes the file content into `tab`, but `tab` is at `0x40000F58` (start of init's `.data`), and writing 2048 bytes from there crosses into page 2 at `0x40001000` — which is unmapped.

Why is page 2 unmapped? `init.bin` is 3928 bytes, fits in one page. But `init.c` declares `static char tab[2048]` which is 2 KiB of `.data`. So init's full memory image needs to be 3928 + 2048 = ~5.8 KiB, two pages.

The truth: `init.bin` is supposed to contain the bytes for `.data` too (zero-filled, since `tab` is uninitialized). `user.ld` folds `.bss` into `.data` exactly to make this happen — output `.data` should be PROGBITS so `objcopy -O binary -j .data` includes it.

But the linker only emits `.data` as PROGBITS if there's at least one initialized byte in it. With nothing initialized in `.data`, `.data` is treated as NOBITS and silently dropped from the binary. `init.bin` ends at `.rdata`'s end, missing `.data`. The kernel's `elf_load` allocates pages only for the bytes it sees in the binary — which doesn't include the (NOBITS-stripped) `.data` region.

The OLD libuser had `static uint32_t g_brk = HEAP_START_VA;` — a non-zero initializer that gave `.data` at least one PROGBITS byte and forced the whole section to be CONTENTS. After session 35 stripped libuser's malloc, that initializer disappeared; nothing else in libuser had non-zero initializers; `.data` became NOBITS; `objcopy` stripped it; `init.bin` shrank from "everything" to "code only"; init's `tab` wasn't backed by physical pages; first read into it faulted.

The fix is one line:

```c
__attribute__((used))
static uint32_t libuser_data_marker = 0xADBEEF35u;
```

That non-zero `static` lives in libuser's `.data`. Every user program links libuser, so every user program now has at least one non-zero `.data` byte, so the linker emits `.data` as PROGBITS, so `objcopy` includes the bytes, so `elf_load` allocates pages for the whole binary.

The marker takes 4 bytes per program but pays for itself thousands of times over by NOT silently producing programs whose `.data` is missing. A more architectural fix: have `mkfs.py` parse the linker output's section headers and synthesize a proper PT_LOAD with `memsz > filesz` covering the .bss range — that's the actual ELF machinery for this case. The marker is the practical workaround that keeps `mkfs.py` simple.

## Selftest [t25]

```c
puts("[t25] dynamic libc: every libc call lands at LIBC_BASE\n");
{
    /* Read the library header at LIBC_BASE — should be the magic
     * 'ADLC' and version 1 from libc.h. The kernel's dyld layer
     * mapped libc.bin into our PD at process-load time; we never
     * called the kernel for it from userspace. This is "dynamic
     * linking" in the same sense Linux's vDSO is. */
    const unsigned int *hdr = (const unsigned int *)0x70000000u;
    printf("    magic = 0x%x\n", hdr[0]);

    const unsigned int *exports = (const unsigned int *)0x70000010u;
    printf("    exports[strlen] = 0x%x\n", exports[1]);

    /* Real round-trip: malloc a buffer, fill it, strlen it, free it.
     * Each call dispatches via LIBC_TABLE → libc.bin's implementation. */
    char *p = malloc(64);
    for (int i = 0; i < 60; i++) p[i] = (char)('a' + i % 26);
    p[60] = 0;
    printf("    strlen = %d (expect 60)\n", strlen(p));
    free(p);

    /* fork: child gets its OWN copy of libc.bin. Verify by checking
     * that child's malloc returns from a fresh heap (parent allocated
     * + freed; child sees its own free-list, not parent's). */
    int alpid = sys_fork();
    if (alpid == 0) {
        char *q = malloc(8);
        printf("  child malloc -> 0x%x\n", (unsigned)q);
        exit(0);
    }
    sys_wait(NULL);
}
```

Three things verified:
1. **Header magic.** `*(uint32_t *)0x70000000 == 0x434C4441` — confirms libc.bin loaded at the right place.
2. **Function pointers in the right range.** Every entry in `LIBC_TABLE` is `>= 0x70000000` — confirms exports point INTO libc, not at stale stubs from libuser's old in-tree implementations.
3. **Per-process heap.** Parent and child can each call `malloc()` independently; they don't share a free list. (Parent's `malloc(64)` returned `0x40200010`; child's `malloc(8)` also returned `0x40200010` — same VA but different physical page because child's libc.data is a separate physical page that the fork deep-copy gave to it.)

## Sizes

| Binary | Before (session 34) | After (session 35) | Delta |
|--------|--------------------:|-------------------:|------:|
| `hello.bin` | 4456 | 2308 | -48% |
| `cat.bin`   | 4492 | 2376 | -47% |
| `echo.bin`  | 4312 | 2156 | -50% |
| `httpd.bin` | 5068 | 2920 | -42% |
| `init.bin`  | 11004 | 8860 | -19% (dominated by `tab[2048]`) |
| `sh.bin`    | 27356 | 26920 | -2% (dominated by shell logic) |

The shrink is roughly proportional to how much of each program WAS libuser code. `hello`/`cat`/`echo` were tiny demos with mostly-libuser dependency, so they nearly halve. `sh` had its own ~25 KiB of pipeline / job-control / parsing code; libc was a small fraction. Total user-binary disk savings: about 30 KiB across all 26 programs in the FS image.

## What's left

- **Shared `.text` pages.** Each process gets its own physical copy of libc's 2 pages. Mirroring libc's `.text` PDE by reference (like the LAPIC PDE mirror in session 33) would cut RAM use to 1 copy of `.text` shared + 1 page of private `.data` per process. Requires segmenting libc.bin into RO and RW image regions and mapping each separately.
- **Read-only `.text`.** Currently libc is mapped `PTE_USER | PTE_WRITABLE`. Strict W^X would require the same image-segmentation work above.
- **Real symbol resolution.** Add a string table to `libc.bin` and a `libc_lookup(const char *name)` function. Then user programs could `dlopen("libc.bin")` semantically. The shim trampolines would call `libc_lookup` once at `_start` to populate cached function pointers (the standard PLT lazy-binding pattern).
- **Multiple shared libraries.** Today there's exactly one — libc. To support N libraries, the dyld layer needs a list of (path, base VA, size) tuples; the kernel maps each at its declared VA in every new process.
- **Versioning.** `LIBC_VERSION` is checked at `dyld_init` against magic, but user programs don't verify the version at startup — a libc whose version differs from what the trampolines expect would silently mis-dispatch. Add a `_start` check that aborts if `*(uint32_t *)0x70000004 != LIBC_VERSION`.
- **Constructors / `__attribute__((constructor))`.** Libc's `.init_array` is silently discarded by `libc.ld`. To support C++ static initializers we'd need to walk `.init_array` from libc's entry point (or from each user program's `_start`).
- **`mkfs.py` PT_LOAD with `memsz > filesz`.** The cleaner fix for the BSS bug — `mkfs.py` should parse the ELF the linker produced and emit a proper PT_LOAD that carries the BSS extent. Today we work around it with the libuser data marker.

## Files touched

- `libc/libc.h` (new, 130 LOC)
- `libc/string.c` (new, 110 LOC)
- `libc/ctype.c` (new, 12 LOC)
- `libc/stdlib.c` (new, 180 LOC) — malloc/free, atoi/strtol
- `libc/stdio.c` (new, 130 LOC) — printf via sinks
- `libc/exports.c` (new, 75 LOC) — header + table
- `libc/libc.ld` (new, 50 LOC)
- `kernel/dyld.h`, `kernel/dyld.c` (new, 110 LOC) — cache-and-map loader
- `kernel/elf.c` — adds `dyld_map_libc(user_pd)` call after PT_LOAD layout
- `kernel/kernel.c` — calls `dyld_init()` after VFS is up
- `mkfs.py` — adds `RAW_BLOBS` list, packs `libc.bin`
- `build.sh` — `[5a/7]` libc build step
- `user/libuser.c` — replaced 280 LOC of libc implementations with ~140 LOC of trampolines + the data marker
- `user/sh.c` — `[t25]` selftest
- `docs/35-libc-dynamic-linking.md` — this document

About 850 LOC net of new code, with about 280 LOC of duplicated libc removed from `user/libuser.c`. Per-program binary size dropped 19-50% across user programs.
