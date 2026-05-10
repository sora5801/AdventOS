/*
 * Block-device vtable. Lets callers (bcache, the new sys_block_*
 * syscalls) talk to ATA and USB Mass Storage Class through a
 * uniform interface — name, total block count, block size, and
 * synchronous read_blocks / write_blocks.
 *
 * Drivers register themselves with `blkdev_register()` at boot;
 * a small fixed array holds them. The boot ATA disk is always
 * `blkdev_get(0)`; USB drives come in as 1, 2, ... in attach order.
 *
 * Async I/O, IRQ-driven completion, and per-device queues are
 * deliberately out of scope. Both backing transports (ATA polled
 * PIO, UHCI synchronous TDs) block until the transfer finishes.
 */
#ifndef ADVENTOS_BLKDEV_H
#define ADVENTOS_BLKDEV_H

#include "../include/types.h"

#define BLKDEV_MAX        4
#define BLKDEV_NAME_LEN   16

struct blkdev;

/* All ops return 0 on success, -1 on failure. `lba` and `n` are
 * in units of `block_size` bytes. */
typedef int (*blkdev_read_fn) (struct blkdev *d, uint32_t lba, uint32_t n, void *buf);
typedef int (*blkdev_write_fn)(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf);

struct blkdev {
    char            name[BLKDEV_NAME_LEN];   /* "ata0", "usb0", ... */
    uint32_t        block_size;              /* always 512 for now  */
    uint32_t        n_blocks;                /* total addressable   */
    blkdev_read_fn  read;
    blkdev_write_fn write;
    void           *driver_data;             /* per-driver private  */
};

/* Slot management. Returns the assigned device index (0..BLKDEV_MAX-1)
 * or -1 if the table is full. The caller retains ownership of the
 * struct blkdev — the table just stores pointers. */
int             blkdev_register(struct blkdev *d);

/* Lookup. NULL if `idx` is out of range or unbound. */
struct blkdev  *blkdev_get(int idx);

/* Number of registered devices. */
int             blkdev_count(void);

#endif
