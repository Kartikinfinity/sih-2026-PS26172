# EXP-008 — Baseline DS-CNN keyword spotter

| Field | Value |
|---|---|
| Experiment ID | EXP-008 |
| Date | 2026-09-06 |
| Phase | 10-11 (baseline model / training) |
| Status | **PASS as a baseline**, with two honest caveats |
| Artifacts | `model/kws_float.keras`, `model/norm.json`, `tools/train_kws.py`, `tools/build_dataset.py`, `tools/features.py` |

## Model

Depthwise-separable CNN (ARM ML-KWS / MobileNet family), as the plan specifies.

| Property | Value |
|---|---|
| Input | 98 frames x 40 Mel, 1 channel |
| Architecture | Conv(64, 10x4, stride 2) -> 4 x [DWConv 3x3 + PWConv 1x1] -> GAP -> Dense(3) |
| **Parameters** | **23,747** |
| float32 weights | 92.8 KB |
| int8 estimate | **~23 KB** |
| Classes | keyword / unknown / silence |

23 KB int8 sits below the plan's 40-150 KB target band, leaving room to grow the model
if accuracy demands it.

## Headline result

| Split | Accuracy |
|---|---|
| Validation | 0.9675 |
| **Test (held out)** | **0.9505** |

## Caveat 1 — that headline is inflated

Silence is **38 % of the test set** and is classified **140/140 perfectly**. It is a
trivial class: background noise looks nothing like speech in the feature domain.

Excluding silence, accuracy on the classes that are actually hard is
**206/224 = 0.9196**. That is the number to quote, not 0.9505.

## Caveat 2 — the metric that matters is not accuracy

For a wake word, what counts is the trade between missing the keyword and firing when it
was not said. Measured on the held-out test set:

| Threshold | Miss rate | False-fire rate |
|---|---|---|
| 0.50 | 0.0214 | **0.0625** |
| 0.70 | 0.0357 | 0.0536 |
| 0.90 | 0.1286 | 0.0179 |
| 0.95 | 0.1357 | 0.0045 |
| 0.99 | 0.2500 | 0.0000 |

**Every error is the same error.** The confusion matrix shows silence is never confused
with anything, keyword recall is 0.9786, and all 18 test mistakes are
**unknown -> keyword** (15) or keyword -> unknown (3). The model's weakness is
distinguishing the keyword from phonetically similar words -- exactly the axis the hard
negatives were recorded to probe.

Note that this false-fire rate is measured against **deliberately confusable words**, a
worst case. It is not the rate against arbitrary audio, which will be lower.

## The model overfits

Training accuracy reached **1.0000** with loss ~0.004 while validation loss sat at
0.07-0.13. With 115 source positives and 68 source negatives, the network memorises the
training clips. Augmentation (random crop, gain jitter, noise mixing at five SNRs)
multiplies examples but not underlying variety.

## What has NOT been measured

- **False activations per hour of continuous audio** -- the metric the plan actually
  specifies (section 7). What is reported above is a per-clip rate on a curated set. These
  are different quantities and must not be conflated.
- Any performance for a speaker other than the one who recorded the data (DECISION-02).
- On-device accuracy. This is a float32 host model; nothing has been quantised or run on
  the ESP32.
- Inference latency and CPU cost on hardware.

## Evidence-based next step

The "unknown" class has only **68 source clips** and is where **100 % of the meaningful
errors** occur. This is now a measured justification, not a guess, for importing a large
public speech corpus (Google Speech Commands, ~2.3 GB, tens of thousands of utterances)
to broaden that class before further tuning.

Deliberately deferred until the measurement existed. Downloading 2.3 GB on the assumption
that more data would help would have been unfalsifiable; the confusion matrix now says
precisely which class needs it and why.

## Decision

**PROCEED to Phase 12** (int8 quantisation) to establish the deployable size and measure
accuracy retention, then decide between importing Speech Commands and moving to
on-device deployment.
