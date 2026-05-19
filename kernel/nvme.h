/*
 * NVMe — Non-Volatile Memory express. The modern PCIe-attached SSD
 * interface every contemporary OS image targets first. Successor to
 * AHCI/SATA: instead of one SATA link with a 32-slot queue, NVMe
 * uses paired submission/completion queues sitting directly in host
 * memory, with command DMA over PCIe.
 *
 * QEMU CLI:
 *   -drive id=nvme0,file=nvme.img,format=raw,if=none \
 *   -device nvme,drive=nvme0,serial=adventos-nvme0
 *
 * Scope of this driver:
 *   - Single controller (the first NVMe class device we find on PCI).
 *   - Single namespace (NSID=1 only; multi-NS would be a small loop).
 *   - One I/O submission/completion queue pair (qid=1), 64 entries.
 *   - Polled / IRQ-driven (sti+hlt) completion; one command in flight
 *     at a time (BKL serialization is in place anyway).
 *   - PRP1 + PRP2 addressing, so a single command can DMA up to 8 KiB
 *     = 16 LBAs (at 512 B). Larger transfers split at the blkdev layer.
 *
 * Registers as a blkdev named "nvme0" — bcache, the FS layer, and
 * `SYS_BLOCK_*` all work through it just like ATA / virtio-blk /
 * virtio-scsi / AHCI.
 */
#ifndef ADVENTOS_NVME_H
#define ADVENTOS_NVME_H

void nvme_init(void);

#endif
