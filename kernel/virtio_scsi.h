/*
 * virtio-scsi — paravirtualized SCSI HBA. The "real" virtio storage
 * device: speaks SCSI on the wire, supports multiple targets per
 * controller and multiple LUNs per target. Most cloud images expose
 * disk via this, not virtio-blk.
 *
 * QEMU CLI:
 *   -device virtio-scsi-pci,id=scsi0,disable-modern=on \
 *   -drive id=hd0,file=disk.img,format=raw,if=none \
 *   -device scsi-hd,drive=hd0,bus=scsi0.0
 *
 * Slots into the same blkdev table as ATA / virtio-blk / USB MSC /
 * AHCI. Limitations vs a full SCSI client:
 *   - One target, one LUN (the first scsi-hd on the bus).
 *   - One in-flight request at a time (polled completion).
 *   - READ(10) / WRITE(10) commands (32-bit LBA — caps at 2 TiB).
 */
#ifndef ADVENTOS_VIRTIO_SCSI_H
#define ADVENTOS_VIRTIO_SCSI_H

void virtio_scsi_init(void);

#endif
