/*
 * virtio-rng — paravirtualized entropy source. One request queue;
 * the driver hands the host an empty buffer and the host fills it
 * with random bytes.
 *
 * QEMU CLI to enable:
 *   -object rng-random,id=rng0,filename=/dev/urandom \
 *   -device virtio-rng-pci,rng=rng0
 *
 * On Windows hosts, replace rng-random with rng-builtin (uses
 * QEMU's own PRNG) since /dev/urandom doesn't exist:
 *   -object rng-builtin,id=rng0 \
 *   -device virtio-rng-pci,rng=rng0
 *
 * Exposes a single primitive — virtio_rng_get(buf, len) — that
 * blocks until the host fills the buffer. SYS_GETRANDOM in the
 * syscall layer wraps it.
 */
#ifndef ADVENTOS_VIRTIO_RNG_H
#define ADVENTOS_VIRTIO_RNG_H

#include "../include/types.h"

void virtio_rng_init(void);

/* Returns 1 if a virtio-rng device is present + ready, 0 otherwise.
 * Callers should fall back to PIT/timing entropy if 0. */
int  virtio_rng_available(void);

/* Pull `len` bytes of host entropy into `buf`. Returns bytes
 * obtained (== len on success) or -1 on transport failure /
 * device-not-present. Caller-blocking; uses the same hlt-based
 * yield as the rest of the virtio common layer. */
int  virtio_rng_get(void *buf, int len);

#endif
