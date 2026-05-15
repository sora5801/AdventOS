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
    -mno-stack-arg-probe          # don't emit __chkstk_ms for big stack frames
                                  # (e.g. the 4.5 KiB superblock buffer in
                                  # fs_write_super_inst after FS_MAX_FILES=128)
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

# Session 64: libjson is a static archive of one object that programs
# needing JSON I/O (agentd, ls/cat/wc/date/ps --json) link in. It does
# NOT join libc.bin because we don't want to bump the dynamic-libc
# ABI version for an additive feature most binaries don't touch.
echo "[5c/7] build libjson (static, statically linked into JSON-aware programs)"
mkdir -p libjson/_obj
"$CC" "${USER_CFLAGS[@]}" -c -o libjson/_obj/libjson.o libjson/libjson.c
LIBJSON_OBJS=(libjson/_obj/libjson.o)
echo "        libjson.o = $(stat -c%s libjson/_obj/libjson.o) bytes"

echo "[5/7] build user programs"
"$CC" "${USER_CFLAGS[@]}" -c -o user/_obj/start.o   user/start.S
"$CC" "${USER_CFLAGS[@]}" -c -o user/_obj/libuser.o user/libuser.c

# Basic programs — no libjson, no libcrypto. The smallest binaries.
USER_PROGS=(hello count sh echo httpd ed init
            head tail grep sort uniq tee tr seq kill pwd
            nc wget telnet irc ircd beep usbtest vi id
            dbg dbgtest sandbox kvctl)
for name in "${USER_PROGS[@]}"; do
    "$CC" "${USER_CFLAGS[@]}" -c -o "user/_obj/${name}.o" "user/${name}.c"
    "$LD" -m i386pe -T user/user.ld -o "user/_obj/${name}.elf" \
        user/_obj/start.o "user/_obj/${name}.o" user/_obj/libuser.o
    "$OBJCOPY" -O binary -j .text -j .rdata -j .data \
        "user/_obj/${name}.elf" "user/_obj/${name}.bin"
    # Session 57: emit a symbol-table sidecar for the debugger. nm
    # output is `<8-hex-digits> <type> <name>` — keep only the T/t
    # (text, both global and file-static) rows so the debugger sees
    # function entry points. Stripping the leading mingw underscore
    # is done by the debugger at load time, NOT here, so the file
    # stays a faithful nm dump.
    nm "user/_obj/${name}.elf" \
        | awk '/^[0-9a-fA-F]+ [Tt] / {printf "%s %s\n", $1, $3}' \
        > "user/_obj/${name}.syms"
    echo "        ${name}.bin = $(stat -c%s user/_obj/${name}.bin) bytes"
done

# Session 64: programs with a --json mode or built directly on libjson
# (the agent RPC daemon). Link libjson.o in addition to libuser.
JSON_PROGS=(ls cat wc date ps agentd)
for name in "${JSON_PROGS[@]}"; do
    "$CC" "${USER_CFLAGS[@]}" -c -o "user/_obj/${name}.o" "user/${name}.c"
    "$LD" -m i386pe -T user/user.ld -o "user/_obj/${name}.elf" \
        user/_obj/start.o "user/_obj/${name}.o" user/_obj/libuser.o \
        "${LIBJSON_OBJS[@]}"
    "$OBJCOPY" -O binary -j .text -j .rdata -j .data \
        "user/_obj/${name}.elf" "user/_obj/${name}.bin"
    nm "user/_obj/${name}.elf" \
        | awk '/^[0-9a-fA-F]+ [Tt] / {printf "%s %s\n", $1, $3}' \
        > "user/_obj/${name}.syms"
    echo "        ${name}.bin = $(stat -c%s user/_obj/${name}.bin) bytes"
done

# Crypto-using programs link against the libcrypto static archive
# in addition to libuser. Kept separate from USER_PROGS so the
# basic programs don't pay the libcrypto link cost.
TLS_PROGS=(cryptotest httpsd httpsget login sshd ssh rsatest)
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

fs_lba=384   # MUST match kernel/fs.h::FS_DISK_OFFSET_SECTORS (bumped from 256 in session 71)

# --- Session 72: build-time size assertions ---------------------------
#
# A whole class of silent breakage comes from a binary or data file
# outgrowing a hardcoded buffer / disk-offset constant. Session 71 hit
# this twice in one commit: kernel.bin overran both the bootloader's
# load budget AND the fs_lba cutoff, then the agentd manifest overran
# its 4 KiB sys_read buffer. Each fails at runtime in a confusing way
# (boot hang for the first, "manifest parse failed" for the second).
#
# These checks fail fast at build time with a precise "fix it here"
# pointer. Update the constants below in lockstep with the files they
# mirror.
BOOTLOADER_LOAD_SECTORS=384   # 3 DAP reads of 128 sectors in boot/boot.S
AGENTD_MANIFEST_MAX=8192      # MANIFEST_MAX in user/agentd.c

kernel_size=$(stat -c%s kernel/kernel.bin)
on_disk_budget=$(( (fs_lba - 1) * 512 ))               # kernel ends before FS
load_budget=$(( BOOTLOADER_LOAD_SECTORS * 512 ))       # bootloader reads ≥ kernel
kernel_budget=$on_disk_budget
[ "$load_budget" -lt "$kernel_budget" ] && kernel_budget=$load_budget
if [ "$kernel_size" -gt "$kernel_budget" ]; then
    echo "ERROR: kernel.bin is $kernel_size bytes, max is $kernel_budget" >&2
    if [ "$on_disk_budget" -le "$load_budget" ]; then
        echo "  hit limit: kernel must end before FS at LBA $fs_lba" >&2
        echo "  fix:       bump FS_DISK_OFFSET_SECTORS in kernel/fs.h" >&2
        echo "             AND fs_lba in build.sh (in lockstep)" >&2
    else
        echo "  hit limit: bootloader only loads $BOOTLOADER_LOAD_SECTORS sectors" >&2
        echo "  fix:       add another in-place DAP read in boot/boot.S" >&2
        echo "             and bump BOOTLOADER_LOAD_SECTORS here to match" >&2
    fi
    exit 1
fi

manifest_size=$(stat -c%s fs/etc/agent.tools.json)
if [ "$manifest_size" -gt "$AGENTD_MANIFEST_MAX" ]; then
    echo "ERROR: agent.tools.json is $manifest_size bytes, MANIFEST_MAX is $AGENTD_MANIFEST_MAX" >&2
    echo "  fix: bump MANIFEST_MAX (and SCRATCH_MAX / g_tools_arr) in" >&2
    echo "       user/agentd.c, then bump AGENTD_MANIFEST_MAX here to match." >&2
    exit 1
fi

echo "[6/7] build disk image (boot + kernel)"
cat boot/boot.bin kernel/kernel.bin > os.img

echo "[7/7] mkfs + append AdventFS at LBA $fs_lba"
python mkfs.py
fs_offset=$(( fs_lba * 512 ))
sz=$(stat -c%s os.img)
if [ "$sz" -lt "$fs_offset" ]; then
    pad=$(( fs_offset - sz ))
    dd if=/dev/zero bs=1 count="$pad" >> os.img 2>/dev/null
fi
cat fs.img >> os.img

# Pad up to (fs_lba + 4096) sectors so SYS_FS_WRITE can grow files
# past the initial mkfs payload. Session 46 bumped 1024 → 2048;
# session 54 bumps 2048 → 4096 to match the kernel-side FS bitmap
# cap (kernel/fs.c::FS_BITMAP_BYTES_MAX). Crucial for files like
# /etc/ssh_host_key that sshd creates AT RUNTIME — without enough
# padding here QEMU silently drops writes past EOF, and the file's
# data block reads -1 on the next boot even though the metadata
# entry persists.
final_size=$(( (fs_lba + 4096) * 512 ))
sz=$(stat -c%s os.img)
if [ "$sz" -lt "$final_size" ]; then
    truncate -s "$final_size" os.img
fi

echo "        os.img = $(stat -c%s os.img) bytes  (boot + kernel + FS @ LBA $fs_lba)"

echo "OK. Minimal run (graphical QEMU window + USB keyboard, nothing else):"
echo "    qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32 \\"
echo "        -smp 1 \\"
echo "        -device piix3-usb-uhci,id=usb0 \\"
echo "        -device usb-kbd,bus=usb0.0"
echo ""
echo "  Click into the QEMU window and type. The USB-HID polling task"
echo "  (kernel/usb_hid.c:usb_hid_kbd_task) reads each 8-byte boot-protocol"
echo "  report and injects ASCII into the kbd ring buffer. Output also"
echo "  tees to the host terminal because of -serial stdio."
echo ""
echo "Full run with networking + USB storage (skip if any device errors):"
echo "    qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32 \\"
echo "        -smp 1 \\"
echo "        -netdev user,id=net0,hostfwd=tcp::8080-:80,hostfwd=tcp::7000-:7000,hostfwd=tcp::2222-:2222 \\"
echo "        -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \\"
echo "        -device piix3-usb-uhci,id=usb0 \\"
echo "        -device usb-kbd,bus=usb0.0 \\"
echo "        -drive id=usbfs,file=usbfs.img,format=raw,if=none \\"
echo "        -device usb-storage,drive=usbfs,bus=usb0.0"
echo ""
echo "  Hostfwd maps: 8080 → in-guest httpd, 7000 → agentd JSON-RPC,"
echo "  2222 → in-guest sshd. -smp 1 stays recommended; -smp 2 + agentd"
echo "  hits a TCP-loopback hang. See docs/66-smp-loopback-fix.md."
echo ""
echo "  AC97 audio was dropped from this hint — newer QEMU requires"
echo "  -device AC97,audiodev=snd0 + -audiodev <backend>,id=snd0 with a"
echo "  working backend (sdl/pa/wasapi). The kernel has an AC97 driver"
echo "  but no userspace consumes audio right now, so AC97 is opt-in."
echo ""
echo "Adding '-display none' makes QEMU headless but takes away the only"
echo "working input source on Windows / MSYS2 — neither the COM1 RX IRQ"
echo "(IRQ 4 never fires for piped/non-TTY stdin in this QEMU config) nor"
echo "QMP sendkey routes to PS/2 IRQ 1 without an active display backend."
echo "If you want headless, drive the OS through the agentd JSON-RPC"
echo "endpoint on 127.0.0.1:7000 (curl-friendly) or ssh into port 2222 —"
echo "both still work under -display none."
echo ""
echo "From the host (any mode):  curl http://localhost:8080/"
