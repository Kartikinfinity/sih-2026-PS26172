# EXP-010 — On-device inference: runs correctly, 123x too slow

| Field | Value |
|---|---|
| Experiment ID | EXP-010 |
| Date | 2026-09-06 |
| Phase | 14-15 (TFLite Micro deployment / on-device inference) |
| Status | **PARTIAL — functional PASS, performance FAIL** |
| Artifacts | `code/exp010_ondevice_kws.cpp`, `model/kws_model.cc` |

## What worked

TensorFlow Lite Micro **compiles and runs on the existing Arduino / ESP-IDF 4.4.7
toolchain**. The Phase 0 deferred question — whether deployment forces a migration to
ESP-IDF 5.x — is answered for compilation: it does not.

| Measurement | Value |
|---|---|
| Model in flash | 47,416 bytes |
| **Arena used** | **132,004 bytes** (internal SRAM) |
| Free heap after allocation | 139,416 bytes |
| Firmware RAM | 25.4 % of 327,680 |
| Firmware flash | 6.0 % of 6,553,600 |
| **Feature extraction** | **1.11 ms/frame** |

Feature cost of 1.11 ms/frame matches EXP-006's 1.107 ms independently — the streaming
implementation costs the same per frame as the batch one, as designed.

The full chain executes: I2S -> sliding frame -> log-Mel -> normalise -> int8 quantise ->
interpreter -> softmax -> threshold. No allocation failures, no invoke errors.

## What failed: inference latency

```
infer 12280.19 ms   (measured repeatedly, steady state)
```

**12.28 seconds per inference against a 100 ms budget — roughly 123x over.**

This is not first-call initialisation; five consecutive inferences measured
12280.19-12280.56 ms, a spread of 0.4 ms.

### Why this is anomalous, not merely "unoptimised"

Approximate multiply-accumulate count for the model:

| Layer | MACs |
|---|---|
| Conv 10x4x1x64 over 49x20 | ~2.5 M |
| 4 x [DW 3x3x64 + PW 1x1x64x64] over 49x20 | ~18.3 M |
| **Total** | **~21 M** |

At 240 MHz, even a naive **1 MAC per cycle** would finish in ~87 ms. The measured
12,280 ms implies roughly **0.007 MACs per cycle** — about 140x slower than a scalar
lower bound. That is not the signature of "missing SIMD"; it points to a pathologically
slow kernel path in this TFLM port.

## The detections shown are NOT evidence that detection works

The log shows keyword probability rising 0.890 -> 0.997 with `*** SENTINEL ***` firing.
**This must not be read as working keyword spotting.**

The loop is single-threaded. During a 12.28 s `Invoke()` no features are computed, the
I2S DMA queue overflows, and the next 98 frames are built from whatever backlog remains.
The temporal relationship between spoken audio and classified window is therefore broken.
The outputs are plausible-looking and untrustworthy.

**On-device detection accuracy is unmeasured.** What is demonstrated is that the pipeline
executes end to end and produces well-formed probabilities — nothing about whether it
detects the right things at the right times.

## Now-justified next steps, in order of expected value

1. **Per-operator profiling.** 140x below a scalar bound suggests one specific kernel is
   responsible. Measure before optimising — `ESP32TFLMWrapper` exposes per-op timing.
2. **ESP-NN via esp-tflite-micro (ESP-IDF 5.x).** This is now backed by measurement
   rather than assumption, which is exactly the evidence the Phase 0 note said would be
   required. Expected gain is roughly 3-10x on ESP32-S3 vector instructions — helpful but
   **not sufficient alone** for 123x.
3. **Shrink the model.** The input is 98x40, striding to 49x20 with 64 channels
   throughout. ARM's reference DS-CNN-S uses ~49x10 (10 MFCC, not 40 Mel), so this model
   carries roughly 4x the spatial work of a standard KWS network. More aggressive
   striding, fewer channels, or fewer Mel bins are all available and none require new
   data.

Realistically **both 2 and 3** will be needed.

## What this does NOT prove

- On-device detection accuracy, false-fire rate, or latency-to-detection.
- That ESP-NN alone closes the gap.
- Anything about power or long-run stability.

## Decision

**Do not proceed to Phase 16 (detection logic).** Building temporal smoothing on top of an
inference that takes 12 s would be building on a layer that is not working. Profile first,
then reduce the model and adopt ESP-NN, then re-measure.
