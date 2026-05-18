#include "pci.h"
#include "../include/io.h"

static uint32_t make_address(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    return (uint32_t)(0x80000000u
                      | ((uint32_t)bus  << 16)
                      | ((uint32_t)dev  << 11)
                      | ((uint32_t)func <<  8)
                      | (off & 0xFCu));
}

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDRESS, make_address(bus, dev, func, off));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t v = pci_config_read32(bus, dev, func, (uint8_t)(off & 0xFCu));
    return (uint16_t)(v >> ((off & 2) * 8));
}

uint8_t pci_config_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t v = pci_config_read32(bus, dev, func, (uint8_t)(off & 0xFCu));
    return (uint8_t)(v >> ((off & 3) * 8));
}

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func,
                        uint8_t off, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, make_address(bus, dev, func, off));
    outl(PCI_CONFIG_DATA, value);
}

static void fill(struct pci_device *out,
                 uint8_t bus, uint8_t dev, uint8_t func,
                 uint16_t vid, uint16_t did)
{
    out->bus          = bus;
    out->device       = dev;
    out->func         = func;
    out->vendor_id    = vid;
    out->device_id    = did;
    out->subsystem_id = pci_config_read16(bus, dev, func, PCI_CFG_SUBSYS_ID);
    out->bar0         = pci_config_read32(bus, dev, func, PCI_CFG_BAR0);
    out->bar1         = pci_config_read32(bus, dev, func, PCI_CFG_BAR1);
    out->io_base      = (uint16_t)(out->bar0 & ~0x3u);
    out->io_base1     = (uint16_t)(out->bar1 & ~0x3u);
    out->irq_line     = (uint8_t)(pci_config_read32(bus, dev, func, 0x3C) & 0xFF);

    /* Ensure I/O space + bus master are enabled in CMD register. */
    uint32_t cmd_status = pci_config_read32(bus, dev, func, PCI_CFG_COMMAND);
    cmd_status |= 0x0007;          /* IO + MEM + bus master */
    pci_config_write32(bus, dev, func, PCI_CFG_COMMAND, cmd_status);
}

int pci_find(uint16_t vendor, uint16_t device, struct pci_device *out) {
    return pci_find_nth(vendor, device, 0, out);
}

int pci_find_nth(uint16_t vendor, uint16_t device, int n,
                 struct pci_device *out)
{
    int matched = 0;
    /* Brute-force: walk bus 0..3 only, dev 0..31, func 0..7. Enough for
     * QEMU's flat PCI topology — no PCI-to-PCI bridges to descend. */
    for (uint8_t bus = 0; bus < 4; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t v = pci_config_read32(bus, dev, func, 0x00);
                uint16_t vid = (uint16_t)(v & 0xFFFF);
                uint16_t did = (uint16_t)((v >> 16) & 0xFFFF);
                if (vid == 0xFFFF) {
                    if (func == 0) break;       /* no device here at all */
                    continue;                   /* try next function     */
                }
                if (vid == vendor && did == device) {
                    if (matched == n) {
                        if (out) fill(out, bus, dev, func, vid, did);
                        return 0;
                    }
                    matched++;
                }
            }
        }
    }
    return -1;
}
