/*
 * beep — generate a short tone (or simple tune) and play it via the
 * AC97 codec. PCM format: 16-bit signed little-endian stereo at 48
 * kHz, matching what kernel/ac97.c hands the hardware unmodified.
 *
 * Usage:
 *   beep            — A4 (440 Hz) for 250ms
 *   beep <freq_hz>  — that frequency for 250ms
 *   beep tune       — a 4-note arpeggio (C major: C E G C')
 *
 * Each "frame" of PCM is two int16: { left, right }. Samples are a
 * sine wave generated incrementally via small integer math (no
 * floats — we're freestanding ring 3 with no libm). The sine table
 * is 256 entries, scaled to int16 amplitude ~25% of full-scale to
 * avoid clipping any downstream mixer.
 */
#include "libuser.h"

#define SAMPLE_RATE   48000
#define CHANNELS      2
#define BITS_PER_SAMP 16
#define BYTES_PER_FRAME (CHANNELS * BITS_PER_SAMP / 8)   /* = 4 */
#define BYTES_PER_SEC (SAMPLE_RATE * BYTES_PER_FRAME)    /* = 192000 */

/* 256-entry quarter-wave sine table (only the [0, π/2) quadrant —
 * the others are reflections). Values are amplitude * 32767. We use
 * amplitude 0.25 (= 8192) for a comfortable listening level — full
 * scale is unpleasantly loud through speakers. */
static const short SINE_QUARTER[64] = {
    /* sin(i * π/128) for i in 0..63, scaled to ~8192 (0x2000) */
       0,   201,   401,   601,   801,  1000,  1198,  1394,
    1590,  1785,  1978,  2169,  2359,  2547,  2734,  2918,
    3100,  3279,  3457,  3631,  3803,  3972,  4138,  4301,
    4460,  4617,  4769,  4918,  5063,  5205,  5342,  5475,
    5604,  5728,  5848,  5963,  6074,  6181,  6282,  6379,
    6470,  6557,  6639,  6716,  6787,  6853,  6914,  6970,
    7020,  7065,  7104,  7138,  7166,  7189,  7206,  7218,
    7224,  7224,  7218,  7207,  7190,  7167,  7138,  7104,
};

/* Look up sin(2π * i / 256) scaled to ~7224. Folds the four
 * quadrants down to the table. */
static int sine256(int i) {
    i &= 0xFF;                      /* wrap to one period */
    int q = i >> 6;                 /* quadrant 0..3 */
    int j = i & 0x3F;               /* 0..63 */
    switch (q) {
        case 0: return  SINE_QUARTER[j];
        case 1: return  SINE_QUARTER[63 - j];
        case 2: return -SINE_QUARTER[j];
        default: return -SINE_QUARTER[63 - j];
    }
}

/* Generate `nframes` of a sine wave at `freq_hz` into `out`.
 * Returns bytes written. Phase increments by `freq_hz * 256 /
 * SAMPLE_RATE` per frame — small enough that integer arithmetic
 * has plenty of headroom. We use a 24-bit "phase accumulator" so
 * we can advance fractionally per frame without losing precision. */
static int gen_sine(short *out, int nframes, int freq_hz) {
    /* delta = freq * 65536 * 256 / SAMPLE_RATE
     * Phase is a Q24.8 value — top byte indexes the table, bottom
     * is a fractional rounding helper. */
    unsigned int delta = (unsigned int)freq_hz * 65536u / SAMPLE_RATE;
    unsigned int phase = 0;
    for (int i = 0; i < nframes; i++) {
        int s = sine256(phase >> 8);
        out[2*i + 0] = (short)s;     /* left  */
        out[2*i + 1] = (short)s;     /* right */
        phase += delta;
    }
    return nframes * BYTES_PER_FRAME;
}

/* Apply a brief linear fade-in/out (10ms each end) to suppress click
 * at start/stop. */
static void apply_fades(short *buf, int nframes) {
    int fade = SAMPLE_RATE / 100;     /* 10 ms */
    if (fade > nframes / 2) fade = nframes / 2;
    for (int i = 0; i < fade; i++) {
        int gain = (i << 8) / fade;   /* 0..255 */
        buf[2*i + 0] = (short)((buf[2*i + 0] * gain) >> 8);
        buf[2*i + 1] = (short)((buf[2*i + 1] * gain) >> 8);
    }
    for (int i = 0; i < fade; i++) {
        int gain = (i << 8) / fade;
        int j = nframes - 1 - i;
        buf[2*j + 0] = (short)((buf[2*j + 0] * gain) >> 8);
        buf[2*j + 1] = (short)((buf[2*j + 1] * gain) >> 8);
    }
}

/* Play a single tone for `ms` milliseconds at `freq_hz`. Allocates
 * a buffer big enough for the full tone, generates samples, sends
 * the whole thing through SYS_AUDIO_PLAY. */
static int play_tone(int freq_hz, int ms) {
    int nframes  = SAMPLE_RATE * ms / 1000;
    int nbytes   = nframes * BYTES_PER_FRAME;
    short *buf   = malloc(nbytes);
    if (!buf) {
        puts("beep: malloc failed\n");
        return -1;
    }
    gen_sine(buf, nframes, freq_hz);
    apply_fades(buf, nframes);

    int wrote = sys_audio_play(buf, nbytes);
    free(buf);
    if (wrote < 0) {
        puts("beep: sys_audio_play returned -1 — no AC97 codec?\n");
        return -1;
    }
    printf("beep: queued %d ms of %d Hz (%d bytes)\n", ms, freq_hz, wrote);
    /* Sleep just longer than the tone duration so the tone finishes
     * before we exit (DMA keeps reading from staging buffers; if we
     * exit too early the next sys_audio_play call clobbers them). */
    sys_sleep_ms(ms + 50);
    return 0;
}

int main(int argc, char **argv) {
    int freq = 440;     /* A4 */
    int ms   = 250;

    if (argc >= 2) {
        if (strcmp(argv[1], "tune") == 0) {
            /* Simple C-major arpeggio. */
            int notes[4] = { 262, 330, 392, 523 };  /* C4 E4 G4 C5 */
            for (int i = 0; i < 4; i++) {
                if (play_tone(notes[i], 200) < 0) return 1;
            }
            return 0;
        }
        freq = atoi(argv[1]);
        if (freq < 50 || freq > 8000) {
            puts("beep: frequency out of range (50..8000)\n");
            return 1;
        }
    }

    return play_tone(freq, ms) < 0 ? 1 : 0;
}
