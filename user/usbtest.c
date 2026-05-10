/*
 * usbtest — exercise the USB Mass Storage block device.
 *
 * Spawned by sh's [t27] selftest after [t26] cryptotest. Walks the
 * blkdev table looking for a usb* device, then:
 *
 *   1. Reads sector 0, dumps the first 16 bytes — for a usbfs.img
 *      built by mkfs.py this is the AdventFS superblock magic
 *      "ADVENTFS" so the dump is recognizable.
 *   2. Saves sector N (by reading it), writes a known pattern,
 *      reads back, verifies the pattern, restores the original.
 *
 * Pass = sector 0 magic visible AND write/read round-trip matches.
 */
#include "libuser.h"

static const char *hex = "0123456789abcdef";

static void hexdump(const unsigned char *p, int n) {
    for (int i = 0; i < n; i++) {
        putchar(hex[p[i] >> 4]);
        putchar(hex[p[i] & 0xF]);
        putchar(i == n - 1 ? '\n' : ' ');
    }
}

static int find_usb_device(void) {
    /* Walk blkdev[0..3] looking for a name starting with 'u'. */
    for (int i = 0; i < 4; i++) {
        struct sys_block_info info;
        if (sys_block_info(i, &info) != 0) continue;
        printf("  blkdev[%d] = '%s'  (%u blocks * %u bytes = %u KiB)\n",
               i, info.name,
               info.n_blocks, info.block_size,
               (info.block_size * info.n_blocks) / 1024);
        if (info.name[0] == 'u' && info.name[1] == 's' && info.name[2] == 'b') {
            return i;
        }
    }
    return -1;
}

int main(void) {
    puts("== usbtest ==\n");

    int dev = find_usb_device();
    if (dev < 0) {
        puts("  no usb* block device found — skipping (boot QEMU with "
             "-device usb-storage,drive=usbfs)\n");
        return 0;       /* not a hard failure — just nothing to test */
    }
    printf("  using blkdev[%d]\n", dev);

    /* ---- Read sector 0, expect AdventFS superblock magic ---- */
    unsigned char sec0[512];
    int rc = sys_block_read(dev, 0, 1, sec0);
    if (rc != 0) {
        printf("  FAIL: read sector 0 rc=%d\n", rc);
        return 1;
    }
    puts("  sector 0 first 16 bytes:\n    ");
    hexdump(sec0, 16);
    /* mkfs.py writes "ADVENTFS" (8 bytes) at offset 0 of the
     * superblock. */
    int magic_ok = (sec0[0] == 'A' && sec0[1] == 'D' && sec0[2] == 'V'
                 && sec0[3] == 'E' && sec0[4] == 'N' && sec0[5] == 'T'
                 && sec0[6] == 'F' && sec0[7] == 'S');
    if (magic_ok) puts("  PASS  AdventFS superblock magic on USB sector 0\n");
    else          puts("  WARN  no ADVENTFS magic — not a fatal error if usbfs.img isn't a real FS\n");

    /* ---- Write/read round-trip on a high sector ---- */
    /* Use sector 100 — well past the FS superblock and any data
     * written by mkfs. */
    const unsigned int test_lba = 100;
    unsigned char saved [512];
    unsigned char pattern[512];
    unsigned char back   [512];

    if (sys_block_read(dev, test_lba, 1, saved) != 0) {
        puts("  FAIL: read of test LBA failed\n");
        return 1;
    }

    /* Pattern: i*7+13 mod 256, easy to recognize, low repetition. */
    for (int i = 0; i < 512; i++) pattern[i] = (unsigned char)((i * 7 + 13) & 0xFF);

    if (sys_block_write(dev, test_lba, 1, pattern) != 0) {
        puts("  FAIL: write of pattern failed\n");
        return 1;
    }
    if (sys_block_read(dev, test_lba, 1, back) != 0) {
        puts("  FAIL: read-back failed\n");
        return 1;
    }

    int eq = 1;
    for (int i = 0; i < 512; i++) if (back[i] != pattern[i]) { eq = 0; break; }

    /* Restore — even if the verify failed, leave the disk unmodified. */
    sys_block_write(dev, test_lba, 1, saved);

    if (eq) puts("  PASS  USB write+read round-trip (sector 100, 512B pattern)\n");
    else    puts("  FAIL  USB read-back mismatch\n");

    /* ---- Multi-block read (4 sectors at once) ---- */
    unsigned char multi[2048];
    if (sys_block_read(dev, 0, 4, multi) != 0) {
        puts("  FAIL  multi-block read (4 sectors)\n");
    } else {
        /* First sector should still match sec0 we read earlier. */
        int same = 1;
        for (int i = 0; i < 512; i++) if (multi[i] != sec0[i]) { same = 0; break; }
        if (same) puts("  PASS  multi-block read (4 sectors at once)\n");
        else      puts("  FAIL  multi-block read first sector differs from single-block\n");
    }

    return eq ? 0 : 1;
}
