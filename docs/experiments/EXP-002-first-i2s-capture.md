# EXP-002 — First INMP441 I2S Capture (raw data inspection)

| Field | Value |
|---|---|
| Experiment ID | EXP-002 |
| Date | 2026-09-05 |
| Phase | 2-3 (I2S capture / raw PCM inspection) |
| Status | **INCONCLUSIVE** (superseded by EXP-002B) |
| Artifacts | `EXP-002-raw-capture.log`, `code/exp002_raw_i2s_capture.cpp` |

## Configuration

Legacy `driver/i2s.h` (ESP-IDF 4.4.7). Master RX, 16 kHz, `I2S_BITS_PER_SAMPLE_32BIT`,
`I2S_CHANNEL_FMT_ONLY_LEFT`, `I2S_COMM_FORMAT_STAND_I2S`, 8 DMA buffers x 256 frames.
Pins SCK=6, WS=5, SD=4. 512 frames per read (32 ms). 32 s capture, 126 data blocks.

## Results against pre-declared criteria

| # | Criterion | Result | Verdict |
|---|---|---|---|
| 1 | driver install + set_pin | both `ESP_OK` | PASS |
| 2 | 2048 bytes / 512 frames per read | 126/126 blocks exact, 0 short reads | PASS |
| 3 | Data alive (not zeros/stuck) | `flat=1`, `zeros=0` after first block | PASS |
| 4 | Silence floor > 0 | mean RMS 54,844 (0.65% FS) | PASS |
| 5 | Responds to sound (>=10x) | 16.3x observed, but stimulus was UNCONTROLLED | **UNVALIDATED** |
| 6 | No gross clipping | **0** clipped blocks of 126 | PASS |
| 7 | DC offset small vs RMS | **66/126 blocks DC-dominated** (\|mean\| > 0.7·rms) | **FAIL** |

## Measurements

| Window | mean RMS | max RMS |
|---|---|---|
| 0-4 s (startup) | 776,024 | 2,533,517 |
| 4-8 s (quiet) | 59,395 | 98,310 |
| 12-28 s (active) | 178,679 | 893,710 |
| 28-32 s (quiet) | 42,413 | 62,495 |

Overall RMS dynamic range across capture: **103x**. Quiet floor ~0.65% FS; loudest
observed 10.7% FS. Never approached full scale.

## Confirmed by evidence (not assumption)

**24-bit-in-32-bit slot alignment is correct.** 504 raw 32-bit words were sampled from the
log; **every single one had low byte `00`**. This is the padding the INMP441 appends after
its 24 data bits, so `raw >> 8` is the correct recovery of a signed 24-bit sample.

## CORRECTION (post-hoc, 2026-09-05)

The user confirmed afterwards that they **performed no actions during this capture** — no
silence phase, no speech, no tapping. The 32 s therefore recorded uncontrolled ambient
room sound.

Consequence: the 16.3x RMS variation is real, but it cannot be attributed to a known
stimulus. Criterion 5 ("responds to sound") is **NOT satisfied** — the loud windows may
have been any ambient event. The earlier PARTIAL PASS verdict was wrong and is withdrawn.

What survives this correction (all independent of any stimulus): criteria 1, 2, 3, 6, the
24-in-32 bit alignment proof, and the DC-offset finding. Those rest on block accounting and
raw word inspection, not on what the user did.

## Anomalies

1. **Large low-frequency / DC wander.** Block means swing between roughly -2.2e6 and
   +2.5e6 within a few hundred ms, while the *within-block* spread stays far narrower.
   The signal is therefore riding on a large, slowly-moving offset rather than being
   centred on zero.
2. **Startup transient.** The first ~2-4 s show RMS up to 2.5e6 (~30% FS), far above the
   settled floor. A settling/discard period is needed before any measurement is trusted.

## What this proves

The I2S peripheral, the confirmed wiring, the channel selection and the bit alignment are
all correct, and the microphone is a live acoustic sensor that responds to sound with no
clipping.

## What this does NOT prove

- **The actual sample rate was never measured.** 16 kHz is configured, not verified.
- No time-domain waveform or spectrum has been inspected, so the nature of the
  low-frequency content (mains hum? sub-audio drift? artifact?) is unknown.
- Nothing has been listened to. No WAV exists.

## Decision

**REPEAT as EXP-002B** with a controlled, time-anchored stimulus protocol before any
progression. Then EXP-003.

Superseded decision (no longer valid): **PROCEED to EXP-003**, which must: (a) measure the true sample rate empirically,
(b) transfer raw PCM to the PC, (c) inspect waveform and spectrum to diagnose the
low-frequency component before any preprocessing is designed.
