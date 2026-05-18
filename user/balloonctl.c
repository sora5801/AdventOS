/*
 * balloonctl — show virtio-balloon stats. The target (number of
 * pages the host wants us to surrender) is controlled from the QEMU
 * monitor:  (qemu) balloon 24
 *
 * Output:
 *   actual:   ballooned pages currently held by host
 *   target:   pages the host last requested (config.num_pages)
 *   inflated: cumulative pages ever surrendered
 *   deflated: cumulative pages ever taken back
 *
 * For the actual size in MiB just multiply by 4 (each page = 4 KiB).
 */
#include "libuser.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    unsigned int s[4];
    int rc = sys_virtio_balloon_stats(s);
    if (rc < 0) {
        puts("balloonctl: no virtio-balloon device "
             "(boot QEMU with -device virtio-balloon-pci)\n");
        return 1;
    }
    printf("actual:   %u pages (%u KiB)\n", s[0], s[0] * 4);
    printf("target:   %u pages (%u KiB)\n", s[1], s[1] * 4);
    printf("inflated: %u pages cumulative\n", s[2]);
    printf("deflated: %u pages cumulative\n", s[3]);
    return 0;
}
