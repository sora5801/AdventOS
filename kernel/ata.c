#include "ata.h"
#include "blkdev.h"
#include "spinlock.h"
#include "../include/io.h"

#define ATA_PRIMARY_BASE  0x1F0
#define ATA_PRIMARY_CTRL  0x3F6

#define ATA_REG_DATA      (ATA_PRIMARY_BASE + 0)
#define ATA_REG_ERROR     (ATA_PRIMARY_BASE + 1)
#define ATA_REG_SECCOUNT  (ATA_PRIMARY_BASE + 2)
#define ATA_REG_LBA0      (ATA_PRIMARY_BASE + 3)
#define ATA_REG_LBA1      (ATA_PRIMARY_BASE + 4)
#define ATA_REG_LBA2      (ATA_PRIMARY_BASE + 5)
#define ATA_REG_DRIVE     (ATA_PRIMARY_BASE + 6)
#define ATA_REG_STATUS    (ATA_PRIMARY_BASE + 7)
#define ATA_REG_COMMAND   (ATA_PRIMARY_BASE + 7)

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_FLUSH         0xE7

#define POLL_LIMIT 1000000u

static spinlock_t g_lock = SPINLOCK_INIT;

/* 4× control-register read = ~400ns of guaranteed delay, the canonical
 * way to give the drive a moment after writes to the drive-select reg. */
static inline void ata_400ns(void) {
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
}

static int ata_wait_not_busy(void) {
    for (uint32_t i = 0; i < POLL_LIMIT; i++) {
        uint8_t s = inb(ATA_REG_STATUS);
        if (!(s & ATA_SR_BSY)) return 0;
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < POLL_LIMIT; i++) {
        uint8_t s = inb(ATA_REG_STATUS);
        if (s & ATA_SR_ERR)                       return -1;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

/* ---- blkdev adapter ------------------------------------------- */

static int ata_blkdev_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf) {
    (void)d;
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < n; i++) {
        if (ata_read_sector(lba + i, p + i * 512) != 0) return -1;
    }
    return 0;
}
static int ata_blkdev_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf) {
    (void)d;
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < n; i++) {
        if (ata_write_sector(lba + i, p + i * 512) != 0) return -1;
    }
    return 0;
}

static struct blkdev g_ata_blkdev = {
    .name        = "ata0",
    .block_size  = ATA_SECTOR_SIZE,
    /* IDENTIFY would give us the real LBA28 size; for the demo
     * disk (~140 MiB worth of sectors per QEMU's default), we just
     * declare a large bound. The kernel never reads past the FS
     * area set up by mkfs.py, and bcache doesn't range-check. */
    .n_blocks    = 0x100000,    /* 512 MiB worth of sectors */
    .read        = ata_blkdev_read,
    .write       = ata_blkdev_write,
    .driver_data = 0,
};

void ata_init(void) {
    spin_lock_init(&g_lock);
    /* Mask the controller's IRQ line — we'll only use polling. Bit 1
     * of the device-control register is nIEN. */
    outb(ATA_PRIMARY_CTRL, 0x02);
    blkdev_register(&g_ata_blkdev);
}

static int ata_setup_lba28(uint32_t lba) {
    if (ata_wait_not_busy() != 0) return -1;
    outb(ATA_REG_DRIVE,    0xE0 | (uint8_t)((lba >> 24) & 0x0F));
    ata_400ns();
    outb(ATA_REG_SECCOUNT, 1);
    outb(ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_REG_LBA1, (uint8_t)((lba >>  8) & 0xFF));
    outb(ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    return 0;
}

int ata_read_sector(uint32_t lba, void *buf) {
    spin_lock(&g_lock);

    if (ata_setup_lba28(lba) != 0) { spin_unlock(&g_lock); return -1; }
    outb(ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    if (ata_wait_drq() != 0) { spin_unlock(&g_lock); return -1; }

    uint16_t *p = (uint16_t *)buf;
    for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
        p[i] = inw(ATA_REG_DATA);
    }

    spin_unlock(&g_lock);
    return 0;
}

int ata_write_sector(uint32_t lba, const void *buf) {
    spin_lock(&g_lock);

    if (ata_setup_lba28(lba) != 0) { spin_unlock(&g_lock); return -1; }
    outb(ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    if (ata_wait_drq() != 0) { spin_unlock(&g_lock); return -1; }

    const uint16_t *p = (const uint16_t *)buf;
    for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
        outw(ATA_REG_DATA, p[i]);
    }

    /* The controller may continue writing for a while; flush so the
     * data is durably committed before we declare success. */
    if (ata_wait_not_busy() != 0) { spin_unlock(&g_lock); return -1; }
    outb(ATA_REG_COMMAND, ATA_CMD_FLUSH);
    if (ata_wait_not_busy() != 0) { spin_unlock(&g_lock); return -1; }

    spin_unlock(&g_lock);
    return 0;
}
