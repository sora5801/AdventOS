/*
 * virtio — the paravirtualized device family used by modern QEMU and
 * KVM. AdventOS speaks the legacy (pre-1.0) PCI transport because
 * QEMU's `-device virtio-blk-pci` / `-device virtio-net-pci` default
 * to "transitional" mode, which keeps the legacy I/O-port BAR0 alive
 * for guests that don't grok the modern MMIO capability layout.
 *
 * Legacy PCI configuration registers (offsets from BAR0 I/O base):
 *
 *   0x00  uint32   Device features (RO)   — what the host supports
 *   0x04  uint32   Driver features        — what we (the guest) acked
 *   0x08  uint32   Queue address (PFN)    — phys >> 12 of vq.desc page
 *   0x0C  uint16   Queue size (RO)        — number of descriptors
 *   0x0E  uint16   Queue select           — which vq we're configuring
 *   0x10  uint16   Queue notify           — write qidx to kick host
 *   0x12  uint8    Device status          — ACK / DRIVER / OK / FAIL
 *   0x13  uint8    ISR status (RC)        — read-and-clear pending IRQ
 *   0x14  ...      Device-specific config — varies by device class
 *
 * Each virtqueue has three pieces laid out in guest physical memory:
 *
 *   descriptor[qsize]                      — 16-byte entries (addr/len/flags/next)
 *   avail ring                             — guest -> host: descriptor heads to process
 *   used ring                              — host -> guest: completed descriptors
 *
 * The avail ring is page-aligned with the descriptor table (we put
 * them in one page each, the used ring on the next page); the host
 * reads desc-PFN, then computes avail-PFN and used-PFN from the spec
 * layout.
 *
 * Sources:
 *   - virtio 1.0 spec, §4.1 (legacy PCI transport), §2.4 (virtqueues)
 *   - QEMU source: hw/virtio/virtio-pci.c (transitional fallbacks)
 */
#ifndef ADVENTOS_VIRTIO_H
#define ADVENTOS_VIRTIO_H

#include "../include/types.h"

/* PCI vendor ID for all virtio devices. */
#define VIRTIO_VENDOR_ID         0x1AF4

/* Legacy/transitional PCI device IDs. The transitional bridge in
 * QEMU keeps these IDs alive for back-compat with guests that don't
 * use the modern (1.0+) layout.
 *
 * (Modern-only IDs are 0x1040+T where T is the device type; not
 * targeted by this driver.) */
#define VIRTIO_LEGACY_NET        0x1000
#define VIRTIO_LEGACY_BLK        0x1001
#define VIRTIO_LEGACY_BALLOON    0x1002
#define VIRTIO_LEGACY_CONSOLE    0x1003
#define VIRTIO_LEGACY_SCSI       0x1004
#define VIRTIO_LEGACY_RNG        0x1005
#define VIRTIO_LEGACY_9P         0x1009

/* Legacy PCI I/O register offsets from BAR0. */
#define VIRTIO_PCI_HOST_FEATURES   0x00
#define VIRTIO_PCI_GUEST_FEATURES  0x04
#define VIRTIO_PCI_QUEUE_PFN       0x08
#define VIRTIO_PCI_QUEUE_NUM       0x0C
#define VIRTIO_PCI_QUEUE_SEL       0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10
#define VIRTIO_PCI_STATUS          0x12
#define VIRTIO_PCI_ISR             0x13
#define VIRTIO_PCI_CONFIG          0x14   /* device-specific config base */

/* Status register bits (driver -> device handshake). */
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FAILED       128

/* Descriptor flag bits. */
#define VIRTQ_DESC_F_NEXT          1
#define VIRTQ_DESC_F_WRITE         2     /* host writes (device-input desc) */
#define VIRTQ_DESC_F_INDIRECT      4

/* Available-ring flag bits. */
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1

/* Descriptor (16 bytes). 64-bit addr field gets the upper half left
 * at zero since AdventOS is 32-bit. */
struct virtq_desc {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

/* Available ring: guest -> host. flags + idx + ring[qsize] + used_event. */
struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];                /* qsize entries */
    /* uint16_t used_event;          (only if VIRTIO_F_EVENT_IDX) */
} __attribute__((packed));

/* Used-ring entry: 8 bytes. */
struct virtq_used_elem {
    uint32_t id;                    /* head desc index */
    uint32_t len;                   /* total bytes written into the chain */
} __attribute__((packed));

/* Used ring: host -> guest. flags + idx + ring[qsize] + avail_event. */
struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];  /* qsize entries */
    /* uint16_t avail_event;         (only if VIRTIO_F_EVENT_IDX) */
} __attribute__((packed));

/* In-kernel state for one virtqueue. */
struct virtqueue {
    uint16_t            qsize;       /* number of descriptors in the ring */
    uint16_t            qidx;        /* which queue this is (0..N-1) */
    uint16_t            free_head;   /* head of free-descriptor linked list */
    uint16_t            n_free;      /* descriptors currently free */
    uint16_t            last_used;   /* last used.idx we processed */
    struct virtq_desc  *desc;        /* qsize entries */
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint32_t            ring_phys;   /* physical PFN-shifted base */
};

/* Read the device feature bits and ack a (driver, device) intersection.
 * `wanted` is the bitmask of features we (the driver) understand. The
 * function reads HOST_FEATURES, computes (host & wanted), writes that
 * to GUEST_FEATURES, and returns it. */
uint32_t virtio_negotiate(uint16_t io_base, uint32_t wanted);

/* Status helpers — set bits in the device status register. */
void     virtio_status_reset    (uint16_t io_base);
void     virtio_status_ack      (uint16_t io_base);
void     virtio_status_driver   (uint16_t io_base);
void     virtio_status_driver_ok(uint16_t io_base);
void     virtio_status_failed   (uint16_t io_base);

/* Allocate, zero, and program a virtqueue at index `qidx`. Returns
 * 0 + fills `*vq`, or -1 on failure (queue size 0 or alloc failed).
 * The queue is physically contiguous and the PFN is written into
 * VIRTIO_PCI_QUEUE_PFN at the appropriate qidx. */
int      virtio_queue_init(uint16_t io_base, uint16_t qidx,
                           struct virtqueue *vq);

/* Allocate a free descriptor and return its index (0..qsize-1) or
 * -1 if the queue is exhausted. */
int      virtio_alloc_desc(struct virtqueue *vq);

/* Return a chain of descriptors (head..tail-via-NEXT) to the free
 * pool. Updates n_free accordingly. */
void     virtio_free_desc_chain(struct virtqueue *vq, uint16_t head);

/* Push descriptor `head` onto the avail ring and kick the host.
 * Caller is responsible for having filled the descriptor(s) in. */
void     virtio_submit(uint16_t io_base, struct virtqueue *vq, uint16_t head);

/* Wait (busy-poll) until used.idx > vq->last_used, with a deadline
 * in PIT ticks. Returns 0 on completion (and updates last_used +
 * frees the descriptor chain), -1 on timeout. */
int      virtio_wait_used(uint16_t io_base, struct virtqueue *vq,
                          uint32_t timeout_ms);

/* Install a PCI INTx handler for a virtio device. The optional `fn`
 * is called from IRQ context on every interrupt where this device's
 * ISR-status bit 0 is set (= "queue had completions"). Used by
 * virtio-net / virtio-console for RX drain. Pass fn = NULL for
 * sync-only drivers (blk / rng / 9p / balloon) — the handler still
 * reads VIRTIO_PCI_ISR to clear the latch so the line doesn't keep
 * firing, but does no other work; the caller's hlt-based wait sees
 * used.idx advance and proceeds.
 *
 * Multiple devices can share a PCI IRQ line. The first install on
 * a given IRQ installs a master dispatcher into isr_register_irq;
 * subsequent installs just register additional slots. The
 * dispatcher iterates slots on each IRQ and only calls `fn` for
 * slots whose ISR fired. */
void     virtio_install_irq(uint16_t io_base, int irq,
                            void (*fn)(void *), void *cookie);

#endif
