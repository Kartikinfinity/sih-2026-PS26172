# EXP-009 — Full-integer int8 quantisation

| Field | Value |
|---|---|
| Experiment ID | EXP-009 |
| Date | 2026-09-06 |
| Phase | 12-13 (quantisation / offline validation) |
| Status | **PASS** |
| Artifacts | `model/kws_int8.tflite`, `model/kws_model.cc`, `model/model_meta.json`, `tools/quantize_kws.py` |

## Objective

Convert the trained float32 DS-CNN to full-integer int8 -- weights, activations, input and
output tensors -- and **measure** what quantisation costs on the same held-out test set.
Full-integer is required for ESP-NN's optimised kernels; a float or hybrid model would run
but forfeit them.

## Result: quantisation cost nothing in aggregate

| | float32 | int8 |
|---|---|---|
| Accuracy | 0.9505 | **0.9505** |
| Accuracy excluding silence | 0.9196 | **0.9196** |
| **Delta** | | **+0.0000** |
| Prediction agreement | | **98.90 %** |

| | Value |
|---|---|
| **Deployable size** | **47,416 bytes (46.3 KB)** |
| Input quantisation | scale 0.02723478, zero-point −15 |
| Output quantisation | scale 0.07412431, zero-point −14 |

46.3 KB sits inside the plan's 40-150 KB target band. Note it is roughly double the
23 KB naive estimate from parameter count alone: a `.tflite` also carries the graph
structure, per-tensor quantisation parameters and operator metadata.

## The aggregate number hides an operating-point shift

Identical accuracy does not mean identical behaviour. The confusion matrices differ:

| | float32 | int8 |
|---|---|---|
| keyword correct | 137 / 140 | **139 / 140** |
| unknown correct | 69 / 84 | **67 / 84** |

| Threshold | float miss | float false-fire | int8 miss | int8 false-fire |
|---|---|---|---|---|
| 0.50 | 0.0214 | 0.0625 | **0.0071** | **0.0759** |
| 0.90 | 0.1286 | 0.0179 | 0.1143 | 0.0223 |

**The int8 model is slightly more trigger-happy**: it misses fewer keywords and fires
falsely more often. Accuracy is unchanged because the two shifts cancel. Reporting only
"no accuracy loss" would have concealed a real change in the trade-off that matters for a
wake word. 98.90 % agreement means roughly 1 in 90 test examples is decided differently.

## Firmware constants emitted

`model/model_meta.json` carries the normalisation mean/std from training and the input
scale and zero-point. The firmware must apply **both**, in that order, to its log-Mel
features. Getting either wrong degrades accuracy silently while everything still runs --
the same failure class EXP-006's parity test exists to prevent, one layer further on.

`model/kws_model.cc` contains the model as a 16-byte-aligned C array ready to compile in.

## What this does NOT prove

- Nothing has run on the ESP32. This is a host-side TFLite interpreter result.
- On-device inference latency and CPU cost are unmeasured.
- Arena size for TFLite Micro is unknown.
- The false-fire figures remain per-clip on curated hard negatives, **not** activations
  per hour of continuous audio (the plan's section-7 metric).

## Decision

**PROCEED to Phase 14** (on-device deployment). The Phase 0 open question -- ESP-IDF 4.4
with the legacy I2S driver versus IDF 5.x with `esp-tflite-micro` -- must now be resolved,
since this is the phase where it actually bites.
