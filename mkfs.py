#!/usr/bin/env python3
"""
Build the AdventFS image (fs.img).

Pre-built user programs live in user/_obj/<name>.bin as flat binaries
linked at virtual address 0x40000000. mkfs.py wraps each in a minimal
ELF32 executable (one PT_LOAD segment) and lays out:

    Sector 0:        Superblock (magic 'ADVENTFS', file count, file table)
    Sector 1+:       File data, each padded to a sector

The build script appends fs.img to os.img at LBA 200.
"""

import os
import struct
import sys

SECTOR_SIZE   = 512
USER_VA       = 0x40000000
EHDR_SIZE     = 52
PHDR_SIZE     = 32

FS_NAME_MAX   = 16
FS_MAX_FILES  = 16
ENTRY_SIZE    = FS_NAME_MAX + 4 + 4   # name + start_sector + size

OUT_IMG       = 'fs.img'

# Maps on-disk filename → path of the flat binary built by build.sh.
USER_PROGRAMS = [
    ('hello.elf', 'user/_obj/hello.bin'),
    ('count.elf', 'user/_obj/count.bin'),
    ('sh.elf',    'user/_obj/sh.bin'),
]

def make_elf(code, entry_va):
    """Wrap a flat binary in a minimal ELF32 executable.

    Layout: [ELF header 52B][1 program header 32B][code bytes].
    The single PT_LOAD segment is mapped at `entry_va`. Because the
    user program is built with -fno-zero-initialized-in-bss + -fno-common,
    there is no .bss to zero-fill: filesz == memsz == len(code).
    """
    code_size = len(code)

    ident = bytes([
        0x7f, ord('E'), ord('L'), ord('F'),
        1,    # ELFCLASS32
        1,    # ELFDATA2LSB
        1,    # EV_CURRENT
        0,    # ELFOSABI_NONE
        0,    # ABIVERSION
        0, 0, 0, 0, 0, 0, 0,
    ])

    ehdr = ident + struct.pack(
        '<HHIIIIIHHHHHH',
        2,                              # e_type     = ET_EXEC
        3,                              # e_machine  = EM_386
        1,                              # e_version
        entry_va,                       # e_entry
        EHDR_SIZE,                      # e_phoff
        0, 0,                           # e_shoff, e_flags
        EHDR_SIZE,                      # e_ehsize
        PHDR_SIZE, 1,                   # phentsize, phnum
        0, 0, 0,                        # shentsize, shnum, shstrndx
    )

    phdr = struct.pack(
        '<IIIIIIII',
        1,                              # p_type   = PT_LOAD
        EHDR_SIZE + PHDR_SIZE,          # p_offset
        entry_va,                       # p_vaddr
        entry_va,                       # p_paddr
        code_size,                      # p_filesz
        code_size,                      # p_memsz
        7,                              # p_flags  = R | W | X
        0x1000,                         # p_align
    )

    return ehdr + phdr + code

def pad_to_sector(data):
    rem = len(data) % SECTOR_SIZE
    if rem != 0:
        data += b'\x00' * (SECTOR_SIZE - rem)
    return data

def build():
    # Read each user binary and wrap it in ELF.
    files = []
    file_blobs = []
    next_sector = 1
    for fs_name, bin_path in USER_PROGRAMS:
        if not os.path.exists(bin_path):
            print(f"mkfs: {bin_path} not found — build user programs first",
                  file=sys.stderr)
            sys.exit(1)
        raw = open(bin_path, 'rb').read()
        elf = make_elf(raw, USER_VA)
        padded = pad_to_sector(elf)
        files.append((fs_name, next_sector, len(elf)))
        file_blobs.append(padded)
        next_sector += len(padded) // SECTOR_SIZE

    # Pack the superblock to match `struct fs_super` exactly.
    sb  = b'ADVENTFS'
    sb += struct.pack('<I', len(files))
    for name, start, size in files:
        name_b = name.encode('ascii').ljust(FS_NAME_MAX, b'\x00')[:FS_NAME_MAX]
        sb += name_b + struct.pack('<II', start, size)
    sb += b'\x00' * (ENTRY_SIZE * (FS_MAX_FILES - len(files)))
    sb = pad_to_sector(sb)

    fs = sb + b''.join(file_blobs)
    open(OUT_IMG, 'wb').write(fs)

    print(f"  FS    {len(fs)} bytes ({len(fs) // SECTOR_SIZE} sectors)")
    for (name, start, size), blob in zip(files, file_blobs):
        print(f"        {name:<12} sec {start}  ({size} bytes,"
              f" padded to {len(blob)})")

if __name__ == '__main__':
    build()
