# EXP-003 — Sample-rate verification + spectral diagnosis

| Field | Value |
|---|---|
| Experiment ID | EXP-003 |
| Date | 2026-09-05 |
| Phase | 3-5 (raw PCM inspection / signal analysis) |
| Status | **PASS** |
| Artifacts | `EXP-003-ambient-5s.pcm` (320,000 B), `EXP-003-signal-analysis.png`, `code/exp003_rate_and_pcm_dump.cpp` |

## Objective

(1) Measure the TRUE sample rate, never previously verified. (2) Transfer raw PCM to the
PC and identify the low-frequency component that made the EXP-002B detector unreliable.
No user action required — ambient room capture.

## Objective 1 — sample rate: PASS

Measured by counting DMA-delivered frames against `esp_timer` wall clock:

```
frames = 80,384   elapsed = 5.0235 s
configured = 16000 Hz
MEASURED   = 16001.60 Hz   (error +0.010 %)
```

**The hardware is genuinely sampling at 16 kHz.** A +0.010 % error is far below anything
that matters for KWS (it would take ~0.5 % to start distorting Mel features). Every
downstream stage can now safely assume 16 kHz. This was previously an assumption; it is
now a measurement.

## Transfer integrity

320,000 bytes expected, **320,000 received (100 %)**, `END_PCM` marker intact. The 24-bit
alignment held across the entire capture: `low byte all zero: True` for all 80,000 samples
(EXP-002 only sampled 504 words; this checks every one).

## Objective 2 — what the low-frequency component IS

| Measurement | Value |
|---|---|
| DC mean | 14,672 (**0.175 % FS**) |
| raw RMS | 104,852 (1.2499 % FS) |
| AC RMS | 103,820 (1.2376 % FS) |
| min / max | −507,016 / 353,926 |
| clipped samples | **0 / 80,000** |

**The DC mean is small.** So the block-to-block swings seen in EXP-002/002B were *not* a
fixed DC offset. Energy distribution:

| Band | % of total energy |
|---|---|
| 0–10 Hz | **31.14 %** |
| 10–50 Hz | 2.94 % |
| 50–100 Hz | 30.49 % |
| 100–300 Hz | 33.98 % |
| **300–3400 Hz (speech)** | **1.43 %** |
| 3400–8000 Hz | 0.02 % |

**64.57 % of all energy sits below 100 Hz.** The strongest spectral peaks are at
**0.20, 0.40, 0.60, 1.60, 2.20, 2.40 Hz** — sub-audio. A 0.5 Hz wave inside a 32 ms
analysis block looks like a constant offset, which is precisely why the per-block means
swung between ±2.5e6 while the within-block spread stayed narrow. The mystery is solved:
**it is sub-audio drift/rumble, not DC offset and not mains hum.**

Mains hum was explicitly tested and largely ruled out as the dominant term: 45–55 Hz holds
2.00 % and 55–65 Hz holds 9.17 %. Present, but not the main problem.

## The consequence that matters

Removing everything below 100 Hz drops the floor by **2.5x** (AC RMS 103,820 → 41,727).

This resolves the open question from EXP-002B. Of the two candidate explanations:

- **(a) the noise floor is inflated by low-frequency energy — CONFIRMED**
- (b) the microphone signal is genuinely weak — **rejected**

The INMP441 is fine. Our measurement band was wrong. Projecting EXP-002B's confirmed
speech peak (9.6x floor) through a 2.5x floor reduction gives roughly **24x** — comfortably
past the 10x bar that EXP-002B marginally failed. That projection must still be verified
experimentally, not assumed.

## What this proves

Sample rate correct; bit alignment correct across all 80,000 samples; no clipping; the
low-frequency contaminant identified as sub-audio drift; the microphone exonerated.

## What this does NOT prove

- The 2.5x improvement is a projection from offline filtering, not yet measured on-device.
- No WAV file exists; nothing has been listened to.
- Speech has not been re-tested with a corrected detector.

## Decision

**PROCEED to EXP-004:** add a high-pass filter and an AC-coupled level detector to the
firmware, then repeat the controlled speech test. Expect speech >= 10x floor.
