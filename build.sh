#!/usr/bin/env bash
# Standalone build script — equivalent to `make`, but doesn't require make.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

CC=${CC:-gcc}
LD=${LD:-ld}
OBJCOPY=${OBJCOPY:-objcopy}

# --- Platform / target detection ----------------------------------------
#
# AdventOS originally targeted PE/COFF (MSYS2 ucrt64 on Windows hosts).
# To support Linux / WSL hosts too, we now auto-detect and switch between
# PE and ELF output for the freestanding kernel + user binaries.  Either
# way the FINAL artifact (kernel.bin, user/*.bin) is the same flat blob
# produced by `objcopy -O binary` — the difference is just the linker
# emulation (-m "$LD_EMUL" vs -m elf_i386) and one compiler flag.
#
# Key wrinkle: under PE/COFF, gcc prepends an underscore to every public
# symbol (`_kmain`, `_bss_start`, ...).  Our hand-written assembly stubs
# (entry.S, isr_stubs.S, task_switch.S, ap_trampoline.S) are written in
# that convention.  Native i386-elf gcc does NOT prepend the underscore,
# so the symbols mismatch — `call _kmain` in entry.S would not resolve.
# `-fleading-underscore` forces gcc to add the underscore on ELF too,
# keeping the assembly portable between targets.
#
# Override either by exporting TARGET_FORMAT=elf|pe before running.
if [ -z "${TARGET_FORMAT:-}" ]; then
    case "$(uname -s)" in
        Linux*)            TARGET_FORMAT=elf ;;
        MSYS*|MINGW*|CYGWIN*) TARGET_FORMAT=pe ;;
        *)                 TARGET_FORMAT=elf ;;
    esac
fi

case "$TARGET_FORMAT" in
    elf)
        LD_EMUL=elf_i386
        # -fleading-underscore: emit `_kmain` etc. so the existing
        # PE-style hand-written asm symbols resolve under ELF.
        # -mno-stack-arg-probe: on Linux this is a no-op (no __chkstk
        # is emitted by default), but harmless and keeps PE/ELF flag
        # parity in case someone copies a single line.
        TARGET_CC_EXTRA=(-fleading-underscore)
        ;;
    pe)
        LD_EMUL=i386pe
        TARGET_CC_EXTRA=()
        ;;
    *)
        echo "ERROR: unknown TARGET_FORMAT=$TARGET_FORMAT (want elf or pe)" >&2
        exit 1
        ;;
esac
echo "[note] target=$TARGET_FORMAT (ld -m $LD_EMUL)"

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
    # Session 112 — function-level sectioning so --gc-sections at
    # link time drops unreferenced functions (dead -Wunused-function
    # chaff).  Keeps the kernel image under the VGA-RAM 0xA0000
    # ceiling after the WM syscall additions.  WITHOUT -fdata-sections
    # because the PE/COFF backend folds .bss* into .data under
    # --gc-sections, which would blow up kernel.bin (BSS doesn't take
    # disk space; promoting it to .data does).  Function sections are
    # enough on their own.
    -ffunction-sections
    -Wall -Wextra -Wno-unused-parameter
    -Iinclude -Ikernel
    "${TARGET_CC_EXTRA[@]}"
)
ASFLAGS=(-m32 -nostdlib -nostartfiles "${TARGET_CC_EXTRA[@]}")

# Session 80 — opt-in SMP tracing. Set SMP_TRACE=1 in the environment
# (e.g. `SMP_TRACE=1 bash build.sh`) and the kernel's lock / scheduler
# / sock / tcp hot paths emit `[smp] cpuN <event>` lines to serial.
# Default build is unaffected — the macro in kernel/smp_trace.h
# expands to `((void)0)` without -DSMP_TRACE. Used to root-cause the
# four `-smp 2` scheduler/locking bugs documented in
# docs/68-smp2-deadlock-fixes.md.
if [ "${SMP_TRACE:-0}" = "1" ]; then
    echo "[note] SMP_TRACE=1 — kernel will emit [smp] trace lines"
    CFLAGS+=(-DSMP_TRACE)
fi

# Session 82 — opt-in USB-HID per-keystroke trace. Default OFF because
# the per-key serial print used to interleave with the shell's char
# echo, corrupting the agent-training-surface "prompt + echo + output"
# contract. USB driver devs can re-enable with `USB_HID_TRACE=1
# bash build.sh` to see scancode / usage-id diagnostics.
if [ "${USB_HID_TRACE:-0}" = "1" ]; then
    echo "[note] USB_HID_TRACE=1 — kernel will emit [usb-hid] per-key lines"
    CFLAGS+=(-DUSB_HID_TRACE)
fi

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
    # Session 132 — function-level sectioning + --gc-sections drops
    # unreferenced libuser / libgfx / libwm code per binary.  The
    # libuser indirections through LIBC_TABLE (printf → vprintf,
    # malloc → libc.bin malloc, etc.) hide their target symbols
    # from the linker's reachability graph; those wrappers are
    # tagged __attribute__((used)) in libuser.c so the compiler
    # emits them and the linker keeps them.
    -ffunction-sections
    -Wall -Wextra -Wno-unused-parameter
    -Iuser
    "${TARGET_CC_EXTRA[@]}"
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
"$LD" -m "$LD_EMUL" -T linker_boot.ld -o boot/boot.elf boot/boot.o
"$OBJCOPY" -O binary -j .text boot/boot.elf boot/boot.bin
boot_size=$(stat -c%s boot/boot.bin)
if [ "$boot_size" -ne 512 ]; then
    echo "ERROR: boot sector is $boot_size bytes (expected 512)" >&2
    exit 1
fi
echo "        boot.bin = $boot_size bytes"

echo "[4/7] link kernel"
"$LD" -m "$LD_EMUL" -T linker_kernel.ld --gc-sections -o kernel/kernel.elf "${KERNEL_OBJS[@]}"
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
"$LD" -m "$LD_EMUL" -T libc/libc.ld -o libc/_obj/libc.elf "${LIBC_OBJS[@]}"
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

# Session 108 — libgfx — Path C software drawing library. Same shape
# as libjson: one source file, one object, linked into graphical
# programs (gfx, future wm). Smaller than libjson; mostly the 8x8
# font + Bresenham + rect fill.
echo "[5d/7] build libgfx (static, statically linked into graphics programs)"
mkdir -p libgfx/_obj
"$CC" "${USER_CFLAGS[@]}" -c -o libgfx/_obj/libgfx.o libgfx/libgfx.c
LIBGFX_OBJS=(libgfx/_obj/libgfx.o)
echo "        libgfx.o = $(stat -c%s libgfx/_obj/libgfx.o) bytes"

# Session 112 — libwm — Path C client wrapper for SYS_WM_*. Tiny
# (one source file, ~60 lines). Linked into WM client programs
# (currently just `wmhello`); not linked into wmd itself (wmd talks
# to the kernel directly via sys_wm_bind/poll).
echo "[5e/7] build libwm (static, linked into WM client programs)"
mkdir -p libwm/_obj
"$CC" "${USER_CFLAGS[@]}" -c -o libwm/_obj/libwm.o libwm/libwm.c
LIBWM_OBJS=(libwm/_obj/libwm.o)
echo "        libwm.o = $(stat -c%s libwm/_obj/libwm.o) bytes"

# Session 133 (Path B Phase 1 of the tcc port): build a HOST copy of
# tcc.  This is a Phase 1 stepping-stone — Phase 2 cross-compiles tcc
# with USER_CFLAGS for tcc.elf running INSIDE AdventOS, and that's
# a multi-session libuser-expansion project. The host build verifies
# the source we vendored is internally consistent and useful as a
# reference. See tcc/README.AdventOS for the integration plan.
#
# Skipped silently if tcc/ isn't present (don't break the build for
# folks who don't want the extra 1.8MB source tree).
if [ -d tcc ] && [ -f tcc/tcc.c ]; then
    echo "[5f/7] build host tcc (Phase 1 — i386 ELF cross-compiler)"
    # Generate tccdefs_.h from include/tccdefs.h via the c2str helper.
    # Only re-run when the input changes.
    if [ ! -f tcc/tccdefs_.h ] || [ tcc/include/tccdefs.h -nt tcc/tccdefs_.h ]; then
        "$CC" -DC2STR tcc/conftest.c -o tcc/c2str.exe
        tcc/c2str.exe tcc/include/tccdefs.h tcc/tccdefs_.h >/dev/null
    fi
    # One-source build: tcc.c #includes libtcc.c which transitively
    # includes every other source. ONE_SOURCE + TCC_TARGET_I386 are
    # the only -D flags strictly required.
    "$CC" -DONE_SOURCE -DTCC_TARGET_I386 -w -O2 \
        -Itcc -o tcc/tcc.exe tcc/tcc.c
    echo "        tcc.exe = $(stat -c%s tcc/tcc.exe) bytes (i386 cross-compiler)"
fi

echo "[5/7] build user programs"
"$CC" "${USER_CFLAGS[@]}" -c -o user/_obj/start.o   user/start.S
"$CC" "${USER_CFLAGS[@]}" -c -o user/_obj/libuser.o user/libuser.c

# Session 78: libagent — TCP+JSON-RPC client for the in-guest selftests.
# Static archive of one object; linked into the *-selftest programs
# that talk to agentd. Same pattern as libjson above.
"$CC" "${USER_CFLAGS[@]}" -c -o user/_obj/libagent.o user/libagent.c

# Basic programs — no libjson, no libcrypto. The smallest binaries.
# Session 78 additions: sleep (for cancel tests), selftest (meta runner).
# Session 83 additions: cp, mv, rm, mkdir, rmdir, chmod, touch, find —
# path-A "usable Unix" coreutils gap-fill.
USER_PROGS=(hello sh echo httpd ed init
            head tail tee tr seq kill pwd
            nc wget telnet irc ircd beep usbtest vi id
            dbg dbgtest sandbox kvctl agentctl sleep
            sandbox-selftest limits-selftest kv-selftest
            smp-hammer
            cp mv rm mkdir rmdir chmod touch find
            man
            lua
            cc
            aplay
            rand hvc balloonctl)
# Session 108+: graphics programs link in libgfx on top of libuser.
# Same separate-list pattern as the JSON / agent / crypto buckets.
GFX_PROGS=(gfx mouse wmd)
# Session 112 — WM client programs link libwm + libgfx on top of
# libuser. libwm wraps the SYS_WM_* protocol; libgfx is pulled in
# because clients may want font/glyph helpers later (none used by
# wmhello today, but the per-program ELF size is small).
WMCLIENT_PROGS=(wmhello wmtype wmclock wmpaint wmpair wmfiles wmsysinfo wmps wmterm wmedit wmcalc wmview)
# Session 81 note: `count`, `pluck`, `where`, `sort` moved to
# JSON_PROGS below because they're JSONL-aware producers/consumers
# and need libjson linked.
# Session 82: `grep` and `uniq` joined them. tee/tr stay here —
# tee is byte-transparent (no JSONL parsing) and tr refuses
# --advjson outright (no JSONL parsing either), so neither needs
# libjson linked.
for name in "${USER_PROGS[@]}"; do
    "$CC" "${USER_CFLAGS[@]}" -c -o "user/_obj/${name}.o" "user/${name}.c"
    "$LD" -m "$LD_EMUL" -T user/user.ld --gc-sections -o "user/_obj/${name}.elf" \
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
# Session 81: pluck, where, count, sort are JSONL-aware too.
# Session 82: grep, uniq joined them.
JSON_PROGS=(ls cat wc date ps agentd pluck where count sort grep uniq)
for name in "${JSON_PROGS[@]}"; do
    "$CC" "${USER_CFLAGS[@]}" -c -o "user/_obj/${name}.o" "user/${name}.c"
    "$LD" -m "$LD_EMUL" -T user/user.ld --gc-sections -o "user/_obj/${name}.elf" \
        user/_obj/start.o "user/_obj/${name}.o" user/_obj/libuser.o \
        "${LIBJSON_OBJS[@]}"
    "$OBJCOPY" -O binary -j .text -j .rdata -j .data \
        "user/_obj/${name}.elf" "user/_obj/${name}.bin"
    nm "user/_obj/${name}.elf" \
        | awk '/^[0-9a-fA-F]+ [Tt] / {printf "%s %s\n", $1, $3}' \
        > "user/_obj/${name}.syms"
    echo "        ${name}.bin = $(stat -c%s user/_obj/${name}.bin) bytes"
done

# Session 78: in-guest selftests that talk to agentd via TCP. Link
# libagent on top of libuser. Each is a small standalone binary;
# the meta-runner `selftest.elf` (in USER_PROGS above) fork+execs
# them in turn.
# Session 79: meta-runner moved here too because it now links libagent
# (for the inter-test state-summary print + future drain helpers).
AGENT_PROGS=(jobs-selftest subscribe-selftest cron-selftest selftest pipeline-selftest)
for name in "${AGENT_PROGS[@]}"; do
    "$CC" "${USER_CFLAGS[@]}" -c -o "user/_obj/${name}.o" "user/${name}.c"
    # Session 81: pipeline-selftest uses libjson for parsing the
    # JSONL output of its sub-shell calls. Link libjson alongside
    # libagent — harmless for the other agent selftests (linker
    # discards unreferenced symbols).
    "$LD" -m "$LD_EMUL" -T user/user.ld --gc-sections -o "user/_obj/${name}.elf" \
        user/_obj/start.o "user/_obj/${name}.o" \
        user/_obj/libuser.o user/_obj/libagent.o \
        "${LIBJSON_OBJS[@]}"
    "$OBJCOPY" -O binary -j .text -j .rdata -j .data \
        "user/_obj/${name}.elf" "user/_obj/${name}.bin"
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
    "$LD" -m "$LD_EMUL" -T user/user.ld --gc-sections -o "user/_obj/${name}.elf" \
        user/_obj/start.o "user/_obj/${name}.o" user/_obj/libuser.o \
        "${LIBCRYPTO_OBJS[@]}"
    "$OBJCOPY" -O binary -j .text -j .rdata -j .data \
        "user/_obj/${name}.elf" "user/_obj/${name}.bin"
    echo "        ${name}.bin = $(stat -c%s user/_obj/${name}.bin) bytes"
done

# Session 108 — Path C graphical programs link libgfx in addition to
# libuser. Started with `gfx`; the future `wm` daemon will also land
# in this bucket.
for name in "${GFX_PROGS[@]}"; do
    src="user/${name}.c"
    if [ ! -f "$src" ]; then continue; fi
    "$CC" "${USER_CFLAGS[@]}" -c -o "user/_obj/${name}.o" "$src"
    "$LD" -m "$LD_EMUL" -T user/user.ld --gc-sections -o "user/_obj/${name}.elf" \
        user/_obj/start.o "user/_obj/${name}.o" user/_obj/libuser.o \
        "${LIBGFX_OBJS[@]}"
    "$OBJCOPY" -O binary -j .text -j .rdata -j .data \
        "user/_obj/${name}.elf" "user/_obj/${name}.bin"
    nm "user/_obj/${name}.elf" \
        | awk '/^[0-9a-fA-F]+ [Tt] / {printf "%s %s\n", $1, $3}' \
        > "user/_obj/${name}.syms"
    echo "        ${name}.bin = $(stat -c%s user/_obj/${name}.bin) bytes"
done

# Session 112 — WM client programs. Link libwm in addition to
# libgfx + libuser. Same shape as GFX_PROGS otherwise.
for name in "${WMCLIENT_PROGS[@]}"; do
    src="user/${name}.c"
    if [ ! -f "$src" ]; then continue; fi
    "$CC" "${USER_CFLAGS[@]}" -c -o "user/_obj/${name}.o" "$src"
    "$LD" -m "$LD_EMUL" -T user/user.ld --gc-sections -o "user/_obj/${name}.elf" \
        user/_obj/start.o "user/_obj/${name}.o" user/_obj/libuser.o \
        "${LIBGFX_OBJS[@]}" "${LIBWM_OBJS[@]}"
    "$OBJCOPY" -O binary -j .text -j .rdata -j .data \
        "user/_obj/${name}.elf" "user/_obj/${name}.bin"
    nm "user/_obj/${name}.elf" \
        | awk '/^[0-9a-fA-F]+ [Tt] / {printf "%s %s\n", $1, $3}' \
        > "user/_obj/${name}.syms"
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
AGENTD_MANIFEST_MAX=24576     # MANIFEST_MAX in user/agentd.c (session 77 bump for cron tools)
# Session 75: agentd.bin cap. user.ld folds .bss into .data so the
# raw bin includes every zero-initialized buffer (conns x REQ_MAX +
# RESP_MAX + SCRATCH_MAX, jobs x JOB_RING_SZ x 2, g_tools_arr, etc).
# Post-session-74 footprint is ~221 KiB; session 76 nudged it to
# ~237 KiB; session 77 takes it to ~270 KiB after the g_tools_arr
# bump (16->24 KiB for cron schemas) plus the 32-entry cron table.
# Cap at 320 KiB for ~16% headroom. A runaway here is usually a
# JOB_MAX / MAX_CONN / MAX_CRON_ENTRIES bump or a giant new global
# — fix the constant before bumping this.
AGENTD_BIN_MAX=491520        # 480 KiB cap on user/_obj/agentd.bin
                             # (session 79 bump 320 -> 384 — moved
                             #  load_tools_manifest's raw[24K] +
                             #  scratch[32K] from stack to .bss to
                             #  avoid a 57 KiB stack frame that was
                             #  intermittently overflowing.)
                             # (session 81 bump 384 -> 480 — shell.run
                             #  added a 64 KiB stdout-capture buffer
                             #  for JSONL pipelines per docs/69's spec.)

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

agentd_size=$(stat -c%s user/_obj/agentd.bin)
if [ "$agentd_size" -gt "$AGENTD_BIN_MAX" ]; then
    echo "ERROR: agentd.bin is $agentd_size bytes, AGENTD_BIN_MAX is $AGENTD_BIN_MAX" >&2
    echo "  fix: trim user/agentd.c's per-conn / per-job BSS — likely" >&2
    echo "       suspects are JOB_MAX, JOB_RING_SZ, REQ_MAX, MAX_CONN." >&2
    echo "       Or bump AGENTD_BIN_MAX here (and watch the post-FS pad)." >&2
    exit 1
fi

# Session 80: kernel .bss must end below VGA framebuffer at 0xA0000.
#
# The kernel's BSS section grows DOWN from the end of the kernel image
# in memory — kernel_end = bss VMA + bss size. The stack_top symbol is
# placed at the very end of .bss (entry.S `mov $stack_top, %esp`), so
# the stack PHYSICALLY lives at the highest .bss address and grows
# downward from there.
#
# Two physical-memory landmines:
#   0x9FC00  BIOS EBDA — typically holds ACPI RSDP/MADT pointers we
#            read later. Stack pushes corrupt those tables.
#   0xA0000  VGA framebuffer MMIO. Writes here go through the VGA
#            controller (chained-4 latching, write-masking, etc.) and
#            get MANGLED. A stack push that lands in this range stores
#            a different value than written; a later pop reads garbage
#            into EIP and the kernel hangs at the next `ret`.
#
# Session 80 hit this exact bug when SMP_TRACE bloat shifted .bss up
# by 4 KiB into 0xA0000+. Symptom: silent hang at fbcon_init (first
# deep-stack call after boot) — no fault dump, no crash, just frozen.
# Diagnosed by inspecting objdump -h kernel.elf and noticing
# .bss end > 0xA0000.
#
# Hard limit is 0xA0000 (VGA). Softer warning at 0x9FC00 (EBDA).
BSS_HARD_LIMIT=$((0xA0000))    # VGA RAM begins — stack pushes mangle
BSS_SOFT_LIMIT=$((0x9FC00))    # EBDA begins — ACPI tables at risk
bss_end_hex=$(objdump -h kernel/kernel.elf | awk '/\.bss/ {printf "0x%x\n", strtonum("0x"$4) + strtonum("0x"$3); exit}')
bss_end=$((bss_end_hex))
if [ "$bss_end" -ge "$BSS_HARD_LIMIT" ]; then
    printf 'ERROR: kernel .bss ends at 0x%x — overlaps VGA RAM (0xA0000+)\n' \
        "$bss_end" >&2
    echo "  hit limit: stack lives at top of .bss; writes to 0xA0000+ go" >&2
    echo "             through the VGA controller and get mangled." >&2
    echo "             Symptom is a silent hang at the first deep-stack" >&2
    echo "             function call (typically fbcon_init/fbcon_clear)." >&2
    echo "  fix:       trim a BSS-heavy global, OR shrink .text/.rdata" >&2
    echo "             enough to drop .bss start by 4 KiB, OR move the" >&2
    echo "             stack to a hardcoded address in entry.S below" >&2
    echo "             0x9FC00." >&2
    exit 1
fi
if [ "$bss_end" -ge "$BSS_SOFT_LIMIT" ]; then
    printf 'WARNING: kernel .bss ends at 0x%x — extends into BIOS EBDA (0x9FC00+)\n' \
        "$bss_end" >&2
    echo "  ACPI RSDP/MADT may live here. smp_init / acpi parsing may" >&2
    echo "  read corrupted bytes once the kernel stack grows past this" >&2
    echo "  region. Trim BSS or move the stack before bss creeps to 0xA0000." >&2
fi

# Session 75: at-a-glance sizes / headroom so we notice a budget
# creeping toward its cap weeks before it actually breaks. Printed
# AFTER the assertions pass so a build failure prints the precise
# diagnostic, not this generic line.
pct() { echo $(( $1 * 100 / $2 )); }
echo "sizes: kernel.bin $kernel_size/$kernel_budget ($(pct $kernel_size $kernel_budget)%)" \
     " agent.tools.json $manifest_size/$AGENTD_MANIFEST_MAX ($(pct $manifest_size $AGENTD_MANIFEST_MAX)%)" \
     " agentd.bin $agentd_size/$AGENTD_BIN_MAX ($(pct $agentd_size $AGENTD_BIN_MAX)%)"

# Session 134 (Path B Phase 2 of the tcc port): CROSS-compile tcc for
# AdventOS so it runs inside the OS. Uses our libuser+libc.bin libc
# surface; stub system headers in tcc/adventos-include/ keep gcc from
# pulling in the host Windows ucrt.  See docs/120-pathB-tcc-phase2.md.
#
# Two -D flags disable subsystems we don't need:
#   CONFIG_TCC_SEMLOCK=0  — single-threaded, no pthread sem_t needed
#   CONFIG_TCC_BACKTRACE=0 — no signal-handler-based crash dumps
# CONFIG_TCC_STATIC=1 keeps tcc from referencing dlopen at link time.
if [ -d tcc ] && [ -f tcc/tcc.c ] && [ -d tcc/adventos-include ]; then
    echo "[5g/7] cross-compile tcc.elf (Phase 2 — tcc running inside AdventOS)"
    mkdir -p tcc/_obj
    "$CC" "${USER_CFLAGS[@]}" \
        -nostdinc \
        -U_WIN32 -U_WIN64 -U__WIN32__ -U__WIN32 -U__MINGW32__ -U__MINGW64__ \
        -DTCC_TARGET_I386 -DONE_SOURCE -DCONFIG_TCC_STATIC=1 \
        -DCONFIG_TCC_PREDEFS=1 -DCONFIG_TCC_SEMLOCK=0 \
        -DCONFIG_TCC_BACKTRACE=0 \
        -DTCC_VERSION='"0.9.28adventos"' \
        -include tcc/adventos-include/adventos-libc.h \
        -Iuser -Itcc/adventos-include -Itcc -Itcc/include \
        -w -c -o tcc/_obj/tcc-cross.o tcc/tcc.c
    "$CC" "${USER_CFLAGS[@]}" -w \
        -c -o tcc/_obj/softfp_stubs.o tcc/adventos-include/softfp_stubs.c
    "$LD" -m i386pe -T user/user.ld -o tcc/_obj/tcc.elf \
        user/_obj/start.o tcc/_obj/tcc-cross.o user/_obj/libuser.o \
        tcc/_obj/softfp_stubs.o
    "$OBJCOPY" -O binary -j .text -j .rdata -j .data \
        tcc/_obj/tcc.elf tcc/_obj/tcc.bin
    echo "        tcc.bin = $(stat -c%s tcc/_obj/tcc.bin) bytes (i386 AdventOS user program)"
fi

echo "[6/7] build disk image (boot + kernel)"
cat boot/boot.bin kernel/kernel.bin > os.img

echo "[7/7] mkfs + append AdventFS at LBA $fs_lba"
"${PYTHON:-$(command -v python python3 2>/dev/null | head -1)}" mkfs.py
fs_offset=$(( fs_lba * 512 ))
sz=$(stat -c%s os.img)
if [ "$sz" -lt "$fs_offset" ]; then
    pad=$(( fs_offset - sz ))
    dd if=/dev/zero bs=1 count="$pad" >> os.img 2>/dev/null
fi
cat fs.img >> os.img

# Pad up to (fs_lba + 8192) sectors so SYS_FS_WRITE can grow files
# past the initial mkfs payload. Session 46 bumped 1024 → 2048;
# session 54 bumps 2048 → 4096; session 75 bumps 4096 → 8192 to
# match the kernel-side FS bitmap cap (kernel/fs.c::
# FS_BITMAP_BYTES_MAX). Crucial for files like /etc/ssh_host_key
# that sshd creates AT RUNTIME — without enough padding here QEMU
# silently drops writes past EOF, and the file's data block reads
# -1 on the next boot even though the metadata entry persists.
final_size=$(( (fs_lba + 8192) * 512 ))
sz=$(stat -c%s os.img)
if [ "$sz" -lt "$final_size" ]; then
    truncate -s "$final_size" os.img
fi

echo "        os.img = $(stat -c%s os.img) bytes  (boot + kernel + FS @ LBA $fs_lba)"

echo "OK. Minimal run (graphical QEMU window + USB keyboard + USB tablet):"
echo "    qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32 \\"
echo "        -smp 1 \\"
echo "        -device piix3-usb-uhci,id=usb0 \\"
echo "        -device usb-kbd,bus=usb0.0 \\"
echo "        -device usb-tablet,bus=usb0.0"
echo ""
echo "  Click into the QEMU window and type. The USB-HID polling task"
echo "  (kernel/usb_hid.c:usb_hid_kbd_task) reads each 8-byte boot-protocol"
echo "  report and injects ASCII into the kbd ring buffer. Output also"
echo "  tees to the host terminal because of -serial stdio."
echo ""
echo "  usb-tablet (session 141, Path C phase 34) delivers ABSOLUTE pointer"
echo "  coords so the wmd cursor stays locked to QEMU's host pointer — no"
echo "  click-to-grab, no PS/2 acceleration drift.  Drop it if you prefer"
echo "  the old PS/2-relative behaviour; the kernel still initialises the"
echo "  PS/2 mouse path as a fallback."
echo ""
echo "Full run with networking + USB storage (skip if any device errors):"
echo "    qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32 \\"
echo "        -smp 2 \\"
echo "        -netdev user,id=net0,hostfwd=tcp::8080-:80,hostfwd=tcp::7000-:7000,hostfwd=tcp::2222-:2222 \\"
echo "        -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \\"
echo "        -device piix3-usb-uhci,id=usb0 \\"
echo "        -device usb-kbd,bus=usb0.0 \\"
echo "        -device usb-tablet,bus=usb0.0 \\"
echo "        -drive id=usbfs,file=usbfs.img,format=raw,if=none \\"
echo "        -device usb-storage,drive=usbfs,bus=usb0.0"
echo ""
echo "  Hostfwd maps: 8080 → in-guest httpd, 7000 → agentd JSON-RPC,"
echo "  2222 → in-guest sshd. -smp 2 works as of session 80 (four"
echo "  scheduler/locking bugs fixed; see docs/68-smp2-deadlock-fixes.md)."
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
