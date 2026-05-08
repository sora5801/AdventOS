#!/usr/bin/env bash
# Standalone build script — equivalent to `make`, but doesn't require make.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

CC=${CC:-gcc}
LD=${LD:-ld}
OBJCOPY=${OBJCOPY:-objcopy}

CFLAGS=(
    -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector
    -fno-asynchronous-unwind-tables -fno-unwind-tables
    -fno-builtin -fno-common -fno-omit-frame-pointer
    -mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-3dnow -mno-avx
    -mgeneral-regs-only
    -nostdlib -nostartfiles
    -O2 -std=gnu11
    -Wall -Wextra -Wno-unused-parameter
    -Iinclude -Ikernel
)
ASFLAGS=(-m32 -nostdlib -nostartfiles)

mkdir -p boot kernel

echo "[1/5] compile C sources"
KERNEL_OBJS=()
for src in kernel/*.c; do
    obj="${src%.c}.o"
    "$CC" "${CFLAGS[@]}" -c -o "$obj" "$src"
    KERNEL_OBJS+=("$obj")
done

echo "[2/5] assemble"
for src in kernel/*.S; do
    obj="${src%.S}.o"
    "$CC" "${ASFLAGS[@]}" -c -o "$obj" "$src"
    KERNEL_OBJS+=("$obj")
done
"$CC" "${ASFLAGS[@]}" -c -o boot/boot.o boot/boot.S

echo "[3/5] link bootloader"
"$LD" -m i386pe -T linker_boot.ld -o boot/boot.elf boot/boot.o
"$OBJCOPY" -O binary -j .text boot/boot.elf boot/boot.bin
boot_size=$(stat -c%s boot/boot.bin)
if [ "$boot_size" -ne 512 ]; then
    echo "ERROR: boot sector is $boot_size bytes (expected 512)" >&2
    exit 1
fi
echo "        boot.bin = $boot_size bytes"

echo "[4/5] link kernel"
"$LD" -m i386pe -T linker_kernel.ld -o kernel/kernel.elf "${KERNEL_OBJS[@]}"
"$OBJCOPY" -O binary -j .text -j .rdata -j .data -j .up1 -j .up2 kernel/kernel.elf kernel/kernel.bin
echo "        kernel.bin = $(stat -c%s kernel/kernel.bin) bytes"

echo "[5/6] build disk image (boot + kernel)"
cat boot/boot.bin kernel/kernel.bin > os.img

echo "[6/6] mkfs + append AdventFS at LBA 200"
python mkfs.py
fs_lba=200
fs_offset=$(( fs_lba * 512 ))
sz=$(stat -c%s os.img)
if [ "$sz" -lt "$fs_offset" ]; then
    pad=$(( fs_offset - sz ))
    dd if=/dev/zero bs=1 count="$pad" >> os.img 2>/dev/null
fi
cat fs.img >> os.img
echo "        os.img = $(stat -c%s os.img) bytes  (boot + kernel + FS @ LBA $fs_lba)"

echo "OK. Run with:"
echo "    qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32"
