#ifndef ADVENTOS_PCI_H
#define ADVENTOS_PCI_H

#include "../include/types.h"

/*
 * Tiny PCI helper. Just enough to find a device by vendor/device id
 * and pull the pieces we need (BAR0, IRQ line) for the RTL8139 driver.
 *
 * No bridge handling, no MSI, no MMIO BARs decoded — for the i386
 * RTL8139 in QEMU, BAR0 is an I/O BAR which is exactly what we want.
 */

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

struct pci_device {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar0;          /* raw BAR0 contents (low bit = I/O if 1)  */
    uint16_t io_base;       /* BAR0 with low 2 bits masked, if I/O BAR */
    uint8_t  irq_line;
};

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func,
                            uint8_t off, uint32_t value);

/* Returns 0 + fills `out` if found; -1 otherwise. */
int      pci_find(uint16_t vendor, uint16_t device, struct pci_device *out);

#endif
