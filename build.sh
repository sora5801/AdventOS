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

# User programs share the kernel's freestanding constraints, plus
# -fno-zero-initialized-in-bss so uninitialized globals end up in .data
# rather than .bss. That keeps filesz == memsz, so the kernel's ELF
# loader doesn't have to zero-fill anything beyond the file content.
USER_CFLAGS=(
    -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector
    -fno-asynchronous-unwind-tables -fno-unwind-tables
    -fno-builtin -fno-common -fno-zero-initialized-in-bss
    -fno-omit-frame-pointer
    -mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-3dnow -mno-avx
    -mgeneral-regs-only
    -mno-stack-arg-probe          # don't emit __chkstk for big frames
    -nostdlib -nostartfiles
    -O2 -std=gnu11
    -Wall -Wextra -Wno-unused-parameter
    -Iuser
)

mkdir -p boot kernel user/_obj

echo "[1/7] compile kernel C sources"
KERNEL_OBJS=()
for src in kernel/*.c; do
    obj="${src%.c}.o"
    "$CC" "${CFLAGS[@]}" -c -o "$obj" "$src"
    KERNEL_OBJS+=("$obj")
done

echo "[2/7] assemble kernel"
for src in kernel/*.S; do
    obj="${src%.S}.o"
    "$CC" "${ASFLAGS[@]}" -c -o "$obj" "$src"
    KERNEL_OBJS+=("$obj")
done
"$CC" "${ASFLAGS[@]}" -c -o boot/boot.o boot/boot.S

echo "[3/7] link bootloader"
"$LD" -m i386pe -T linker_boot.ld -o boot/boot.elf boot/boot.o
"$OBJCOPY" -O binary -j .text boot/boot.elf boot/boot.bin
boot_size=$(stat -c%s boot/boot.bin)
if [ "$boot_size" -ne 512 ]; then
    echo "ERROR: boot sector is $boot_size bytes (expected 512)" >&2
    exit 1
fi
echo "        boot.bin = $boot_size bytes"

echo "[4/7] link kernel"
"$LD" -m i386pe -T linker_kernel.ld -o kernel/kernel.elf "${KERNEL_OBJS[@]}"
"$OBJCOPY" -O binary -j .text -j .rdata -j .data -j .up1 -j .up2 kernel/kernel.elf kernel/kernel.bin
echo "        kernel.bin = $(stat -c%s kernel/kernel.bin) bytes"

echo "[5a/7] build libc.bin (dynamic library at VA 0x70000000)"
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
echo "        libc.bin = $(stat -c%s libc/_obj/libc.bin) bytes"

echo "[5b/7] build libcrypto (static, statically linked into TLS programs)"
mkdir -p libcrypto/_obj
LIBCRYPTO_OBJS=()
for src in libcrypto/*.c; do
    obj="libcrypto/_obj/$(basename "${src%.c}").o"
    "$CC" "${USER_CFLAGS[@]}" -c -o "$obj" "$src"
    LIBCRYPTO_OBJS+=("$obj")
done
echo "        compiled $(echo ${LIBCRYPTO_OBJS[@]} | wc -w) crypto objects"

echo "[5/7] build user programs"
"$CC" "${USER_CFLAGS[@]}" -c -o user/_obj/start.o   user/start.S
"$CC" "${USER_CFLAGS[@]}" -c -o user/_obj/libuser.o user/libuser.c

USER_PROGS=(hello count sh cat echo httpd ed init
            wc head tail grep sort uniq tee tr seq date kill ls pwd
            nc wget telnet irc ircd gui beep usbtest)
for name in "${USER_PROGS[@]}"; do
    "$CC" "${USER_CFLAGS[@]}" -c -o "user/_obj/${name}.o" "user/${name}.c"
    "$LD" -m i386pe -T user/user.ld -o "user/_obj/${name}.elf" \
        user/_obj/start.o "user/_obj/${name}.o" user/_obj/libuser.o
    "$OBJCOPY" -O binary -j .text -j .rdata -j .data \
        "user/_obj/${name}.elf" "user/_obj/${name}.bin"
    echo "        ${name}.bin = $(stat -c%s user/_obj/${name}.bin) bytes"
done

# Crypto-using programs link against the libcrypto static archive
# in addition to libuser. Kept separate from USER_PROGS so the
# basic programs don't pay the libcrypto link cost.
TLS_PROGS=(cryptotest httpsd httpsget)
for name in "${TLS_PROGS[@]}"; do
    src="user/${name}.c"
    if [ ! -f "$src" ]; then continue; fi
    "$CC" "${USER_CFLAGS[@]}" -c -o "user/_obj/${name}.o" "$src"
    "$LD" -m i386pe -T user/user.ld -o "user/_obj/${name}.elf" \
        user/_obj/start.o "user/_obj/${name}.o" user/_obj/libuser.o \
        "${LIBCRYPTO_OBJS[@]}"
    "$OBJCOPY" -O binary -j .text -j .rdata -j .data \
        "user/_obj/${name}.elf" "user/_obj/${name}.bin"
    echo "        ${name}.bin = $(stat -c%s user/_obj/${name}.bin) bytes"
done

echo "[6/7] build disk image (boot + kernel)"
cat boot/boot.bin kernel/kernel.bin > os.img

echo "[7/7] mkfs + append AdventFS at LBA 200"
python mkfs.py
fs_lba=256   # MUST match kernel/fs.h::FS_DISK_OFFSET_SECTORS
fs_offset=$(( fs_lba * 512 ))
sz=$(stat -c%s os.img)
if [ "$sz" -lt "$fs_offset" ]; then
    pad=$(( fs_offset - sz ))
    dd if=/dev/zero bs=1 count="$pad" >> os.img 2>/dev/null
fi
cat fs.img >> os.img

# Pad up to (fs_lba + 1024) sectors so SYS_FS_WRITE can grow files
# past the initial mkfs payload. The kernel's fs.c caps the FS area
# at 1024 sectors past the superblock; QEMU's raw drive treats the
# file's size as the disk's size, so any write beyond it is silently
# discarded — that's what bit us in session 19 before this padding.
final_size=$(( (fs_lba + 1024) * 512 ))
sz=$(stat -c%s os.img)
if [ "$sz" -lt "$final_size" ]; then
    truncate -s "$final_size" os.img
fi

echo "        os.img = $(stat -c%s os.img) bytes  (boot + kernel + FS @ LBA $fs_lba)"

echo "OK. Run with:"
echo "    qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32 \\"
echo "        -smp 2 \\"
echo "        -netdev user,id=net0,hostfwd=tcp::8080-:80 \\"
echo "        -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \\"
echo "        -device AC97 -audiodev sdl,id=snd0 \\"
echo "        -device piix3-usb-uhci,id=usb0 \\"
echo "        -device usb-kbd,bus=usb0.0 \\"
echo "        -drive id=usbfs,file=usbfs.img,format=raw,if=none \\"
echo "        -device usb-storage,drive=usbfs,bus=usb0.0"
echo "Then from the host:  curl http://localhost:8080/"
