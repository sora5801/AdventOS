#!/usr/bin/env python3
"""
Build the AdventFS image (fs.img). Hierarchical layout (session 25).

Pre-built user programs live in user/_obj/<name>.bin as flat binaries
linked at virtual address 0x40000000. mkfs.py wraps each in a minimal
ELF32 executable (one PT_LOAD segment) and lays out:

    Sector 0..1:     Superblock (magic 'ADVENTFS', file count, 16 entries)
    Sector 2+:       File data, each padded to a sector

Each entry is 32 bytes:
    char     name[16];
    uint32_t start_sector;
    uint32_t size;
    uint8_t  type;          /* 0 free, 1 file, 2 dir */
    uint8_t  parent_dir;    /* entry idx, or 0xFF for root */
    uint8_t  reserved[6];

Directories are entries with type=2 and size=start_sector=0. The
directory tree is encoded as a parent-pointer forest — walking the
table and matching `parent_dir` enumerates each directory's children.

The build script appends fs.img to os.img at LBA 200.
"""

import os
import struct
import sys

SECTOR_SIZE       = 512
USER_VA           = 0x40000000
EHDR_SIZE         = 52
PHDR_SIZE         = 32

FS_NAME_MAX       = 16
FS_MAX_FILES      = 64          # bumped from 32 in session 29 (network apps)
FS_ENTRY_SIZE     = 32          # name(16) + start(4) + size(4) + type(1) + parent(1) + 6 reserved
FS_SUPER_SECTORS  = 5           # 1 header sector + 4 entry sectors (64 * 32 = 2048B)

FS_TYPE_FREE      = 0
FS_TYPE_FILE      = 1
FS_TYPE_DIR       = 2
FS_DIR_ROOT       = 0xFF

OUT_IMG           = 'fs.img'

# Directory layout. Build the entries in this order; parent_dir
# references resolve to the table index (assigned by enumerate).
# Top-level: /etc, /bin, plus the executable binaries living at root.
DIRECTORIES = [
    'etc',
]

# (on-disk filename, source binary path, parent directory name or None for root)
USER_PROGRAMS = [
    ('hello.elf', 'user/_obj/hello.bin', None),
    ('count.elf', 'user/_obj/count.bin', None),
    ('sh.elf',    'user/_obj/sh.bin',    None),
    ('cat.elf',   'user/_obj/cat.bin',   None),
    ('echo.elf',  'user/_obj/echo.bin',  None),
    ('httpd.elf', 'user/_obj/httpd.bin', None),
    ('ed.elf',    'user/_obj/ed.bin',    None),
    ('init.elf',  'user/_obj/init.bin',  None),
    # Coreutils sweep — session 26.
    ('wc.elf',    'user/_obj/wc.bin',    None),
    ('head.elf',  'user/_obj/head.bin',  None),
    ('tail.elf',  'user/_obj/tail.bin',  None),
    ('grep.elf',  'user/_obj/grep.bin',  None),
    ('sort.elf',  'user/_obj/sort.bin',  None),
    ('uniq.elf',  'user/_obj/uniq.bin',  None),
    ('tee.elf',   'user/_obj/tee.bin',   None),
    ('tr.elf',    'user/_obj/tr.bin',    None),
    ('seq.elf',   'user/_obj/seq.bin',   None),
    ('date.elf',  'user/_obj/date.bin',  None),
    ('kill.elf',  'user/_obj/kill.bin',  None),
    ('ls.elf',    'user/_obj/ls.bin',    None),
    ('pwd.elf',   'user/_obj/pwd.bin',   None),
    # Network-app sweep — session 29.
    ('nc.elf',    'user/_obj/nc.bin',    None),
    ('wget.elf',  'user/_obj/wget.bin',  None),
    ('telnet.elf','user/_obj/telnet.bin',None),
    ('irc.elf',   'user/_obj/irc.bin',   None),
    ('ircd.elf',  'user/_obj/ircd.bin',  None),
    # GUI demo (session 34) — user-mmaps the framebuffer + polls the
    # PS/2 mouse to draw a cursor and a few interactive tiles.
    ('gui.elf',   'user/_obj/gui.bin',   None),
    # Session 37: AC97 audio test program — generates PCM tones.
    ('beep.elf',  'user/_obj/beep.bin',  None),
    # Session 36: TLS 1.3 + HTTPS using libcrypto.
    ('cryptotest.elf', 'user/_obj/cryptotest.bin', None),
    ('httpsd.elf',     'user/_obj/httpsd.bin',     None),
    ('httpsget.elf',   'user/_obj/httpsget.bin',   None),
    # Session 41: USB Mass Storage round-trip test.
    ('usbtest.elf',    'user/_obj/usbtest.bin',    None),
]

# Raw blobs that aren't ELFs — the kernel reads them as flat data.
RAW_BLOBS = [
    # Session 35: the dynamic libc. Mapped into every user process at
    # virtual address 0x70000000 by the dyld layer.
    ('libc.bin',  'libc/_obj/libc.bin',  None),
]

# (on-disk filename, source path, parent directory name or None for root)
DATA_FILES = [
    ('hello.txt', 'fs/hello.txt', None),
    ('inittab',   'fs/inittab',   'etc'),
]


def make_elf(code, entry_va):
    """Wrap a flat binary in a minimal ELF32 executable."""
    code_size = len(code)
    ident = bytes([
        0x7f, ord('E'), ord('L'), ord('F'),
        1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    ])
    ehdr = ident + struct.pack(
        '<HHIIIIIHHHHHH',
        2, 3, 1, entry_va, EHDR_SIZE, 0, 0,
        EHDR_SIZE, PHDR_SIZE, 1, 0, 0, 0,
    )
    phdr = struct.pack(
        '<IIIIIIII',
        1, EHDR_SIZE + PHDR_SIZE, entry_va, entry_va,
        code_size, code_size, 7, 0x1000,
    )
    return ehdr + phdr + code


def pad_to_sector(data):
    rem = len(data) % SECTOR_SIZE
    if rem:
        data += b'\x00' * (SECTOR_SIZE - rem)
    return data


def encode_entry(name, start, size, type_, parent):
    name_b = name.encode('ascii').ljust(FS_NAME_MAX, b'\x00')[:FS_NAME_MAX]
    return name_b + struct.pack('<IIBB6x', start, size, type_, parent)


def build_image(directories, user_programs, raw_blobs, data_files,
                 out_name, log_prefix):
    """Generic build — takes its own file lists + output filename. Used
    to produce fs.img (the boot disk) AND usbfs.img (the USB drive
    image attached via -device usb-storage,drive=usbfs)."""
    entries = []
    file_blobs = []
    next_sector = FS_SUPER_SECTORS

    dir_idx_by_name = {}
    for dname in directories:
        dir_idx_by_name[dname] = len(entries)
        entries.append((dname, 0, 0, FS_TYPE_DIR, FS_DIR_ROOT))

    def add_file(name, blob, raw_size, parent_dir_name):
        nonlocal next_sector
        parent_idx = (FS_DIR_ROOT if parent_dir_name is None
                      else dir_idx_by_name[parent_dir_name])
        padded = pad_to_sector(blob)
        entries.append((name, next_sector, raw_size, FS_TYPE_FILE, parent_idx))
        file_blobs.append(padded)
        next_sector += len(padded) // SECTOR_SIZE

    for fs_name, bin_path, parent in user_programs:
        if not os.path.exists(bin_path):
            print(f"mkfs: {bin_path} not found — build user programs first",
                  file=sys.stderr); sys.exit(1)
        raw = open(bin_path, 'rb').read()
        elf = make_elf(raw, USER_VA)
        add_file(fs_name, elf, len(elf), parent)

    for fs_name, src_path, parent in raw_blobs:
        if not os.path.exists(src_path):
            print(f"mkfs: {src_path} not found", file=sys.stderr); sys.exit(1)
        raw = open(src_path, 'rb').read()
        add_file(fs_name, raw, len(raw), parent)

    for fs_name, src_path, parent in data_files:
        if not os.path.exists(src_path):
            print(f"mkfs: {src_path} not found", file=sys.stderr); sys.exit(1)
        raw = open(src_path, 'rb').read()
        add_file(fs_name, raw, len(raw), parent)

    if len(entries) > FS_MAX_FILES:
        print(f"mkfs: {len(entries)} entries exceeds FS_MAX_FILES "
              f"({FS_MAX_FILES})", file=sys.stderr); sys.exit(1)

    super_hdr = b'ADVENTFS' + struct.pack('<I', len(entries))
    super_hdr = super_hdr.ljust(SECTOR_SIZE, b'\x00')

    entry_blob = b''
    for (name, start, size, type_, parent) in entries:
        entry_blob += encode_entry(name, start, size, type_, parent)
    entry_blob += b'\x00' * (FS_ENTRY_SIZE * (FS_MAX_FILES - len(entries)))
    entry_blob = entry_blob.ljust(FS_SUPER_SECTORS * SECTOR_SIZE - SECTOR_SIZE, b'\x00')

    fs = super_hdr + entry_blob + b''.join(file_blobs)
    open(out_name, 'wb').write(fs)

    print(f"  {log_prefix:5} {len(fs)} bytes ({len(fs) // SECTOR_SIZE} sectors)")
    for i, (name, start, size, type_, parent) in enumerate(entries):
        kind = 'DIR ' if type_ == FS_TYPE_DIR else 'FILE'
        parent_str = '/' if parent == FS_DIR_ROOT else f"/{directories[parent]}"
        if type_ == FS_TYPE_FILE:
            print(f"        [{i:2}] {kind} {parent_str}/{name:<12} "
                  f"sec {start}  ({size} bytes)")
        else:
            print(f"        [{i:2}] {kind} {parent_str}/{name}")


def build():
    """Build the boot disk filesystem image (fs.img)."""
    print(f"        layout: superblock @ sector 0..{FS_SUPER_SECTORS - 1}, "
          f"data @ sector {FS_SUPER_SECTORS}+")
    build_image(DIRECTORIES, USER_PROGRAMS, RAW_BLOBS, DATA_FILES,
                'fs.img', 'FS')


def build_usb():
    """Build a small AdventFS image (usbfs.img) that QEMU exposes as
    a USB Mass Storage device. Just enough content for usbtest to
    find the AdventFS magic on sector 0 and a single readable file."""
    readme_path = 'fs/usb-readme.txt'
    if not os.path.exists(readme_path):
        os.makedirs(os.path.dirname(readme_path), exist_ok=True)
        open(readme_path, 'w').write(
            "Hello from a USB Mass Storage device!\n\n"
            "QEMU exposes this file via:\n"
            "    -drive id=usbfs,file=usbfs.img,format=raw,if=none\n"
            "    -device usb-storage,drive=usbfs\n\n"
            "AdventOS's UHCI driver enumerates the device, the Bulk-Only\n"
            "Transport layer wraps SCSI commands, and the new sys_block_*\n"
            "syscalls expose block-level access to user space.\n"
        )
    build_image(directories=[],
                user_programs=[],
                raw_blobs=[],
                data_files=[('readme.txt', readme_path, None)],
                out_name='usbfs.img',
                log_prefix='USB')

    # Pad usbfs.img up to 256 KiB (512 sectors) so the SCSI READ
    # CAPACITY result is sensible and write tests at LBA 100 fit.
    target = 256 * 1024
    cur = os.path.getsize('usbfs.img')
    if cur < target:
        with open('usbfs.img', 'ab') as f:
            f.write(b'\x00' * (target - cur))


if __name__ == '__main__':
    build()
    build_usb()
