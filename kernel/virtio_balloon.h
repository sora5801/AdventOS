/*
 * virtio-balloon — cooperative memory pressure between guest and
 * host. The host advertises "I want N more pages from you" via the
 * `num_pages` field in config space; the guest responds by allocating
 * pages from PMM and reporting them via the INFLATE virtqueue. If
 * the host wants to give pages back (num_pages < actual), the guest
 * removes them from its ballooned set via the DEFLATE virtqueue.
 *
 * QEMU CLI to enable:
 *   -device virtio-balloon-pci
 *
 * Then set the target from the QEMU monitor:
 *   (qemu) balloon 24       # ask guest to inflate down to 24 MiB
 *
 * Stats are exposed via SYS_VIRTIO_BALLOON_STATS:
 *   out[0] = actual pages ballooned
 *   out[1] = target pages (host's `num_pages`)
 *   out[2] = total pages ever inflated (cumulative counter)
 *   out[3] = total pages ever deflated (cumulative counter)
 *
 * Implementation cap: BALLOON_MAX_PAGES (= 1024 = 4 MiB). With 32 MiB
 * RAM total, we never give back more than ~12% of the system.
 */
#ifndef ADVENTOS_VIRTIO_BALLOON_H
#define ADVENTOS_VIRTIO_BALLOON_H

#include "../include/types.h"

void virtio_balloon_init(void);

/* Spawn the cooperation task. Called from kmain after task_init. */
void virtio_balloon_start_task(void);

/* Stats fill (used by SYS_VIRTIO_BALLOON_STATS). Returns 0 / -1. */
int  virtio_balloon_get_stats(uint32_t out[4]);

#endif
