/*
 * virtio-console — paravirtualized serial port (legacy ID 0x1003).
 *
 * Two queues: vq 0 = host->guest receive, vq 1 = guest->host
 * transmit. We don't negotiate VIRTIO_CONSOLE_F_MULTIPORT — the
 * default port 0 needs no control messages, and a single console
 * is enough for a "second serial" channel into AdventOS.
 *
 * QEMU CLI to enable:
 *   -chardev socket,id=hvc0,host=localhost,port=5556,server=on,wait=off \
 *   -device virtio-serial-pci,id=vsbus \
 *   -device virtconsole,chardev=hvc0,bus=vsbus.0
 *
 * From the host:  nc localhost 5556
 *   - anything you type is delivered to the guest (read via
 *     SYS_VIRTIO_CONSOLE_READ)
 *   - the guest writes (SYS_VIRTIO_CONSOLE_WRITE) show up in nc
 *
 * Buffering: 8 RX descriptors of 1024 bytes each pre-armed at init.
 * The polling task drains them every 50 ms into a small in-kernel
 * ring; SYS_VIRTIO_CONSOLE_READ pulls from that ring (non-blocking).
 */
#ifndef ADVENTOS_VIRTIO_CONSOLE_H
#define ADVENTOS_VIRTIO_CONSOLE_H

#include "../include/types.h"

void virtio_console_init(void);
void virtio_console_start_polling(void);

/* Write `n` bytes to the host. Synchronous (blocks until the host's
 * RX ring has been consumed). Returns bytes sent or -1. */
int  virtio_console_write(const void *buf, int n);

/* Read up to `n` bytes from the receive ring. Non-blocking: returns
 * 0 if nothing's pending. Returns -1 if no device. */
int  virtio_console_read(void *buf, int n);

#endif
