/*
 * aplay — sound consumer for the AC97 codec. Reads PCM data (either
 * from a file or from stdin via the pipeline) and feeds it to
 * sys_audio_play in 4 KiB chunks so a long clip plays continuously
 * without buffering the whole thing in memory.
 *
 * Two input modes:
 *
 *   1) `aplay <file>` — opens the file, peeks 12 bytes, and:
 *      - if the first 4 bytes are "RIFF" and bytes 8..11 are "WAVE",
 *        parses the WAV header to find the data chunk + verifies the
 *        format matches what the AC97 driver expects (PCM int16
 *        little-endian stereo 48 kHz)
 *      - otherwise treats the whole file as raw PCM
 *
 *   2) `aplay` (no args) — reads raw PCM from stdin (fd 0). Useful
 *      for piping output of generators: `gen-sine | aplay`.
 *
 * The kernel driver wants exactly:
 *   - 16-bit signed samples
 *   - little-endian byte order
 *   - stereo (interleaved L,R,L,R,...)
 *   - 48 kHz sample rate
 *
 * For a WAV file with different parameters we just warn and play
 * anyway; the output will sound wrong but the user can hear that
 * something's coming out.
 */
#include "libuser.h"

#define CHUNK     4096
#define WAV_HDR   44     /* canonical PCM WAV header — we don't parse
                            optional chunks, just the standard 44-byte
                            prefix Audacity / ffmpeg produce */

/* Read up to `max` bytes from `fd`, looping until EOF or max. Returns
 * bytes read (>=0) or -1 on error. */
static int read_full(int fd, void *buf, int max) {
    char *p = (char *)buf;
    int total = 0;
    while (total < max) {
        int n = sys_read(fd, p + total, max - total);
        if (n <= 0) break;
        total += n;
    }
    return total;
}

/* Parse a 44-byte canonical PCM WAV header. Returns the byte offset
 * of the data chunk on success (= 44 for canonical), -1 if not a WAV.
 * Fills *out_channels, *out_rate, *out_bps for the caller to warn. */
static int parse_wav_header(const unsigned char *hdr,
                            int *out_channels, int *out_rate, int *out_bps)
{
    if (hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F') return -1;
    if (hdr[8] != 'W' || hdr[9] != 'A' || hdr[10] != 'V' || hdr[11] != 'E') return -1;
    if (hdr[12] != 'f' || hdr[13] != 'm' || hdr[14] != 't' || hdr[15] != ' ') return -1;
    /* fmt chunk is at offset 12, length at 16..19 (= 16 for PCM).
     * channels  at 22..23, sample-rate at 24..27, bps at 34..35. */
    *out_channels = (int)hdr[22] | ((int)hdr[23] << 8);
    *out_rate     = (int)hdr[24] | ((int)hdr[25] << 8)
                  | ((int)hdr[26] << 16) | ((int)hdr[27] << 24);
    *out_bps      = (int)hdr[34] | ((int)hdr[35] << 8);
    /* For a canonical PCM file the "data" chunk follows fmt at offset
     * 36 (4 bytes id, 4 bytes size). Non-canonical files can have
     * other chunks ("LIST", "JUNK") before data; we don't handle
     * those. */
    if (hdr[36] != 'd' || hdr[37] != 'a' || hdr[38] != 't' || hdr[39] != 'a') {
        return -1;     /* not canonical — fall through to raw PCM */
    }
    return 44;
}

/* Read CHUNK bytes at a time from fd and push to sys_audio_play.
 * Stops at EOF. Returns 0 on success, -1 if any audio_play fails. */
static int play_stream(int fd, int already_read, const void *initial) {
    static unsigned char buf[CHUNK];
    int total = 0;

    /* If we pre-read some bytes (e.g. the WAV header detection probe),
     * play those first. */
    if (already_read > 0 && initial) {
        const unsigned char *p = (const unsigned char *)initial;
        for (int i = 0; i < already_read; i++) buf[i] = p[i];
        /* Top up to CHUNK from fd. */
        int more = read_full(fd, buf + already_read, CHUNK - already_read);
        int len  = already_read + more;
        /* Round down to multiple of 4 (one stereo 16-bit sample). */
        len &= ~3;
        if (len > 0) {
            if (sys_audio_play(buf, len) < 0) {
                puts("aplay: sys_audio_play failed (no AC97?)\n");
                return -1;
            }
            total += len;
        }
        if (more < CHUNK - already_read) goto done;  /* EOF reached */
    }

    for (;;) {
        int n = read_full(fd, buf, CHUNK);
        if (n <= 0) break;
        n &= ~3;
        if (n == 0) break;
        if (sys_audio_play(buf, n) < 0) {
            puts("aplay: sys_audio_play failed mid-stream\n");
            return -1;
        }
        total += n;
    }

done:
    /* DMA needs time to drain the staging buffers — sleep based on
     * the data we pushed. 48 kHz * 4 bytes per frame = 192000 B/s. */
    int ms = total / 192;     /* 192 bytes per ms */
    if (ms > 0) sys_sleep_ms(ms + 50);
    printf("aplay: queued %d bytes (~%d ms)\n", total, ms);
    return 0;
}

int main(int argc, char **argv) {
    int fd;
    int from_stdin = (argc < 2);

    if (from_stdin) {
        fd = 0;        /* stdin */
    } else {
        fd = sys_open(argv[1]);
        if (fd < 0) {
            printf("aplay: cannot open %s\n", argv[1]);
            return 1;
        }
    }

    /* Probe the first 44 bytes — if it's a canonical PCM WAV header,
     * skip it and warn on format mismatch. */
    unsigned char probe[WAV_HDR];
    int got = read_full(fd, probe, WAV_HDR);

    if (got >= WAV_HDR) {
        int chans = 0, rate = 0, bps = 0;
        int data_off = parse_wav_header(probe, &chans, &rate, &bps);
        if (data_off > 0) {
            printf("aplay: WAV  channels=%d  rate=%d  bps=%d\n",
                   chans, rate, bps);
            if (chans != 2 || rate != 48000 || bps != 16) {
                puts("aplay: WARN — AC97 wants 16-bit 48 kHz stereo; "
                     "output will sound wrong\n");
            }
            /* The WAV header is consumed; the remaining file is pure
             * PCM. play_stream needs no initial bytes. */
            int rc = play_stream(fd, 0, 0);
            if (!from_stdin) sys_close(fd);
            return rc < 0 ? 1 : 0;
        }
        /* Not a WAV — treat probe as the first 44 bytes of raw PCM. */
    }

    int rc = play_stream(fd, got, probe);
    if (!from_stdin) sys_close(fd);
    return rc < 0 ? 1 : 0;
}
