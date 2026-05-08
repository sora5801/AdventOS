# Session 1 — Bootloader + 32-bit kernel from scratch

**Goal:** A real bootable OS in C on Windows. No GRUB, no QEMU `-kernel`. The first byte the BIOS jumps to has to be ours.

## Architecture

```
BIOS  →  MBR (boot/boot.S, 0x7C00, real mode)
              │  int 13h ah=42h to LBA-read kernel
              │  enable A20, GDT, CR0.PE=1
              ▼
         pm_entry (32-bit)
              │  reload data segs, ESP=0x90000
              ▼
         kernel/entry.S at 0x10000
              │  zero BSS, install own stack
              ▼
         kmain → drivers, IDT, shell
```

Kernel is a flat binary loaded at physical 0x10000. No multiboot, no relocations. The bootloader's far-jump targets a literal `0x10000` and trusts that the linker put the entry stub at the start of the binary.

## Toolchain reality check

This was the first wall. We're on Windows 11 with MSYS2 UCRT64:

| Tool | Status | Implication |
|------|--------|-------------|
| `gcc` 15.2 | Available, but produces **PE/COFF** | Can't link as ELF directly |
| `ld` | Emulations: `i386pe`, `i386pep` only | No `i386elf` emulation |
| `objcopy` | Output targets include `binary`, `elf32-i386` | Can convert PE → flat |
| `nasm` | **Missing** | Use GNU `as` (built into gcc) |
| `qemu-system-i386` | Available | `-drive format=raw,file=...` |
| `make` | **Missing** (mingw32-make exists) | Wrote `build.sh` instead |

The first probe build failed with `cannot perform PE operations on non PE output file 'test.bin'` when trying `ld --oformat=binary` directly with PE inputs. The working pattern is two-step:

```sh
ld -m i386pe -T linker.ld -o foo.elf foo.o      # PE intermediate
objcopy -O binary -j .text -j .rdata ... foo.bin # strip headers, raw bytes
```

`-O binary` is in objcopy's supported targets even though `i386elf` isn't a valid `ld` emulation. The "section below image base" warnings ld emits (PE expects sections above the default image base of 0x400000; we're at 0x10000) are noise — the binary is correct.

## The first triple fault

After the toolchain probe worked, the real kernel triple-faulted at `pmm_init`. QEMU's interrupt log:

```
EIP=000119ca  ... env->regs[R_EAX]=000007ff
Servicing INT=0x06    ← invalid opcode
... no handler (IDT wasn't loaded yet)
INT=0x0d              ← GP from looking up #UD in empty IDT
INT=0x08              ← double fault
Triple fault
```

EIP 0x119ca was inside `_memset`. The instruction was:

```
movd 0xc(%ebp), %xmm0
punpcklbw %xmm0, %xmm0
pshufd $0x0, %xmm0, %xmm0
```

GCC auto-vectorized `memset` with SSE2. SSE wasn't enabled in CR4 → `#UD`. No IDT yet → escalation cascade.

**Fix:**
```
-mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-3dnow -mno-avx -mgeneral-regs-only
```

`-mgeneral-regs-only` is the most important — it tells GCC to emit only general-register code in any function in the file, including library helpers like `memset`. Without it, every `-O2` kernel will eventually triple-fault on the first SSE-ish operation.

## ISR convention

Stub macro pattern, used throughout:

```asm
.macro ISR_NOERR num
    .global _isr\num
_isr\num:
    cli
    push    $0          /* fake error code */
    push    $\num       /* int number       */
    jmp     isr_common_stub
.endm
```

`isr_common_stub` does `pusha`, saves DS, switches DS/ES/FS/GS to kernel data, pushes ESP as the C handler arg, calls `_isr_handler`, restores everything, `add $8, %esp` to skip int_no + err_code, `iret`.

The C side gets a `struct registers *` matching the stack layout exactly:

```c
struct registers {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp_orig, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
};
```

`useresp`/`ss` are only valid when the interrupt came from ring 3 (privilege change pushes them); for ring 0 they're not on the stack and shouldn't be read.

## The serial trap

QEMU `-display none -serial stdio` was the verification harness from minute one. First versions of the keyboard driver also accepted serial input as a fallback (`if (serial_has_data()) ...`). Polling worked for slow input but **dropped bytes when the host shoved a whole line in at once**. Example:

- Sent: `echo hello AdventOS this is a long line\nuptime\n`
- Got:  `echo hello AdventOS this is  long euptime\n`

Bytes vanished from the middle. The 16550 UART has a 16-byte FIFO. With 14-byte threshold and only polling, bursts overflowed before the polling loop could drain.

**Fix:** real RX IRQ. `serial_install_irq` registers IRQ 4, enables RDA interrupt in IER, and the handler drains the FIFO into a 256-byte ring buffer. After this, `printf '...long string...'` came through intact.

## Linker symbol mangling on mingw32

mingw32-i386 prepends `_` to all C symbols. Linker scripts aren't C, so they don't add the prefix automatically. To make linker-defined symbols match C `extern` declarations:

| Linker script | C declaration | Resolves to |
|---|---|---|
| `_kernel_start = .;` | `extern uint8_t kernel_start;` | `_kernel_start` ✓ |
| `kernel_start = .;` | `extern uint8_t kernel_start;` | mismatch (`_kernel_start` not found) |

The convention used everywhere: linker scripts define `_name`, C declares `name`, the compiler's automatic underscore makes them match.

## Section name truncation (early warning)

PE/COFF section names cap at 8 characters when stored in the section table. Names longer than 8 get silently truncated. We hit this later (`.user_code` → `.user_co`) and now follow the rule: **all custom section names ≤ 8 chars**. This becomes a recurring constraint.

## Files added

| File | Role |
|---|---|
| `boot/boot.S` | 512-byte MBR: real-mode setup, LBA disk read, A20, GDT, far-jump to 32-bit |
| `kernel/entry.S` | 32-bit entry: BSS clear, stack switch, call kmain |
| `kernel/gdt.{c,h,_load.S}` | 5-entry GDT (null, k-code, k-data, u-code, u-data) |
| `kernel/idt.{c,h}` | 256-entry IDT, 32 CPU exception gates + 16 IRQ gates |
| `kernel/isr.{c,h}` + `isr_stubs.S` | Per-vector stubs + common dispatch |
| `kernel/pic.{c,h}` | 8259 remap to 0x20/0x28, EOI helpers, mask/unmask |
| `kernel/pit.{c,h}` | 100 Hz timer + ticks counter + pit_sleep |
| `kernel/keyboard.{c,h}` | PS/2 scancode-set-1 → ASCII with shift/caps |
| `kernel/serial.{c,h}` | COM1 driver: TX poll + RX IRQ + 256-byte ring buffer |
| `kernel/vga.{c,h}` | 80×25 text mode with scroll, color, hardware cursor |
| `kernel/kprintf.{c,h}` | `%d %u %x %s %c %p` formatter; later `%-Ns` |
| `kernel/string.{c,h}` | Freestanding `mem*` and `str*` |
| `kernel/shell.{c,h}` | Interactive command loop |
| `kernel/kernel.c` | `kmain` — bring up everything in order |
| `linker_boot.ld`, `linker_kernel.ld` | Layout for boot + kernel binaries |
| `Makefile`, `build.sh` | Build pipeline |

## Design decisions worth defending

**Custom MBR vs GRUB.** A multiboot-compliant kernel + GRUB image would handle disk geometry, ELF loading, memory map (replaces our E820 work in session 2) — but it would hide the real-mode → protected-mode transition that's pedagogically the entire point of session 1. We picked custom.

**Flat binary kernel at 0x10000.** No metadata. The bootloader doesn't need to parse anything; it just reads N sectors to a fixed address and jumps. Tradeoff: no symbols at runtime (we don't have a `panic()` that prints function names), no relocations.

**ld emulation `i386pe` + objcopy → binary.** Forced by toolchain. The "right" alternative would be a real `i386-elf-gcc` cross-compiler, but building one on Windows is a yak. Two-step produces identical machine code.

**Serial RX as IRQ from day one.** Adds complexity (IDT entry, ring buffer, EOI flow) but pays for itself the moment automated testing involves bursts of input.

## Deferred (handled later)

- No memory map awareness (session 2)
- No paging / processes (session 3, 5)
- No multitasking (session 4)
- No filesystem or ELF (session 8)
- No userspace (session 5)

## Pitfalls worth remembering

1. **`-mgeneral-regs-only` is non-negotiable** for any freestanding x86 kernel built with modern GCC. The auto-vectorizer otherwise emits SSE in the most innocuous code.
2. **Linker-script symbols need leading `_` on mingw32** to match C externs after compiler prefixing.
3. **PE section names ≤ 8 chars** or you'll find tools silently truncating them.
4. **`-display none -serial stdio` requires real RX IRQ on the guest side**, not polling — host pipes deliver bursts that overflow the UART FIFO.
5. **PIT IRQ EOI ordering** — not yet a problem here (we only get one IRQ in flight), but became one in session 4. EOI before any handler that might never return.
