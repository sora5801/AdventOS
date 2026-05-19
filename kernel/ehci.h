/*
 * USB EHCI host controller (USB 2.0, 480 Mbps).
 *
 * EHCI is the successor to UHCI / OHCI for USB 2.0. Unlike UHCI's I/O-
 * space register interface, EHCI is purely MMIO: capability registers
 * at BAR0, operational registers at BAR0+CAPLEN. The schedule lives in
 * host memory as two linked lists:
 *
 *   - Async list: control + bulk transfers. A ring of QHs (queue
 *     heads), the controller walks each one in turn, executing its
 *     qTD list (queue transfer descriptors), then moves to the next.
 *   - Periodic list: a frame-indexed array of QHs for interrupt +
 *     isochronous endpoints, walked once per frame.
 *
 * EHCI ports are wired to a companion controller (UHCI / OHCI) for
 * USB 1.x devices: when a low-speed or full-speed device is detected,
 * EHCI hands the port off via PORTSC.PortOwner. On systems where both
 * controllers are present this is automatic; on QEMU's `usb-ehci` it
 * silently drops USB 1.x devices.
 *
 * Scope of this driver (session 125):
 *   - PCI probe: prefer specific QEMU device IDs, fall back to class
 *     code 0x0C / subclass 0x03 / prog-if 0x20 (EHCI).
 *   - BIOS handoff: EECP USBLEGSUP cap walk.
 *   - HC reset + async list bring-up with a placeholder QH.
 *   - Root-hub port enumeration: log which ports have devices and
 *     whether each is high-speed (kept) or low/full-speed (released
 *     to a companion, if any).
 *
 * Transfer integration with usb_core (control / bulk / interrupt
 * through the existing class drivers) is deliberately left as a
 * follow-up — the call sites in usb_core / hid / msc / cdc-acm /
 * cdc-ecm are tightly bound to UHCI and rewiring them is its own
 * session of work. This driver gets to "controller alive + ports
 * surveyed," which is the EHCI equivalent of UHCI's uhci_init.
 *
 * QEMU CLI:
 *   -device usb-ehci,id=usb2
 *   -device usb-storage,bus=usb2.0,drive=...
 */
#ifndef ADVENTOS_EHCI_H
#define ADVENTOS_EHCI_H

/* Returns 0 if an EHCI controller was brought up, -1 if none found
 * or bring-up failed. Always safe to call; logs failures via kprintf
 * but doesn't panic. */
int ehci_init(void);

/* True if ehci_init() found and brought up a controller. */
int ehci_present(void);

#endif
