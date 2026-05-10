# Session 37 — AC97 sound + PCM playback

**Goal:** Make the box make noise. Until session 36, every output device AdventOS could touch was visual or textual: VGA text, the linear framebuffer, serial. This session adds the Intel ICH AC97 audio codec — a PCI device QEMU emulates with `-device AC97` — and wires a `SYS_AUDIO_PLAY` syscall that lets a ring-3 program queue 16-bit signed stereo PCM at 48 kHz to it. A user program (`beep`) generates sine-wave tones via integer math and plays a 4-note C-major arpeggio that's recovered to within 1% of the right pitch when QEMU's audio output is captured to a WAV file.

End state — the new boot lines:

```
ac97: found at PCI 0:4.0  NAM=0xc000  NABM=0xc400  IRQ=11
ac97: codec ready, 128 KiB staging, BDL @ phys 0xd000
```

And the new `[t27]` selftest:

```
[t27] AC97 audio: play a 4-note arpeggio via sys_audio_play
beep: queued 200 ms of 262 Hz (38400 bytes)
beep: queued 200 ms of 330 Hz (38400 bytes)
beep: queued 200 ms of 392 Hz (38400 bytes)
beep: queued 200 ms of 523 Hz (38400 bytes)
  beep.elf tune exited (code=0) — 0 = AC97 played; -1 = no AC97
```

Captured WAV (via `-audiodev wav,id=snd0,path=beep.wav`):

| Note | Expected (Hz) | Measured (Hz) | Burst |
|------|--------------:|--------------:|--------|
| C4   | 262           | 260           | 0.000s..0.200s |
| E4   | 330           | 330           | 0.298s..0.498s |
| G4   | 392           | 390           | 0.595s..0.795s |
| C5   | 523           | 520           | 0.898s..1.098s |

Each burst is exactly 200 ms (matching the per-note duration `beep tune` emits), with ~98 ms gaps between bursts (the time `beep` spends in `sys_sleep_ms` between notes plus a small queue/drain gap). Frequencies measured by counting zero-crossings of the left channel within each burst: all within 1% of the requested pitch. The audio path goes user → `sys_audio_play` → `ac97_play` → BDL DMA → emulated I/O ports → QEMU AC97 model → WAV file on the host.

## What's in scope

In:

- **`kernel/ac97.{h,c}`** — AC97 driver (~250 LOC). PCI find for vendor 0x8086 device 0x2415 (Intel ICH AC97 — what QEMU emulates). Codec cold/warm reset. Mixer config (full master + PCM-out volume, set sample rate). Buffer Descriptor List (BDL) of 32 entries, one staging page per slot (32 × 4 KiB = 128 KiB total). DMA bus master via the NABM I/O ports.
- **`kernel/pci.h/c`** — extended `struct pci_device` with `bar1` / `io_base1`. AC97 needs both NAM (mixer, BAR0) and NABM (bus master, BAR1) which live in different PCI BARs.
- **`kernel/syscall.{h,c}`** — `SYS_AUDIO_PLAY = 56` (`ebx` = ptr, `ecx` = n_bytes). Returns bytes accepted or -1 if no AC97. Format is fixed: 16-bit signed LE stereo at 48 kHz; n must be a multiple of 4 (one stereo sample).
- **`user/libuser.{h,c}`** — `sys_audio_play(buf, n)` wrapper.
- **`user/beep.c`** (~150 LOC) — sine-tone generator. 256-entry quarter-wave table (only the [0, π/2) quadrant; reflections give the other three). Phase accumulator in fixed-point (Q24.8). 10ms linear fade-in/out to suppress click artifacts. Two modes: `beep [hz]` plays one tone; `beep tune` plays a C-major arpeggio (262, 330, 392, 523 Hz @ 200ms each).
- **`mkfs.py`** — `beep.elf` added to the FS image.
- **`build.sh`** — `beep` joins the user-program list; suggested QEMU command updated to include `-device AC97 -audiodev sdl,id=snd0`.
- **`fs/sh.c`** — `[t27]` selftest forks `beep tune`.

Out:

- **Recording / capture.** AC97 PI (PCM In) box and MC (Mic In) box are present in the spec but we leave them masked — sound goes one way for now.
- **MIDI / synthesis.** beep.c generates raw PCM sine waves; no MIDI parser, no FM/wavetable synth, no envelope.
- **WAV file decoder.** `beep` doesn't load a WAV — it generates samples on the fly. A `wavplay` user program would just `mmap` the file and feed the data section to `sys_audio_play`.
- **AEAD / multi-stream / mixing.** Single source of audio, single stream. The kernel's `ac97_play` is single-task; if two processes call it concurrently they race the BDL.
- **Variable Rate Audio (VRA).** Real hardware codecs without VRA support reject the rate-set register write and stay at 48 kHz. We just write 48000 and rely on that being the default; never check.
- **IRQ-driven refill.** The driver's queue-then-drain model means each `ac97_play` call resets the DMA, queues up to 32 × 4 KiB = ~680 ms of staged audio, starts DMA, and returns. Continuous streams >680ms would need either bigger staging or an IRQ handler that refills slots as DMA passes them. Out of scope for one session; the inaudible sub-millisecond gap between `ac97_play` calls is fine for tones and short clips.
- **Hardware-specific feature detection.** No EAPD, no headphone-jack sensing, no per-codec quirks. We talk to the vanilla Intel ICH AC97 the QEMU model emulates.

## Architecture

```
                      USER (beep.c)
                      ─────────────
   gen_sine(buf, frames, hz)
     phase += hz * 65536 / 48000 each frame
     buf[2i+0] = buf[2i+1] = SINE_TABLE[phase>>8]
   apply_fades(buf)
   sys_audio_play(buf, frames * 4)   ────► int $0x80 SYS_AUDIO_PLAY (56)
                                                          │
                                                          ▼
                                                ac97_play(pcm, n)  KERNEL
                                                  │
                                                  ▼
                                          1. CR.RR = 1     (reset PCM-Out box)
                                          2. for each 4 KiB chunk:
                                              copy chunk → g_stage[slot]
                                              g_bdl[slot] = {addr=g_stage[slot],
                                                             samples=chunk/2,
                                                             ctl=0}
                                              slot++
                                          3. LVI = slot - 1
                                          4. CR.RPBM = 1   (start DMA)
                                          ▼
                                       AC97 BUS MASTER (in I/O ports)
                                          │
                                          ▼
                                  Reads g_bdl[CIV], DMAs samples to codec
                                          │
                                          ▼
                                  CIV++; if CIV > LVI then DCH=1, halt
                                          │
                                  hardware AC97 silicon (= QEMU model)
                                          │
                                          ▼
                                  -audiodev sdl/wav/sdl/none → speaker / file
```

## PCI find + register map

Intel ICH AC97 advertises as `vendor=0x8086, device=0x2415`. `pci_find` walks bus 0 looking for a match, sets bus-master + I/O enables in the PCI command register, and returns BAR0 (NAM = mixer) and BAR1 (NABM = bus master).

QEMU's emulation places the device at PCI 0:4.0 with NAM at I/O port 0xC000 and NABM at 0xC400, IRQ 11. Real hardware uses different addresses — the BAR mechanism is exactly what shields us from caring.

The two BARs decode to two register banks:

```
NAM (BAR0) — codec mixer registers (16-bit each)
  0x00  RESET                 write any value to reset codec
  0x02  Master Volume         L att[12:8] | mute[15] | R att[4:0]
  0x18  PCM-Out Volume        same shape
  0x2C  PCM Front DAC Rate    sample rate in Hz (only with VRA)

NABM (BAR1) — bus master registers
  0x10  PO_BDBAR              PCM-Out BDL base addr (32-bit phys)
  0x14  PO_CIV                Current Index Value (5 bits, RO)
  0x15  PO_LVI                Last Valid Index (5 bits, RW)
  0x16  PO_SR                 Status (DCH, CELV, FIFO err, ...)
  0x18  PO_PICB               Position In Current Buffer (16-bit, RO)
  0x1B  PO_CR                 Control: bit0 = RPBM (run), bit1 = RR (reset)
  0x2C  GLOB_CNT              global control (cold reset, IRQ enable)
  0x30  GLOB_STA              global status (codec ready, ...)
```

Init sequence:

```c
nabm_w32(NABM_GLOB_CNT, GLOB_CNT_COLD_RESET);
/* Wait up to 50ms for primary codec ready (GLOB_STA bit 8) */

nam_w16(NAM_RESET, 1);                      /* mixer reset */
nam_w16(NAM_MASTER_VOL, 0x0000);            /* full volume, unmuted */
nam_w16(NAM_PCM_OUT_VOL, 0x0000);
nam_w16(NAM_PCM_FRONT_DAC_RATE, 48000);     /* may be no-op without VRA */

nabm_w8(NABM_PO_CR, CR_RR);                 /* PCM-Out reset (self-clears) */
/* Wait for RR to clear */
nabm_w32(NABM_PO_BDBAR, (uint32_t)g_bdl);   /* point at BDL */
```

Volume `0x0000` means "0 dB attenuation, not muted" — the loudest possible setting on the codec. Real applications would expose volume via a mixer interface.

## Buffer Descriptor List + DMA

The AC97 PCM-Out engine reads from a 32-entry "Buffer Descriptor List" living in physical memory:

```c
struct ac97_bdle {
    uint32_t addr;          /* phys ptr to PCM data buffer */
    uint16_t samples;       /* COUNT OF 16-BIT SAMPLES, NOT BYTES, NOT FRAMES */
    uint16_t ctl;           /* bit15 = IOC, bit14 = BUP */
} __attribute__((packed));
```

The "samples" count is the gotcha. It's not bytes (which would be `frames * 4` for stereo 16-bit). It's not stereo-frames (which would be `bytes / 4`). It's literally the count of 16-bit values: `bytes / 2`. For our 4 KiB chunk of stereo 16-bit at 48 kHz, that's 4096 / 2 = 2048 samples = 1024 stereo frames = 21.3 ms of audio.

The DMA engine reads the BDL by index. CIV ("current index value") is the entry currently being played. LVI ("last valid index") is the highest entry with valid data. The engine plays through CIV+1, CIV+2, ... until CIV > LVI, then sets the DCH ("DMA controller halted") status bit and waits.

We allocate one PMM page per BDL slot (32 × 4 KiB = 128 KiB) so each slot has its own staging buffer. ac97_play copies user data into staging slots, fills BDL entries, sets LVI, and writes CR.RPBM = 1 to start.

## The "queue then start" insight

The first version of `ac97_play` interleaved BDL writes and DMA starts — every chunk would update LVI to its own slot index and write CR.RPBM:

```c
/* WRONG — race-prone version */
for each chunk:
    install BDL[slot]
    nabm_w8(NABM_PO_LVI, slot);
    nabm_w8(NABM_PO_CR, CR_RPBM);
```

This produced 16 ms of audio out of an expected 250 ms. Captured WAV showed a single tone burst, then silence.

The reason: after the first iteration writes `LVI = 0`, DMA starts and immediately finishes slot 0 (4 KiB = 21 ms ish, but the staged play happens fast — the sample buffer is small relative to one I/O write). CIV becomes 1, the engine looks for entry 1, finds whatever was in BDL[1] (initial memset to all-zero: addr=0, samples=0). With samples=0 the engine skips ahead instantly. It runs through the whole 32-entry ring of zero-samples entries in microseconds, sets DCH, and stops.

Meanwhile our second loop iteration is still busy with `memcpy` for chunk 2. By the time we write `LVI = 1`, the DMA has already halted with DCH=1 — and writing LVI alone doesn't reliably resume play on QEMU's emulation. The result: only the first slot's data (and not all of it) reaches the codec.

The fix is the "queue all then start" pattern: stop the DMA (CR.RR = 1, self-clearing), fill ALL valid BDL slots, set LVI to the last filled slot, then write CR.RPBM = 1. The engine starts with all-correct state and plays through to LVI before halting. With STAGING_NUM_PAGES = 32 we can stage up to 680 ms of audio per call, which covers any single tone we'd want to play.

For continuous streams >680 ms, the right answer is an IRQ-driven refiller hooked into the AC97 IRQ line — every IOC interrupt lets us bump LVI ahead of CIV with newly-staged buffers. The current design is single-shot per `ac97_play` call which suits short clips perfectly and degrades gracefully (sub-millisecond gaps between consecutive calls, inaudible) for short concatenated playback. `beep tune`'s 4 × 200ms notes use this path.

## The user-side sine generator

`beep.c` makes tones without `libm`:

```c
/* 256-entry sine table; we store only [0, π/2) — 64 entries —
 * and reflect via quadrant for the other three quarters. */
static const short SINE_QUARTER[64] = { 0, 201, ..., 7167, 7138, 7104 };

static int sine256(int i) {
    i &= 0xFF;
    int q = i >> 6;        /* quadrant 0..3 */
    int j = i & 0x3F;      /* 0..63 */
    switch (q) {
        case 0: return  SINE_QUARTER[j];
        case 1: return  SINE_QUARTER[63 - j];
        case 2: return -SINE_QUARTER[j];
        default: return -SINE_QUARTER[63 - j];
    }
}

static int gen_sine(short *out, int nframes, int freq_hz) {
    /* Phase accumulator in Q24.8: top byte indexes the table,
     * fractional bits eliminate aliasing at non-integer ratios. */
    unsigned int delta = (unsigned int)freq_hz * 65536u / SAMPLE_RATE;
    unsigned int phase = 0;
    for (int i = 0; i < nframes; i++) {
        int s = sine256(phase >> 8);
        out[2*i + 0] = (short)s;       /* left  */
        out[2*i + 1] = (short)s;       /* right */
        phase += delta;
    }
    return nframes * BYTES_PER_FRAME;
}
```

Amplitude = 0.22 × full-scale (~7224 / 32767) — comfortably loud through headphones, well below clip threshold. The fade-in and fade-out (10 ms each, applied via linear gain ramp) eliminate the click that an abrupt amplitude transition would produce.

For "tune" mode, we just call `play_tone` four times with C4, E4, G4, C5 frequencies. `play_tone` allocates the PCM buffer, generates samples, calls `sys_audio_play`, sleeps the tone duration plus 50 ms (to ensure DMA finishes before the next call resets the engine), and frees.

## Verified output

QEMU's `-audiodev wav,id=snd0,path=beep.wav,out.frequency=48000` writes the codec output to a WAV file. After `[t27]` runs:

```
$ python -c "..."
WAV: rate=48000
  audio @ t=0.000s..0.200s (200.0ms)    ← C4 burst
  audio @ t=0.298s..0.498s (200.0ms)    ← E4 burst
  audio @ t=0.595s..0.795s (200.0ms)    ← G4 burst
  audio @ t=0.898s..1.098s (200.0ms)    ← C5 burst
```

Each burst is exactly 200 ms wide, with consistent gaps (~98 ms — the `sys_sleep_ms(ms+50)` between notes plus a few ms of queue setup overhead).

Counting zero-crossings of the left channel within each burst gives the actual frequency:

| Burst | Note | Expected (Hz) | Measured (Hz) | Error |
|------:|------|--------------:|--------------:|------:|
| 1     | C4   | 262           | 260           | -0.8% |
| 2     | E4   | 330           | 330           | 0.0% |
| 3     | G4   | 392           | 390           | -0.5% |
| 4     | C5   | 523           | 520           | -0.6% |

The small consistent under-shoot is integer-truncation in `delta = hz * 65536 / 48000`. For 262 Hz, `delta = 357` exactly, but the rounded phase accumulation produces an effective frequency of `357 * 48000 / 65536 = 261.5 Hz`. Within 1% — fine for a beep. A more careful implementation would use `delta = hz * 65536 / 48000 + (extra fractional carry bits)` for sub-Hz precision.

## Bugs

**1. The race-prone interleaved-LVI bug.** Documented above — initial implementation set LVI per slot and DMA halted on the first all-zero "garbage" BDL entry past LVI=0. Fix: queue all slots then set LVI once.

**2. Staging-page reuse mid-DMA.** First "fix" was to make the BDL ring smaller than 32 (BDL_NUM_ENTRIES = STAGING_NUM_PAGES = 8). But the AC97 hardware CIV register is always 5 bits = 32-entry ring, regardless of how many entries WE choose to populate. After the engine plays slot 7, CIV becomes 8 and the engine starts reading from BDL[8] — which we'd left as memset(0). DMA reads garbage from physical address 0 with samples=0 → skips → CIV=9 → … → cycles. Result: only slot 0 actually played. Fixed by going back to 32 BDL slots with 32 staging pages, matching the hardware ring.

**3. WAV recorder default frequency.** QEMU's `-audiodev wav` defaults to 44.1 kHz output, which silently resamples our 48 kHz codec output. Files were 1.78 MB and the captured audio was pitch-shifted. Fix: `-audiodev wav,id=snd0,path=beep.wav,out.frequency=48000` to record at the codec's actual rate. Without this, the analysis script computed slightly-too-high frequencies that didn't match the requested notes.

## Build + run

```bash
$ bash build.sh                # builds os.img + beep.elf
$ qemu-system-i386 -drive format=raw,file=os.img \
    -serial stdio -m 32 -smp 2 \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
    -audiodev sdl,id=snd0 \
    -device AC97,audiodev=snd0
```

`-audiodev sdl` outputs to the host's audio device (you'll hear the beeps). `-audiodev none` runs silently. `-audiodev wav,id=snd0,path=out.wav,out.frequency=48000` writes to a WAV file for offline analysis.

## What's left

- **Audio input.** AC97 has PI and MC boxes for line-in and microphone. Same BDL machinery, same I/O ports (different offsets). About a day's work to add `sys_audio_record(buf, n)`.
- **IRQ-driven refill.** Today's queue-then-drain works for short clips. Real audio applications (music, voice) need continuous play, which means an IRQ handler hooked into the AC97 IRQ line. On every `IOC` interrupt the handler advances the staged data and bumps LVI. ~50 LOC.
- **Multi-stream mixing.** Currently a single producer calls `ac97_play`. Two producers race the BDL. A kernel-side software mixer (read user PCM via syscall, mix into a master buffer, hand to AC97) would let multiple programs play simultaneously. Or expose AC97 directly as `/dev/dsp` and let an in-userspace mixer (a la PulseAudio) do the work.
- **WAV / OGG / MP3 decoding.** None of these in libc. WAV is trivial to add (~30 LOC of header parser + raw PCM forward to `sys_audio_play`). OGG/MP3 need real codecs.
- **Volume control syscall.** Right now master volume is hardcoded to maximum at boot. A `sys_audio_volume(0..100)` would write to `NAM_MASTER_VOL` on demand.
- **Sample-rate conversion.** All input must be exactly 48 kHz stereo. A real driver would resample (linear interpolation suffices for most uses).
- **`/dev/dsp`-style file abstraction.** Today the only way to play sound is `sys_audio_play`. A character device backed by AC97 would let user programs write PCM via standard `write(fd, buf, n)`.

## Files touched

- `kernel/ac97.h`, `kernel/ac97.c` (new, ~290 LOC)
- `kernel/pci.h`, `kernel/pci.c` — added `bar1` / `io_base1` to struct
- `kernel/syscall.h`, `kernel/syscall.c` — `SYS_AUDIO_PLAY = 56`
- `kernel/kernel.c` — `ac97_init()` after PIT
- `user/libuser.h`, `user/libuser.c` — `sys_audio_play()`
- `user/beep.c` (new, ~150 LOC) — sine-tone generator
- `user/sh.c` — `[t27]` selftest
- `mkfs.py` — adds `beep.elf`
- `build.sh` — `beep` joined `USER_PROGS`; sample QEMU command updated
- `docs/37-ac97-pcm.md` — this document

About 600 LOC of new code, 30 LOC of existing-file deltas. The driver is the bulk; the user demo and tests are about 200 LOC together.
