/*
 * USB CDC-ACM (Communications Device Class — Abstract Control Model)
 * driver. Standard "USB serial" — the protocol Arduino-style boards,
 * USB modems, and BLE radios speak.
 *
 * The driver enumerates CDC-ACM devices, sends the
 * SET_CONTROL_LINE_STATE class request (DTR=1, RTS=1) to assert the
 * host side of the virtual line, and spawns a polling kernel task
 * that reads bulk-IN data and routes incoming bytes to the kernel's
 * serial console (so `echo hello > /dev/ttyACM0` on the host prints
 * "hello" on AdventOS's serial output via kprintf).
 *
 * Kernel-side write API:
 *   usb_cdc_acm_write(data, len)  — send `len` bytes back to the host
 *                                   via bulk-OUT. Returns bytes sent
 *                                   or -1 if no CDC-ACM device.
 *
 * Wire-level test setup (real hardware):
 *   - plug in a USB-serial dongle that speaks CDC-ACM (Arduino Uno,
 *     ESP32-S3, USB-to-RS232 with CDC firmware, etc.)
 *   - QEMU `-device usb-host,hostbus=...,hostaddr=...` passes the
 *     dongle through
 *
 * QEMU does NOT ship an emulated CDC-ACM device — its `usb-serial`
 * uses the FTDI vendor protocol. Hence "graceful no-op when absent."
 */
#ifndef ADVENTOS_USB_CDC_ACM_H
#define ADVENTOS_USB_CDC_ACM_H

#include "usb.h"

/* Called from usb_core's enumeration once a CDC-ACM data interface
 * (class 0x0A) is identified. */
void usb_cdc_acm_attach(struct usb_device *d, int data_iface,
                        int comm_iface, int ep_in, int ep_out, int ep_max);

/* Send `len` bytes back to the host via the first attached CDC-ACM
 * device. Returns bytes sent or -1 if no device. */
int  usb_cdc_acm_write(const void *data, int len);

/* Spawn the RX polling task. Called from usb_start_polling alongside
 * the HID keyboard polling task. */
void usb_cdc_acm_start_polling(void);

/* Number of attached CDC-ACM ports (each shows up at /dev/ttyACM<n>).
 * Returns 0 when QEMU was launched without a usb-host passthrough of
 * a real CDC-ACM dongle. */
int  usb_cdc_acm_port_count(void);

/* Per-port read: pulls up to `n` bytes from port `port`'s RX ring.
 * Returns the number copied, 0 if the ring was empty, -1 on bad port.
 * Non-blocking — kernel SYS_READ wraps this in a yield loop. */
int  usb_cdc_acm_read(int port, void *buf, int n);

/* Per-port write: queues `len` bytes via the named port's bulk-OUT
 * endpoint. Returns bytes sent or -1. */
int  usb_cdc_acm_write_port(int port, const void *data, int len);

#endif
