#ifndef ADVENTOS_PCI_H
#define ADVENTOS_PCI_H

#include "../include/types.h"

/*
 * Tiny PCI helper. Enough to find a device by vendor/device id and
 * pull the pieces we need (BARs, IRQ line) for the RTL8139, AC97,
 * virtio, and friends.
 *
 * No bridge handling, no MSI/MSI-X, no MMIO BARs decoded. virtio's
 * legacy/transitional interface uses I/O space at BAR0 — exactly
 * what we want.
 */

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* Common PCI config-space offsets we touch. */
#define PCI_CFG_VENDOR_ID         0x00
#define PCI_CFG_DEVICE_ID         0x02
#define PCI_CFG_COMMAND           0x04
#define PCI_CFG_REVISION_ID       0x08
#define PCI_CFG_PROG_IF           0x09
#define PCI_CFG_SUBCLASS          0x0A
#define PCI_CFG_CLASS             0x0B
#define PCI_CFG_BAR0              0x10
#define PCI_CFG_BAR1              0x14
#define PCI_CFG_BAR2              0x18
#define PCI_CFG_SUBSYS_VENDOR_ID  0x2C
#define PCI_CFG_SUBSYS_ID         0x2E
#define PCI_CFG_IRQ_LINE          0x3C

struct pci_device {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_id;   /* config offset 0x2E — virtio uses this
                                to identify the device type (blk/net/...) */
    uint32_t bar0;          /* raw BAR0 contents (low bit = I/O if 1)  */
    uint32_t bar1;          /* raw BAR1 — AC97 needs both NAM (mixer) +
                               NABM (bus master) which live in different BARs */
    uint16_t io_base;       /* BAR0 with low 2 bits masked, if I/O BAR */
    uint16_t io_base1;      /* BAR1 with low 2 bits masked              */
    uint8_t  irq_line;
};

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint8_t  pci_config_read8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func,
                            uint8_t off, uint32_t value);

/* Find the first device matching vendor + device id. Returns 0 +
 * fills `out` on success; -1 if not found. */
int      pci_find(uint16_t vendor, uint16_t device, struct pci_device *out);

/* Find the Nth device matching vendor + device id (0 = first, 1 =
 * second, ...). Returns 0 on success, -1 if none at that index.
 * `out` may be NULL — useful for just counting. The PCI command
 * register is left as-is when out is NULL.  */
int      pci_find_nth(uint16_t vendor, uint16_t device, int n,
                      struct pci_device *out);

#endif
