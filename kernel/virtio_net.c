/*
 * virtio-net driver — see virtio_net.h.
 *
 * Wire format (virtio 1.0 §5.1, without MRG_RXBUF negotiated):
 *
 *   struct virtio_net_hdr {
 *       u8  flags;            // VIRTIO_NET_HDR_F_*
 *       u8  gso_type;         // VIRTIO_NET_HDR_GSO_NONE
 *       u16 hdr_len;          // checksum stuff (we don't use)
 *       u16 gso_size;
 *       u16 csum_start;
 *       u16 csum_offset;
 *   } __attribute__((packed));      // exactly 10 bytes
 *
 * The header is prepended to every TX packet and stripped from every
 * RX packet. We zero the whole header on TX (no offloads); on RX we
 * just skip the first 10 bytes.
 *
 * Two virtqueues:
 *   queue 0 = RX (guest receives) — pre-filled with empty buffers
 *   queue 1 = TX (guest transmits)
 *
 * RX strategy: pre-allocate N_RX_BUFFERS buffers of 1536 bytes each,
 * push all of them into the avail ring at init, then a kernel polling
 * task drains the used ring every 20 ms (low enough latency for TCP
 * over SLIRP, high enough not to dominate CPU when idle).
 *
 * TX strategy: serialize via a spinlock, allocate one descriptor for
 * the header and one for the frame data, push, busy-wait for the
 * used ring to advance, free the descriptors.
 */
#include "virtio_net.h"
#include "virtio.h"
#include "pci.h"
#include "net.h"
#include "eth.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "string.h"
#include "spinlock.h"
#include "pit.h"
#include "task.h"
#include "../include/io.h"

/* Feature bits we want — just VIRTIO_NET_F_MAC so config space has
 * the MAC. Anything else (CSUM offload, GSO, MRG_RXBUF, STATUS) we
 * leave un-negotiated to keep the wire format simple. */
#define VIRTIO_NET_F_MAC      (1u << 5)

/* QEMU's legacy virtio-net uses the 10-byte virtio_net_hdr without
 * num_buffers when VIRTIO_NET_F_MRG_RXBUF is NOT negotiated. (The
 * spec text on legacy headers is ambiguous; the controlling behavior
 * is what QEMU expects, which is host_hdr_len = guest_hdr_len = 10
 * in this configuration. With MRG_RXBUF negotiated both jump to 12.) */
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

#define VNET_HDR_SIZE  10
#define VNET_BUF_SIZE  1536
#define N_RX_BUFFERS   16

/* Per-device state. Only one virtio-net is supported (kept static so
 * the polling task can find it without indirection). */
struct vnet {
    int               in_use;
    struct pci_device pci;
    uint16_t          io;

    struct virtqueue  rx_vq;          /* qidx 0 */
    struct virtqueue  tx_vq;          /* qidx 1 */

    struct mac_addr   mac;

    /* RX buffer table — indexed by descriptor index. Each entry is
     * a kmalloc'd VNET_BUF_SIZE buffer. */
    uint8_t          *rx_buf[N_RX_BUFFERS];

    /* TX is serialized via this lock. We use a single 1536-byte buffer
     * for the header+frame; eth_send already copies onto its own stack
     * before calling net_send_frame so this extra copy is fine. */
    spinlock_t        tx_lock;
    uint8_t          *tx_buf;
};

static struct vnet g_vnet;

/* ---- RX queue setup ------------------------------------------- */

/* Push descriptor `idx` (one writable buffer) onto the RX avail ring
 * and bump avail.idx. Does NOT kick — the caller does that after
 * batching. */
static void rx_arm_descriptor(struct vnet *v, int idx) {
    v->rx_vq.desc[idx].addr_lo = (uint32_t)(uintptr_t)v->rx_buf[idx];
    v->rx_vq.desc[idx].addr_hi = 0;
    v->rx_vq.desc[idx].len     = VNET_BUF_SIZE;
    v->rx_vq.desc[idx].flags   = VIRTQ_DESC_F_WRITE;
    v->rx_vq.desc[idx].next    = 0;

    uint16_t slot = (uint16_t)(v->rx_vq.avail->idx % v->rx_vq.qsize);
    v->rx_vq.avail->ring[slot] = (uint16_t)idx;
    v->rx_vq.avail->idx        = (uint16_t)(v->rx_vq.avail->idx + 1);
}

static int rx_init(struct vnet *v) {
    /* Allocate N_RX_BUFFERS RX buffers. */
    for (int i = 0; i < N_RX_BUFFERS; i++) {
        v->rx_buf[i] = kmalloc(VNET_BUF_SIZE);
        if (!v->rx_buf[i]) return -1;
    }
    /* Mark all descriptors as not-free in our pool (we own them) —
     * virtio_alloc_desc never returns them. */
    v->rx_vq.free_head = 0xFFFFu;
    v->rx_vq.n_free    = 0;

    /* Push every descriptor into the avail ring. */
    for (int i = 0; i < N_RX_BUFFERS; i++) {
        rx_arm_descriptor(v, i);
    }
    /* Memory barrier (compiler) then kick the host so it knows RX is
     * armed. */
    __asm__ volatile ("" ::: "memory");
    outw(v->io + VIRTIO_PCI_QUEUE_NOTIFY, v->rx_vq.qidx);
    return 0;
}

/* ---- RX drain ------------------------------------------------- */

static void rx_drain(struct vnet *v) {
    while ((uint16_t)(v->rx_vq.used->idx - v->rx_vq.last_used) != 0) {
        uint16_t slot = (uint16_t)(v->rx_vq.last_used % v->rx_vq.qsize);
        struct virtq_used_elem ue = v->rx_vq.used->ring[slot];
        uint16_t idx = (uint16_t)ue.id;
        uint32_t total_len = ue.len;

        if (idx < N_RX_BUFFERS && total_len > VNET_HDR_SIZE) {
            /* Skip the virtio_net_hdr prefix; hand the Ethernet
             * frame up the stack. */
            const uint8_t *frame = v->rx_buf[idx] + VNET_HDR_SIZE;
            uint32_t frame_len   = total_len - VNET_HDR_SIZE;
            net_rx_frame(frame, frame_len);
        }

        /* Re-arm the same descriptor. */
        rx_arm_descriptor(v, idx);
        v->rx_vq.last_used = (uint16_t)(v->rx_vq.last_used + 1);
    }
    /* One kick per drain batch is enough. */
    __asm__ volatile ("" ::: "memory");
    outw(v->io + VIRTIO_PCI_QUEUE_NOTIFY, v->rx_vq.qidx);
}

/* IRQ-context shim. Called from virtio.c's master dispatcher when
 * this device's ISR signals queue activity. Walking the RX used
 * ring from IRQ context is fine — rtl8139's IRQ handler does the
 * same shape (hand-up frames via net_rx_frame which descends into
 * the TCP/UDP stack under net_lock). */
static void virtio_net_irq_drain(void *cookie) {
    rx_drain((struct vnet *)cookie);
}

void virtio_net_start_polling(void) {
    /* IRQ-driven now (see virtio_install_irq call in virtio_net_init).
     * Kept under the old name + symbol for kmain compatibility — the
     * polling task that used to live here is gone. */
    if (g_vnet.in_use) {
        kprintf("virtio-net: RX is IRQ-driven (IRQ %u)\n",
                (unsigned)g_vnet.pci.irq_line);
    }
}

/* ---- TX ------------------------------------------------------- */

int virtio_net_send(const void *frame, uint32_t len) {
    struct vnet *v = &g_vnet;
    if (!v->in_use) return -1;
    if (len == 0 || len > 1518) return -1;

    spin_lock(&v->tx_lock);

    /* Build header + frame in our TX buffer. */
    memset(v->tx_buf, 0, VNET_HDR_SIZE);
    memcpy(v->tx_buf + VNET_HDR_SIZE, frame, len);

    if (v->tx_vq.n_free < 2) {
        /* Drain any completed TX descriptors back to the pool. */
        while ((uint16_t)(v->tx_vq.used->idx - v->tx_vq.last_used) != 0) {
            uint16_t slot = (uint16_t)(v->tx_vq.last_used % v->tx_vq.qsize);
            uint16_t head = (uint16_t)v->tx_vq.used->ring[slot].id;
            virtio_free_desc_chain(&v->tx_vq, head);
            v->tx_vq.last_used = (uint16_t)(v->tx_vq.last_used + 1);
        }
        if (v->tx_vq.n_free < 2) {
            spin_unlock(&v->tx_lock);
            return -1;
        }
    }

    int d0 = virtio_alloc_desc(&v->tx_vq);
    int d1 = virtio_alloc_desc(&v->tx_vq);
    if (d0 < 0 || d1 < 0) {
        if (d0 >= 0) virtio_free_desc_chain(&v->tx_vq, (uint16_t)d0);
        if (d1 >= 0) virtio_free_desc_chain(&v->tx_vq, (uint16_t)d1);
        spin_unlock(&v->tx_lock);
        return -1;
    }

    /* desc[0] = header, device reads, chain to desc[1] */
    v->tx_vq.desc[d0].addr_lo = (uint32_t)(uintptr_t)v->tx_buf;
    v->tx_vq.desc[d0].addr_hi = 0;
    v->tx_vq.desc[d0].len     = VNET_HDR_SIZE;
    v->tx_vq.desc[d0].flags   = VIRTQ_DESC_F_NEXT;
    v->tx_vq.desc[d0].next    = (uint16_t)d1;

    /* desc[1] = frame data, device reads, end of chain */
    v->tx_vq.desc[d1].addr_lo = (uint32_t)(uintptr_t)(v->tx_buf + VNET_HDR_SIZE);
    v->tx_vq.desc[d1].addr_hi = 0;
    v->tx_vq.desc[d1].len     = len;
    v->tx_vq.desc[d1].flags   = 0;
    v->tx_vq.desc[d1].next    = 0;

    virtio_submit(v->io, &v->tx_vq, (uint16_t)d0);

    /* Wait for completion (1 s budget — TX on SLIRP completes in
     * microseconds; this is just a deadlock guard). */
    int rc = virtio_wait_used(v->io, &v->tx_vq, 1000);
    spin_unlock(&v->tx_lock);
    if (rc != 0) {
        kprintf("virtio-net: TX timeout, len=%u\n", len);
        return -1;
    }
    return (int)len;
}

/* ---- init ----------------------------------------------------- */

int virtio_net_init(struct mac_addr *out_mac) {
    struct vnet *v = &g_vnet;
    if (v->in_use) return -1;

    if (pci_find(VIRTIO_VENDOR_ID, VIRTIO_LEGACY_NET, &v->pci) != 0) {
        return -1;     /* not present — caller falls back */
    }
    v->io = v->pci.io_base;
    spin_lock_init(&v->tx_lock);

    kprintf("virtio-net: PCI %u:%u.%u  io=0x%x  irq=%u  subsys=0x%x\n",
            v->pci.bus, v->pci.device, v->pci.func,
            (unsigned)v->io, v->pci.irq_line, v->pci.subsystem_id);

    virtio_status_reset(v->io);
    virtio_status_ack(v->io);
    virtio_status_driver(v->io);

    uint32_t feats = virtio_negotiate(v->io, VIRTIO_NET_F_MAC);
    int has_mac = (feats & VIRTIO_NET_F_MAC) != 0;

    /* Set up RX (qidx 0) and TX (qidx 1). */
    if (virtio_queue_init(v->io, 0, &v->rx_vq) != 0) {
        kprintf("virtio-net: RX queue setup failed\n");
        virtio_status_failed(v->io);
        return -1;
    }
    if (virtio_queue_init(v->io, 1, &v->tx_vq) != 0) {
        kprintf("virtio-net: TX queue setup failed\n");
        virtio_status_failed(v->io);
        return -1;
    }

    /* Install our IRQ handler BEFORE DRIVER_OK so we don't miss the
     * first RX completion. */
    virtio_install_irq(v->io, v->pci.irq_line, virtio_net_irq_drain, v);

    /* Tell the device we're done configuring before we start posting
     * RX buffers — some virtio implementations gate ring access on
     * DRIVER_OK. */
    virtio_status_driver_ok(v->io);

    /* Read MAC from device config space. Legacy layout: 6 bytes
     * starting at io+VIRTIO_PCI_CONFIG. */
    if (has_mac) {
        for (int i = 0; i < 6; i++) {
            v->mac.b[i] = inb(v->io + VIRTIO_PCI_CONFIG + i);
        }
    } else {
        /* No VIRTIO_NET_F_MAC — pick a locally-administered address. */
        v->mac.b[0] = 0x52; v->mac.b[1] = 0x54; v->mac.b[2] = 0x00;
        v->mac.b[3] = 0x12; v->mac.b[4] = 0x34; v->mac.b[5] = 0x56;
    }

    /* Allocate TX buffer. */
    v->tx_buf = kmalloc(VNET_BUF_SIZE);
    if (!v->tx_buf) {
        kprintf("virtio-net: TX buf alloc failed\n");
        return -1;
    }

    /* Pre-fill RX descriptors. */
    if (rx_init(v) != 0) {
        kprintf("virtio-net: RX buf alloc failed\n");
        return -1;
    }

    v->in_use = 1;
    if (out_mac) *out_mac = v->mac;

    kprintf("virtio-net: link up — MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            v->mac.b[0], v->mac.b[1], v->mac.b[2],
            v->mac.b[3], v->mac.b[4], v->mac.b[5]);
    return 0;
}
