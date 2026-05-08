#!/usr/bin/env python3
"""
Build the AdventFS image (fs.img):
  - Extract .up1 and .up2 sections from kernel/kernel.elf via objcopy
  - Wrap each in a minimal ELF32 executable (ELF header + 1 PT_LOAD phdr + bytes)
  - Lay out as superblock + concatenated files

The resulting fs.img is appended to os.img at LBA 200 by the build script.
"""

import os
import struct
import subprocess
import sys

SECTOR_SIZE   = 512
USER_VA       = 0x40000000
EHDR_SIZE     = 52
PHDR_SIZE     = 32

FS_NAME_MAX   = 16
FS_MAX_FILES  = 16
ENTRY_SIZE    = FS_NAME_MAX + 4 + 4   # name + start_sector + size

KERNEL_ELF    = 'kernel/kernel.elf'
OUT_IMG       = 'fs.img'

def extract_section(elf_path, section, out_path):
    """objcopy -O binary -j <section> <elf> <out>"""
    subprocess.check_call([
        'objcopy', '-O', 'binary', '-j', section, elf_path, out_path,
    ])

def make_elf(code, entry_va):
    """Synthesize a minimal ELF32 executable wrapping `code`.

    Layout: [ELF header 52B][1 program header 32B][code bytes].
    The single PT_LOAD segment is mapped at `entry_va`.
    """
    code_size = len(code)

    # e_ident[16]
    ident = bytes([
        0x7f, ord('E'), ord('L'), ord('F'),
        1,    # ELFCLASS32
        1,    # ELFDATA2LSB (little-endian)
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
        EHDR_SIZE,                      # e_phoff    (right after ehdr)
        0,                              # e_shoff    (no sections)
        0,                              # e_flags
        EHDR_SIZE,                      # e_ehsize
        PHDR_SIZE,                      # e_phentsize
        1,                              # e_phnum
        0,                              # e_shentsize
        0,                              # e_shnum
        0,                              # e_shstrndx
    )

    phdr = struct.pack(
        '<IIIIIIII',
        1,                              # p_type   = PT_LOAD
        EHDR_SIZE + PHDR_SIZE,          # p_offset (where code starts)
        entry_va,                       # p_vaddr
        entry_va,                       # p_paddr
        code_size,                      # p_filesz
        code_size,                      # p_memsz  (no .bss; equal to filesz)
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
    # Sanity: kernel.elf exists
    if not os.path.exists(KERNEL_ELF):
        print(f"mkfs: {KERNEL_ELF} not found — build the kernel first", file=sys.stderr)
        sys.exit(1)

    # 1. Pull .up1 and .up2 raw bytes out of the kernel.
    extract_section(KERNEL_ELF, '.up1', '_tmp_up1.bin')
    extract_section(KERNEL_ELF, '.up2', '_tmp_up2.bin')
    up1 = open('_tmp_up1.bin', 'rb').read()
    up2 = open('_tmp_up2.bin', 'rb').read()

    # 2. Wrap each in an ELF.
    up1_elf = make_elf(up1, USER_VA)
    up2_elf = make_elf(up2, USER_VA)

    # 3. Pad each to a sector boundary so the next file starts cleanly.
    up1_padded = pad_to_sector(up1_elf)
    up2_padded = pad_to_sector(up2_elf)

    # 4. Build the file table. start_sector is RELATIVE to FS area.
    #    Sector 0 = superblock; sector 1 = first file.
    files = [
        ('hello.elf', 1,                                             len(up1_elf)),
        ('count.elf', 1 + len(up1_padded) // SECTOR_SIZE,            len(up2_elf)),
    ]

    # 5. Pack the superblock to match `struct fs_super` exactly.
    sb  = b'ADVENTFS'
    sb += struct.pack('<I', len(files))
    for name, start, size in files:
        name_b = name.encode('ascii').ljust(FS_NAME_MAX, b'\x00')[:FS_NAME_MAX]
        sb += name_b + struct.pack('<II', start, size)
    # Pad remaining slots with zero entries so the superblock-to-C-struct
    # layout matches whether the kernel reads N or FS_MAX_FILES.
    empty_entry = b'\x00' * ENTRY_SIZE
    sb += empty_entry * (FS_MAX_FILES - len(files))
    sb = pad_to_sector(sb)

    # 6. Concatenate.
    fs = sb + up1_padded + up2_padded
    open(OUT_IMG, 'wb').write(fs)

    print(f"  FS    {len(fs)} bytes ({len(fs) // SECTOR_SIZE} sectors)")
    for name, start, size in files:
        print(f"        {name:<12} sec {start}  ({size} bytes)")

if __name__ == '__main__':
    build()
