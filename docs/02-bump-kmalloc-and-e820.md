# Session 2 — Bump kmalloc + BIOS E820 + meminfo

**Goal:** Give the kernel a heap and have it actually know how much RAM the machine has, instead of hard-coding "32 MB".

## The problem with not having E820

Pre-session-2, anything that wanted to allocate memory had to guess. Hard-coding "use 0x100000 to 0x2000000" works in QEMU `-m 32` and breaks the moment you change the memory size. There's no graceful failure — you just trample reserved regions or run off the end of physical RAM.

The BIOS knows the real layout. INT 15h, AX=E820h enumerates it. The catch: this call is **only available in real mode**, so it has to happen in the bootloader, before we go into protected mode. The result has to survive the mode switch.

## How E820 works

```
Inputs:
  EAX = 0xE820
  EBX = continuation (0 to start; BIOS updates each call; 0 = done)
  ECX = buffer size (must be >= 20)
  EDX = 'SMAP' = 0x534D4150
  ES:DI = pointer to a 24-byte buffer

Outputs:
  CF = 0 on success, 1 on error/end
  EAX = 'SMAP' if successful
  EBX = updated continuation
  ECX = bytes actually written (20 for older BIOSes, 24 for ACPI 3.0+)
  Buffer filled with:
    +0  uint64  base
    +8  uint64  length
    +16 uint32  type   (1=usable, 2=reserved, 3=ACPI reclaim, 4=ACPI NVS, 5=bad)
    +20 uint32  attrs  (only on ACPI 3.0; bit 0 = "valid")
```

Trick: write `1` to the attrs field at offset 20 **before** every call. ACPI 3.0 BIOSes either replace it with their own value or leave our `1` in place; older 20-byte BIOSes don't touch it but our `1` makes the kernel treat the entry as valid.

## Where to stash it

The bootloader and the kernel run in totally different worlds (real vs protected mode, different address spaces, different code). They communicate via a fixed-address scratch region. We picked physical 0x8000 because:

- Above the bootloader's stack (0x7C00) and the bootloader image itself (0x7C00–0x7E00).
- Below where the kernel loads (0x10000).
- Inside the BIOS-usable low memory (0x0–0x9FBFF).
- A whole page so we don't worry about overflow.

Layout:

```
0x8000  uint32_t  count
0x8004  e820_entry entries[count]   (24 bytes each, up to 50)
```

The kernel's `memmap_init()` reads back the count and stashes a pointer to the entries array. Nothing copies the data — it stays at 0x8000 forever. Worth noting for session 3, where we have to mark this page as "used" in the PMM bitmap.

## The 16-bit-mode 32-bit-register dance

The bootloader is `.code16` but uses 32-bit registers (EBX continuation, EDX `'SMAP'`, EAX 0xE820). GAS handles the operand-size override automatically:

```asm
xor     %ax, %ax
mov     %ax, %es              # ES = 0
mov     $0x8004, %di          # DI = 0x8004 (16-bit address)
xor     %ebx, %ebx            # 32-bit reg used in 16-bit mode (0x66 prefix)
xor     %ebp, %ebp            # entry counter
e820_loop:
    movl    $1, %es:20(%di)   # ACPI 3.0 valid bit
    mov     $24, %ecx
    mov     $0x534D4150, %edx
    mov     $0xE820, %eax
    int     $0x15
    jc      e820_done
    cmp     $0x534D4150, %eax
    jne     e820_done
    movl    %es:8(%di), %eax  # length low
    or      %es:12(%di), %eax # OR with length high
    jz      e820_skip         # zero-length entry, skip
    inc     %ebp
    add     $24, %di
e820_skip:
    test    %ebx, %ebx
    jz      e820_done
    cmp     $50, %ebp
    jl      e820_loop
e820_done:
    movl    %ebp, 0x8000      # store count at fixed location
```

`movl %ebp, 0x8000` is a 32-bit write to absolute address (in DS:0x8000, with DS=0). GAS emits this with the 0x66 operand-size prefix.

This grew the bootloader by ~60 bytes; still well under the 510-byte cap.

## Bump allocator

The kmalloc API at this stage:

```c
void  kmalloc_init(uintptr_t heap_start, uintptr_t heap_end);
void *kmalloc(size_t size);
void *kzalloc(size_t size);

uintptr_t kmalloc_heap_start(void);
uintptr_t kmalloc_heap_brk(void);     // current bump pointer
uintptr_t kmalloc_heap_end(void);
size_t    kmalloc_used(void);
size_t    kmalloc_free(void);
size_t    kmalloc_total(void);
```

Implementation: a `g_brk` pointer that bumps on every allocation. 16-byte alignment by default. No `kfree` (that comes in session 6 with the free-list rewrite).

```c
void *kmalloc(size_t size) {
    size_t aligned = align_up(size, KMALLOC_ALIGN);
    uintptr_t new_brk = g_brk + aligned;
    if (new_brk < g_brk || new_brk > g_end) return NULL;
    void *p = (void *)g_brk;
    g_brk = new_brk;
    return p;
}
```

The overflow check (`new_brk < g_brk`) matters because `aligned` could be huge enough to wrap around the 32-bit pointer.

Where does the heap live? `kmain` calls `memmap_largest_usable_above(0x100000, &base)` — find the biggest USABLE region whose end is above 1 MiB, return its base and size. With QEMU `-m 32` that's 0x100000..0x1FE0000. `kmalloc_init` gets called with those bounds. (Session 3 changes this to allocate from PMM instead.)

## 64-bit math without libgcc

We're freestanding and don't link libgcc. Anything that requires `__udivdi3`, `__divdi3`, `__umoddi3`, `__moddi3`, `__lshrdi3` for 64-bit operations on a variable amount, or `__muldi3`, breaks the link.

GCC's actual emission rules for `uint64_t`:
- `+`, `-`, comparisons → inline `addl`/`adcl`/`cmpl`/`sbbl`. Safe.
- `>>` or `<<` by **constant** amount → inline `shrdl`/`shrl`. Safe.
- `>>` or `<<` by **variable** amount → libgcc `__lshrdi3` etc. **Not safe.**
- `*` (full 64×64) → libgcc `__muldi3`. **Not safe.**
- `/`, `%` → libgcc. **Not safe.**

The `meminfo` command sums usable RAM from E820 in `uint64_t`, then prints MB and KB. `total >> 20` and `total >> 10` are constant shifts — fine.

```c
uint64_t usable = memmap_total_usable();
uint32_t mb = (uint32_t)(usable >> 20);
uint32_t kb = (uint32_t)(usable >> 10);
kprintf("Total usable RAM: %u MB (%u KB)\n", mb, kb);
```

Splitting 64-bit values into hex display also works without libgcc:

```c
kprintf("0x%08x%08x", (unsigned)(x >> 32), (unsigned)x);
```

## What `meminfo` showed

With QEMU `-m 32`:

```
BIOS E820 memory map (6 entries):
  base               length             type
  0x0000000000000000 0x000000000009fc00 usable
  0x000000000009fc00 0x0000000000000400 reserved
  0x00000000000f0000 0x0000000000010000 reserved
  0x0000000000100000 0x0000000001ee0000 usable
  0x0000000001fe0000 0x0000000000020000 reserved
  0x00000000fffc0000 0x0000000000040000 reserved

Total usable RAM: 31 MB (32255 KB)
```

The 0xFFFC0000 entry is the BIOS flash-mapped window — it's "reserved" RAM that doesn't actually exist in the lower 32 MB. This caused a real bug in session 3.

## Files added

| File | Role |
|---|---|
| `kernel/memmap.{c,h}` | Reads the BIOS dump from 0x8000, exposes count/iterator/total/largest helpers |
| `kernel/kmalloc.{c,h}` | Bump allocator (replaced in session 6) |
| `boot/boot.S` | E820 query loop added before mode switch |

## Design decisions

**Fixed scratch address (0x8000).** Could've used a Multiboot-style information-pointer-in-register approach — bootloader leaves a pointer in EBX or wherever, kernel reads it. Fixed address is simpler and the memory it consumes (one page) is small.

**Bump only, no free.** kmalloc usage at this point is exclusively for things allocated once at boot (kernel stacks for tasks, page tables once paging arrives). Adding a free-list would be premature. Session 6 makes the case for kfree.

**Cap on E820 entries (50).** A real BIOS could return more, but unlikely in practice. The cap is what stops a runaway loop if the BIOS misbehaves.

**Hard 1 MiB minimum for the heap.** `memmap_largest_usable_above(0x100000, ...)` skips low memory entirely. Below 1 MiB has BIOS data, video memory, ROM, the bootloader itself — too many landmines to put a heap in.

## Deferred

- Real allocator with free / coalescing → session 6.
- Awareness of memory at the page level (PMM) → session 3.

## Pitfalls

1. **The 24-byte ACPI-3.0-attrs field** must be initialized to 1 **before** the BIOS call, not relied upon to be set after. Older BIOSes don't write it.
2. **Don't trust E820's reserved-region high addresses for "total RAM" calculations.** The 0xFFFC0000 BIOS-flash entry will inflate any total that doesn't filter by type=USABLE. Bit us in session 3.
3. **64-bit math is a libgcc minefield in a freestanding kernel.** Stick to constants for shifts, avoid divide/modulo entirely. If you genuinely need 64-bit divide, hand-roll it.
4. **EBX as continuation across iterations** must not be clobbered between calls. The BIOS preserves it; just don't touch it yourself.
