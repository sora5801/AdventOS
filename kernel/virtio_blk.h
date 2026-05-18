/*
 * virtio-blk — paravirtualized block device. Registers itself in the
 * global blkdev table so bcache / fs / mkfs-style scanners see it
 * exactly the same way they see ATA and USB MSC.
 *
 * QEMU CLI to enable:
 *   -drive id=vd,file=disk.img,format=raw,if=none \
 *   -device virtio-blk-pci,drive=vd
 *
 * Transitional mode is the default in QEMU 10.x, so the legacy PCI
 * I/O register interface (used by this driver) stays alive alongside
 * the modern MMIO capability layout.
 */
#ifndef ADVENTOS_VIRTIO_BLK_H
#define ADVENTOS_VIRTIO_BLK_H

/* Probe PCI for a virtio-blk device. If one is present, allocate a
 * virtqueue, negotiate features, read capacity, and register the
 * resulting blkdev (name "vblk0"). Safe to call when no device is
 * present — quietly no-ops.
 *
 * Must be called AFTER PMM + paging + PIT are up (we use
 * pmm_alloc_contiguous and pit_ticks). */
void virtio_blk_init(void);

#endif
