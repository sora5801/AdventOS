/*
 * Block-device registry — see blkdev.h.
 */
#include "blkdev.h"

static struct blkdev *g_devs[BLKDEV_MAX];
static int            g_n_devs;

int blkdev_register(struct blkdev *d) {
    if (g_n_devs >= BLKDEV_MAX) return -1;
    g_devs[g_n_devs] = d;
    return g_n_devs++;
}

struct blkdev *blkdev_get(int idx) {
    if (idx < 0 || idx >= g_n_devs) return 0;
    return g_devs[idx];
}

int blkdev_count(void) { return g_n_devs; }
