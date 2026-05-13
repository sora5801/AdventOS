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
# Directories. Strings are top-level; (name, parent) tuples nest one
# level deep (session 59 added /etc/ssl to ship the CA roots).
DIRECTORIES = [
    'etc',
    'mnt',                   # session 42 — USB drive mounts here at /mnt/usb
    ('ssl', 'etc'),          # session 59 — CA root store + httpsd server cert/key
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
    # gui.elf (sessions 34/57/61-63) was removed when AdventOS narrowed
    # to a CLI-only OS for developers and AI agents.
    # Session 37: AC97 audio test program — generates PCM tones.
    ('beep.elf',  'user/_obj/beep.bin',  None),
    # Session 36: TLS 1.3 + HTTPS using libcrypto.
    ('cryptotest.elf', 'user/_obj/cryptotest.bin', None),
    ('httpsd.elf',     'user/_obj/httpsd.bin',     None),
    ('httpsget.elf',   'user/_obj/httpsget.bin',   None),
    # Session 41: USB Mass Storage round-trip test.
    ('usbtest.elf',    'user/_obj/usbtest.bin',    None),
    # Session 46: vi-like modal editor.
    ('vi.elf',         'user/_obj/vi.bin',         None),
    # Session 47: multi-user + login.
    ('login.elf',      'user/_obj/login.bin',      None),
    ('id.elf',         'user/_obj/id.bin',         None),
    # Session 50: TLS-backed remote shell.
    ('sshd.elf',       'user/_obj/sshd.bin',       None),
    ('ssh.elf',        'user/_obj/ssh.bin',        None),
    # Session 57: ptrace-based debugger + its toy target.
    ('dbg.elf',        'user/_obj/dbg.bin',        None),
    ('dbgtest.elf',    'user/_obj/dbgtest.bin',    None),
    # Session 58: RSA-PKCS#1 v1.5 sign/verify exerciser.
    ('rsatest.elf',    'user/_obj/rsatest.bin',    None),
    # gclient.elf (session 62, out-of-process WM client) removed with
    # the WM.
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
    ('passwd',    'fs/passwd',    'etc'),     # session 47
    ('ssh_keys',  'fs/ssh_keys',  'etc'),     # session 53 — pubkey auth
    ('resolv.conf', 'fs/etc/resolv.conf', 'etc'),  # session 60 — DNS fail-over list
    # Session 57: debugger sidecars. The interactive debugger
    # (dbg.elf) opens "<prog>.syms" at runtime to resolve symbol
    # names → user VAs. Only ship for programs we want to debug;
    # adding more here costs ~2 KB each.
    ('dbgtest.syms', 'user/_obj/dbgtest.syms', None),
    ('dbg.syms',     'user/_obj/dbg.syms',     None),
    # Session 59 — CA root store + httpsd's leaf cert + key. The
    # test fixture: `test-ca.der` is a self-signed P-256 CA generated
    # offline with openssl. `server.der` is httpsd's leaf cert signed
    # by that CA. `server.key` is the raw 32-byte ECDSA scalar.
    # httpsget loads test-ca.der as the trust anchor and validates
    # the chain on every connect.
    ('test-ca.der', 'fs/etc/ssl/test-ca.der', 'ssl'),
    ('server.der',  'fs/etc/ssl/server.der',  'ssl'),
    ('server.key',  'fs/etc/ssl/server.key',  'ssl', 0o600),
]

# Session 47: generate /etc/passwd at build time. Format per line:
#   name:salt$sha256_hex:uid:gid:home:shell
# salt is 8 ASCII chars; password hash = sha256(salt || password) in
# lowercase hex. login.elf verifies by recomputing the hash.
USERS = [
    # (name,  password, uid,  gid,  home, shell)
    ('root',  'root',   0,    0,    '/',  'sh.elf'),
    ('guest', 'guest',  1000, 1000, '/',  'sh.elf'),
]
USER_SALTS = ['ABCDef01', 'GH23ij45']

def gen_passwd_file():
    import hashlib
    lines = []
    for i, (name, password, uid, gid, home, shell) in enumerate(USERS):
        salt = USER_SALTS[i % len(USER_SALTS)]
        h = hashlib.sha256((salt + password).encode('ascii')).hexdigest()
        lines.append(f'{name}:{salt}${h}:{uid}:{gid}:{home}:{shell}')
    return '\n'.join(lines) + '\n'


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


def encode_entry(name, start, size, type_, parent, uid=0, gid=0, mode=0o644):
    """fs_entry layout (32 bytes):
         name[16] + start_sector(4) + size(4) + type(1) + parent_dir(1)
         + uid(2) + gid(2) + mode(2)
    Session 47 carved uid+gid out of the original 6-byte reserved
    tail; session 48 carved mode out of the last 2 bytes.
    Files baked by mkfs default to root (uid=0, gid=0); runtime
    creators stamp the calling task's uid/gid via fs.c."""
    name_b = name.encode('ascii').ljust(FS_NAME_MAX, b'\x00')[:FS_NAME_MAX]
    return name_b + struct.pack('<IIBBHHH', start, size, type_, parent,
                                 uid & 0xFFFF, gid & 0xFFFF, mode & 0xFFFF)


def build_image(directories, user_programs, raw_blobs, data_files,
                 out_name, log_prefix):
    """Generic build — takes its own file lists + output filename. Used
    to produce fs.img (the boot disk) AND usbfs.img (the USB drive
    image attached via -device usb-storage,drive=usbfs).

    Session 48: each entry now carries a mode. Defaults:
      directories  → 0o755 (rwxr-xr-x)
      user_programs (ELFs) → 0o755 (executable for everyone)
      raw_blobs (libc.bin) → 0o755 (executable-like; loader maps it)
      data_files → 0o644 (rw-r--r--; readable to all, writable only by root)
    """
    entries = []      # (name, start_sector, size, type, parent_idx, mode)
    file_blobs = []
    next_sector = FS_SUPER_SECTORS

    dir_idx_by_name = {}
    for d in directories:
        # Top-level dirs are bare strings under root. Nested dirs are
        # (name, parent_name) tuples — parent must already have been
        # emitted earlier in the list. Session 59 added /etc/ssl this
        # way so the CA store and server cert/key live there.
        if isinstance(d, tuple):
            dname, parent_name = d
            parent_idx = dir_idx_by_name[parent_name]
        else:
            dname, parent_idx = d, FS_DIR_ROOT
        # Subdirectories are looked up by their bare name (not the
        # full path) — the existing directory table is flat and
        # cross-checks via parent_dir.
        dir_idx_by_name[dname] = len(entries)
        entries.append((dname, 0, 0, FS_TYPE_DIR, parent_idx, 0o755))

    def add_file(name, blob, raw_size, parent_dir_name, mode):
        nonlocal next_sector
        parent_idx = (FS_DIR_ROOT if parent_dir_name is None
                      else dir_idx_by_name[parent_dir_name])
        padded = pad_to_sector(blob)
        entries.append((name, next_sector, raw_size, FS_TYPE_FILE,
                         parent_idx, mode))
        file_blobs.append(padded)
        next_sector += len(padded) // SECTOR_SIZE

    for fs_name, bin_path, parent in user_programs:
        if not os.path.exists(bin_path):
            print(f"mkfs: {bin_path} not found — build user programs first",
                  file=sys.stderr); sys.exit(1)
        raw = open(bin_path, 'rb').read()
        elf = make_elf(raw, USER_VA)
        add_file(fs_name, elf, len(elf), parent, 0o755)

    for fs_name, src_path, parent in raw_blobs:
        if not os.path.exists(src_path):
            print(f"mkfs: {src_path} not found", file=sys.stderr); sys.exit(1)
        raw = open(src_path, 'rb').read()
        add_file(fs_name, raw, len(raw), parent, 0o755)

    for entry in data_files:
        # Allow either (name, src, parent) for the standard 0o644 case
        # or (name, src, parent, mode) for files that need a tighter
        # permission — the session-59 server.key wants 0o600 so guests
        # can't peek at httpsd's private key.
        if len(entry) == 4:
            fs_name, src_path, parent, mode = entry
        else:
            fs_name, src_path, parent = entry
            mode = 0o644
        if not os.path.exists(src_path):
            print(f"mkfs: {src_path} not found", file=sys.stderr); sys.exit(1)
        raw = open(src_path, 'rb').read()
        add_file(fs_name, raw, len(raw), parent, mode)

    if len(entries) > FS_MAX_FILES:
        print(f"mkfs: {len(entries)} entries exceeds FS_MAX_FILES "
              f"({FS_MAX_FILES})", file=sys.stderr); sys.exit(1)

    super_hdr = b'ADVENTFS' + struct.pack('<I', len(entries))
    super_hdr = super_hdr.ljust(SECTOR_SIZE, b'\x00')

    entry_blob = b''
    for (name, start, size, type_, parent, mode) in entries:
        entry_blob += encode_entry(name, start, size, type_, parent,
                                    mode=mode)
    entry_blob += b'\x00' * (FS_ENTRY_SIZE * (FS_MAX_FILES - len(entries)))
    entry_blob = entry_blob.ljust(FS_SUPER_SECTORS * SECTOR_SIZE - SECTOR_SIZE, b'\x00')

    fs = super_hdr + entry_blob + b''.join(file_blobs)
    open(out_name, 'wb').write(fs)

    print(f"  {log_prefix:5} {len(fs)} bytes ({len(fs) // SECTOR_SIZE} sectors)")
    for i, (name, start, size, type_, parent, mode) in enumerate(entries):
        kind = 'DIR ' if type_ == FS_TYPE_DIR else 'FILE'
        parent_str = '/' if parent == FS_DIR_ROOT else f"/{directories[parent]}"
        if type_ == FS_TYPE_FILE:
            print(f"        [{i:2}] {kind} {parent_str}/{name:<12} "
                  f"sec {start}  ({size} bytes, {mode:o})")
        else:
            print(f"        [{i:2}] {kind} {parent_str}/{name}")


def build():
    """Build the boot disk filesystem image (fs.img)."""
    # Session 47: regenerate /etc/passwd on every build so the
    # hashes always match the USERS table. Open in BINARY mode so
    # the on-disk file uses LF line endings on every host platform —
    # Python's default text mode CRLF-translates on Windows, which
    # bakes a stray \r into the last field of each user record and
    # makes login.elf's parsed shell name "sh.elf\r" instead of
    # "sh.elf". The exec then fails with -ENOENT.
    os.makedirs('fs', exist_ok=True)
    open('fs/passwd', 'wb').write(gen_passwd_file().encode('ascii'))

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
