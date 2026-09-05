# MILESTONE 01 — Audio capture path VALIDATED

Date: 2026-09-05 · Covers EXP-000 through EXP-004

The master instruction (§4, "NEVER FAKE SUCCESS") lists what must be verified before audio
capture may be called working. Each item below is checked against the experiment that
established it. Nothing here rests on "it compiled" or "bytes arrived".

| Required check | Result | Evidence |
|---|---|---|
| Expected sample rate | **16,001.60 Hz (+0.010 %)** | EXP-003 — frames counted against `esp_timer` |
| Expected channel count | mono, LEFT (L/R tied to GND) | EXP-001 wiring + `I2S_CHANNEL_FMT_ONLY_LEFT` |
| Expected sample format | 24-bit in 32-bit slot | EXP-003 — low byte zero on **all 80,000** samples |
| Expected sample count | exact, no short reads | EXP-002 126/126 blocks; EXP-004 400,000 frames |
| Expected duration | 24.09 s captured vs 24.0 s expected | EXP-004 |
| Non-zero signal | floor 54,844 AC, never zero | EXP-002B |
| Realistic amplitude | speech peak 1,452 / 32,767 | EXP-004 |
| Speech waveform | ~8 discrete bursts = counting aloud | EXP-004 envelope |
| Silence waveform | stable floor, pre/post agree **1.22x** | EXP-004 |
| Noise behaviour | characterised: 64.6 % sub-100 Hz drift | EXP-003 spectrum |
| Absence of clipping | **0 clipped samples** in every capture | EXP-002/2B/3/4 |
| Reasonable DC offset | mean 0.175 % FS; drift identified as 0.2-2.4 Hz | EXP-003 |
| Correct WAV metadata | 16 kHz mono 16-bit, plays at correct duration | EXP-004 |
| **Correct PCM interpretation** | **user confirmed: speech clear, pitch and speed natural** | EXP-004 listening test |

## Why the listening test mattered most

Wrong sample rate, a half-rate or double-rate channel misread, or a byte-order error would
all still produce plausible statistics — non-zero signal, sensible RMS, a floor, a
response to sound. Every number in this project would have looked healthy.

But they cannot survive a human ear: the voice would play chipmunk-fast or slowed and
deepened. The user confirmed the recording sounds **natural in pitch and speed**, which
independently corroborates the measured 16,001.60 Hz and the 24-in-32 alignment through a
completely different channel of evidence.

**PHASES 2-5 ARE COMPLETE AND EXPERIMENTALLY VALIDATED.**

## What is still NOT proven

- No keyword detection of any kind exists.
- No feature extraction (Mel/MFCC) has been built.
- Broadband RMS was proven to be an inadequate detector (EXP-004, 9.6x — failed its
  pre-declared 10x bar).
- The residual 41 % sub-100 Hz energy in quiet has not been reduced further.
- Nothing about latency, CPU load, or model accuracy has been measured. No claims exist.
