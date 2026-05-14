#
# AdventOS — top-level Makefile
#
# Targets:
#   make           build os.img
#   make run       boot the OS image in QEMU (graphical window + USB keyboard
#                  + rtl8139 networking + serial tee to stdio)
#   make run-bare  bare QEMU — no USB keyboard, no network. Useful for
#                  isolating driver init failures; the PS/2 keyboard built
#                  into i440fx still works.
#   make run-debug --d guest_errors,int + halt-on-shutdown. Same input
#                  setup as `make run`.
#   make clean     wipe build artifacts
#
# `make run-headless` was removed. On Windows / MSYS2, -display none has
# no working input source — neither the COM1 RX IRQ (IRQ 4 never fires
# for piped/non-TTY stdin in QEMU's i440fx + i8259 config) nor QMP
# `sendkey` routes to PS/2 IRQ 1 without an active display backend. If
# you want headless, drive the OS through the agentd JSON-RPC endpoint
# on 127.0.0.1:7000 or ssh into port 2222.
#

SHELL := bash

# --- Tools (all from MSYS2 ucrt64) ---------------------------------------
CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy
QEMU    ?= qemu-system-i386

# --- Flags ----------------------------------------------------------------

# Freestanding C: no libc, no stack canary, no SEH/eh_frame noise.
CFLAGS := -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -fno-builtin -fno-common -fno-omit-frame-pointer \
          -mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-3dnow -mno-avx \
          -mgeneral-regs-only \
          -nostdlib -nostartfiles \
          -O2 -std=gnu11 \
          -Wall -Wextra -Wno-unused-parameter \
          -Iinclude -Ikernel

ASFLAGS := -m32 -nostdlib -nostartfiles

LDFLAGS_BOOT   := -m i386pe -T linker_boot.ld --no-warn-rwx-segments 2>/dev/null
LDFLAGS_KERNEL := -m i386pe -T linker_kernel.ld

# --- Sources --------------------------------------------------------------

KERNEL_C_SRCS := $(wildcard kernel/*.c)
KERNEL_S_SRCS := $(wildcard kernel/*.S)
KERNEL_OBJS   := $(KERNEL_C_SRCS:.c=.o) $(KERNEL_S_SRCS:.S=.o)

BOOT_OBJ := boot/boot.o

# --- Rules ----------------------------------------------------------------

.PHONY: all clean run run-bare run-debug

all: os.img

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -c -o $@ $<

boot/boot.elf: $(BOOT_OBJ) linker_boot.ld
	$(LD) -m i386pe -T linker_boot.ld -o $@ $(BOOT_OBJ)

boot/boot.bin: boot/boot.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@sz=$$(stat -c%s $@); \
	echo "  BOOT  $$sz bytes"; \
	if [ $$sz -ne 512 ]; then \
	    echo "ERROR: boot sector must be 512 bytes, got $$sz"; \
	    exit 1; \
	fi

kernel/kernel.elf: $(KERNEL_OBJS) linker_kernel.ld
	$(LD) -m i386pe -T linker_kernel.ld -o $@ $(KERNEL_OBJS)

kernel/kernel.bin: kernel/kernel.elf
	$(OBJCOPY) -O binary -j .text -j .rdata -j .data -j .up1 -j .up2 $< $@
	@sz=$$(stat -c%s $@); echo "  KERNEL  $$sz bytes"

os.img: boot/boot.bin kernel/kernel.bin
	@cat boot/boot.bin kernel/kernel.bin > $@
	@sz=$$(stat -c%s $@); \
	target=65536; \
	if [ $$sz -gt $$target ]; then target=$$(( ((sz + 511) / 512) * 512 )); fi; \
	pad=$$(( target - sz )); \
	if [ $$pad -gt 0 ]; then \
	    dd if=/dev/zero bs=1 count=$$pad >> $@ 2>/dev/null; \
	fi; \
	echo "  IMG   $$(stat -c%s $@) bytes  (boot=$$(stat -c%s boot/boot.bin) kernel=$$(stat -c%s kernel/kernel.bin))"

clean:
	@rm -f boot/*.o boot/*.elf boot/*.bin
	@rm -f kernel/*.o kernel/*.elf kernel/*.bin
	@rm -f os.img
	@echo "  CLEAN"

# Recommended run. Graphical SDL/GTK window so typing actually reaches
# the guest; `-serial stdio` tees kernel kprintf to the host terminal
# so you can see the boot log. -smp 1 because -smp 2 + agentd in
# inittab hits a TCP-loopback race (docs/66-smp-loopback-fix.md).
#
# usb-kbd is the input path: the kernel's USB-HID polling task
# (kernel/usb_hid.c:usb_hid_kbd_task) reads 8-byte boot-protocol
# reports every ~50ms and injects ASCII into the kbd ring buffer.
# Even without usb-kbd the PS/2 keyboard built into i440fx (i8042 on
# IRQ 1) still delivers — useful as a fallback if USB enumeration
# fails for any reason.
#
# rtl8139 networking is included so the agent endpoints (port 7000
# JSON-RPC and port 2222 sshd) work from the host; port 8080 forwards
# to the in-guest httpd.
run: os.img
	$(QEMU) -drive format=raw,file=os.img -serial stdio -m 32 \
	        -smp 1 \
	        -netdev user,id=net0,hostfwd=tcp::8080-:80,hostfwd=tcp::7000-:7000,hostfwd=tcp::2222-:2222 \
	        -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
	        -device piix3-usb-uhci,id=usb0 \
	        -device usb-kbd,bus=usb0.0

# Bare boot — graphical window, no USB device, no network. Falls back
# to the PS/2 keyboard. Good for isolating driver init bugs.
run-bare: os.img
	$(QEMU) -drive format=raw,file=os.img -serial stdio -m 32 -smp 1

run-debug: os.img
	$(QEMU) -drive format=raw,file=os.img -serial stdio -m 32 -smp 1 \
	        -device piix3-usb-uhci,id=usb0 -device usb-kbd,bus=usb0.0 \
	        -d guest_errors,int -no-reboot -no-shutdown
