#ifndef ADVENTOS_AC97_H
#define ADVENTOS_AC97_H

#include "../include/types.h"

/*
 * AC97 audio driver. Targets Intel ICH AC97 (vendor 0x8086,
 * device 0x2415) — the codec QEMU emulates with `-device AC97`.
 *
 * Format: 16-bit signed little-endian stereo PCM at 48 kHz.
 * That's 192,000 bytes/sec; ~4 KiB = 21 ms of audio.
 *
 * Output path:
 *   user/beep.c calls sys_audio_play(buf, n_bytes)
 *   → kernel ac97_play(buf, n_bytes)
 *     copies into a kernel-side staging page (identity-mapped phys)
 *     pushes a Buffer Descriptor List entry pointing at it
 *     advances LVI (last-valid-index)
 *     CR.RUN bit drives DMA
 *
 * This is a "blocking-ish" driver: ac97_play returns when the
 * data has been queued, NOT when it has finished playing. The
 * underlying DMA engine plays asynchronously while the CPU does
 * other work.
 */

#define AC97_SAMPLE_RATE   48000
#define AC97_CHANNELS      2
#define AC97_BITS          16
/* Bytes per second of PCM. */
#define AC97_BYTES_PER_SEC (AC97_SAMPLE_RATE * AC97_CHANNELS * AC97_BITS / 8)

/* One-time bring-up: PCI find, codec reset, mixer config, BDL alloc.
 * Idempotent — calling more than once is harmless. Logs status via
 * kprintf. If no AC97 is present the driver stays quiet and
 * audio_play returns -1 forever. */
void ac97_init(void);

/* Returns 1 if an AC97 codec was found and initialized successfully,
 * 0 otherwise. The audio syscalls all check this before doing
 * anything. */
int  ac97_available(void);

/* Queue raw PCM data for playback. Returns bytes accepted (= n on
 * success), or -1 if the device isn't available. The buffer is
 * COPIED into kernel-side staging memory; the caller's buffer can
 * be reused immediately on return. n must be a multiple of 4
 * (= one stereo 16-bit sample). */
int  ac97_play(const void *pcm, int n);

/* Stop playback, drain DMA. Used by tear-down, not normal flow. */
void ac97_stop(void);

#endif
