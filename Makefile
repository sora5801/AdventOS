#
# AdventOS — top-level Makefile
#
# Targets:
#   make           build os.img
#   make run       boot the OS image in QEMU (graphical + serial→stdio)
#   make run-headless  same but no QEMU window
#   make clean     wipe build artifacts
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

.PHONY: all clean run run-headless run-debug

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

run: os.img
	$(QEMU) -drive format=raw,file=os.img -serial stdio -m 32

run-headless: os.img
	$(QEMU) -drive format=raw,file=os.img -serial stdio -display none -m 32 -no-reboot

run-debug: os.img
	$(QEMU) -drive format=raw,file=os.img -serial stdio -m 32 \
	        -d guest_errors,int -no-reboot -no-shutdown
