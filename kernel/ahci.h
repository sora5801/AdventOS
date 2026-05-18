/*
 * AHCI 1.3 SATA controller driver. The modern hard-disk interface
 * every real PC has shipped with for the last ~15 years. Slots into
 * the blkdev table the same way ATA and virtio-blk do — clients
 * (bcache, fs, the SYS_BLOCK_* syscalls) talk to it through the
 * uniform block-device vtable.
 *
 * QEMU CLI to attach:
 *   -drive id=hd0,file=disk.img,format=raw,if=none \
 *   -device ahci,id=ahci0 \
 *   -device ide-hd,drive=hd0,bus=ahci0.0
 *
 * Scope of this driver:
 *  - 48-bit LBA reads/writes (READ DMA EXT / WRITE DMA EXT).
 *  - One command slot at a time (slot 0); polled completion.
 *  - First port that has a SATA disk attached; additional ports
 *    are noted in the boot log but unused.
 *  - No NCQ, no port multipliers, no ATAPI, no hot-plug, no IRQs.
 *
 * Polled (not IRQ-driven) for simplicity. AHCI's per-port interrupt
 * vector goes through a single PCI INTx line; future work could
 * wire it through the virtio IRQ dispatcher.
 */
#ifndef ADVENTOS_AHCI_H
#define ADVENTOS_AHCI_H

void ahci_init(void);

#endif
