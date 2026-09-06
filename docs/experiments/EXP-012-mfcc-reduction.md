# EXP-012 — MFCC front end + reduced model: 26.6x faster, under budget

| Field | Value |
|---|---|
| Experiment ID | EXP-012 |
| Date | 2026-09-06 |
| Phase | 7 rebuilt / 10-15 re-run |
| Status | **PASS on latency**, accuracy regression recorded |

## Changes

| | before (EXP-008/011) | after |
|---|---|---|
| Features | 98 x 40 log-Mel, 10 ms hop | **49 x 13 MFCC, 20 ms hop** |
| Input elements | 3,920 | **637** (6.2x fewer) |
| Channels | 64 | 32 |
| Blocks | 4, no internal stride | 4, stride-2 mid-stack |
| Parameters | 23,747 | **7,779** |

## Latency and memory: the target is met

| Measurement | EXP-010 (ref kernels) | EXP-011 (ESP-NN) | **EXP-012** |
|---|---|---|---|
| Inference | 12,280 ms | 2,240 ms | **84.17 ms** |
| Speedup vs previous | — | 5.48x | **26.6x** |
| **Total vs start** | — | — | **146x** |
| Arena | 132,004 B | 132,004 B | **15,460 B** |
| Model in flash | 47,416 B | 47,416 B | **24,536 B** |
| Free heap | 58,312 B | 58,312 B | **168,056 B** |

**84.17 ms is inside the 100 ms budget.**

CPU, computed against the real 20 ms hop:

| Component | Cost | Duty | Share |
|---|---|---|---|
| Feature extraction | 1.17 ms | every 20 ms | 5.9 % |
| Inference | 84.17 ms | every 200 ms | 42.1 % |
| **Total** | | | **48.0 %** of one core |

48 % misses the plan's "well under 10 %" target, but the ESP32-S3 has two cores and
inference has not yet been moved off the audio core, nor duty-cycled behind a cheap
energy gate. Both are available and unexplored.

## The pipeline is no longer stalling itself

This is the qualitative change. At 2,240 ms per inference the audio pipeline stalled, the
DMA queue overflowed, and every result in EXP-010 and EXP-011 was temporally meaningless.
At 84 ms against a 200 ms cadence, feature extraction keeps up and the DMA depth
(16 x 256 = 256 ms) absorbs the pause. **On-device results are now temporally valid for
the first time** and can legitimately be tested.

Observed behaviour in a quiet room: `kw 0.000 unk 0.000 sil 1.000` — correctly and
confidently silence, which the stalled builds could never produce reliably.

## A training failure worth recording

The first MFCC training run collapsed to **test accuracy 0.3846 — exactly the silence
fraction** — predicting one class for everything.

Cause: normalisation. A single global mean/std was inherited from the log-Mel model, where
it was fine because all 40 bands shared a scale. MFCC coefficients do not:

| | mean | std |
|---|---|---|
| c0 (log energy) | 100.02 | 8.85 |
| c1..c12 | -0.71 .. 11.02 | 1.05 .. 3.55 |

Under a global std of 26.6, coefficients c2-c12 collapsed to std **0.04-0.08** — eleven
near-constant channels. The network kept the only usable signal, overall energy, and
degenerated to the majority class.

Fix: per-coefficient (cepstral mean-variance) normalisation. Firmware constants are now
**generated** into `src/kws_params.h` from `model_meta.json` rather than transcribed by
hand, because 26 floats copied manually is precisely how a silent train/inference mismatch
enters.

## Accuracy cost: real, and not hidden

| | log-Mel 98x40 (EXP-008) | MFCC 49x13 |
|---|---|---|
| Test accuracy | 0.9505 | 0.8956 |
| **Excluding silence** | **0.9196** | **0.8304** |
| miss @ 0.5 | 0.0214 | 0.0357 |
| **false-fire @ 0.5** | **0.0625** | **0.1473** |

**False-fire more than doubled.** Latency was bought with accuracy. The 6.2x input
reduction discards information, and 68 source negative clips were already the weakest part
of the dataset (EXP-008).

int8 quantisation of the smaller model cost nothing: 0.8956 float -> 0.8984 int8
(+0.0027), 99.73 % prediction agreement.

## What remains unproven

- On-device detection accuracy against real speech. Now *testable*, but not yet tested.
- False activations per hour of continuous audio (the plan's section-7 metric).
- Any speaker other than the one who recorded the data.

## Known cosmetic defect

The startup banner still prints "49 frames x 40 mel | inference every 100 ms". The values
are 13 MFCC and 200 ms; a string replacement did not match. Display only — the computed
values above are correct.

## Decision

**Run a live on-device detection test**, the first one whose result can be trusted. Then
address the false-fire regression, for which importing Google Speech Commands to broaden
the unknown class is the measured next step (justified since EXP-008).
