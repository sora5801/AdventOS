#ifndef ADVENTOS_DYLD_H
#define ADVENTOS_DYLD_H

#include "../include/types.h"

/*
 * Tiny dynamic loader for AdventOS libc.
 *
 * Userspace gets a "real C library" loaded into every process at a
 * fixed virtual address. The library (libc.bin) is built as a flat
 * binary linked at LIBC_VA = 0x70000000; the first 1 KiB holds a
 * fixed-order export table (function pointer array) that user
 * programs jmp through to call libc functions.
 *
 *   dyld_init()       — called once at boot. Reads libc.bin from
 *                       the FS, caches the bytes in kernel memory,
 *                       validates the magic header. Idempotent.
 *
 *   dyld_map_libc()   — called from elf_load when building a new
 *                       user PD. Allocates physical pages, copies
 *                       libc.bin bytes into them, maps them at
 *                       LIBC_VA in the supplied PD with USER+
 *                       WRITABLE flags. Each process gets its own
 *                       physical copy of libc — wasteful (all .text
 *                       could in principle be shared by reference),
 *                       but means libc's .data is naturally per-
 *                       process so malloc state isn't stepped on.
 */

#define LIBC_VA   0x70000000u

void dyld_init(void);
int  dyld_map_libc(uint32_t *user_pd);

/* For diagnostics / [t25] selftest. */
uint32_t dyld_libc_size(void);
int      dyld_libc_loaded(void);

#endif
