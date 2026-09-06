# EXP-011 — ESP-NN enabled: 5.48x faster, still 22x over budget

| Field | Value |
|---|---|
| Experiment ID | EXP-011 |
| Date | 2026-09-06 |
| Phase | 19 (latency optimisation), pulled forward because Phase 16 is blocked |
| Status | **PARTIAL** — large measured win, target still missed |

## Finding: ESP-NN was present but disabled

The `ESP_TF` library **bundles Espressif's ESP-NN**, including ESP32-S3 assembly
(`esp-nn/esp_nn_conv_s8_mult8_1x1_esp32s3.S` and similar) and ESP-NN kernel variants at
`tensorflow/lite/micro/kernels/esp_nn/`. But those kernels sit behind `#if ESP_NN`, and
the library's `library.json` declares no build flags — so the **reference** kernels
compiled. That is what EXP-010 measured.

Two build flags enable them:

```ini
-DESP_NN=1
-DCONFIG_IDF_TARGET_ESP32S3=1   ; gates ARCH_ESP32_S3 in esp-nn/esp_nn.h
```

## Measured result

| Build | Inference | vs budget |
|---|---|---|
| Reference kernels (EXP-010) | 12,280 ms | 123x over |
| **ESP-NN kernels** | **2,240 ms** | **22.4x over** |
| **Speedup** | **5.48x** | |

5.48x falls inside the 3-10x band predicted in EXP-010, so ESP-NN behaved as expected.
It was **not sufficient alone**, which was also predicted. Flash grew 393,673 -> 397,001 B.

**This is the cheapest 5.48x available: two build flags, no toolchain migration.** The
Phase 0 deferral of the ESP-IDF 5.x question remains correct — ESP-NN was reachable from
the Arduino framework all along.

## The built-in profiler does not work in this port

`tflite::MicroProfiler` was wired into the interpreter. Every operator reported
**`took 0 ticks (-1 ms)`** and `GetTotalTicks()` returned 0, against a wall-clock 2,243 ms.
The profiler's tick source is not implemented in this library. Per-operator attribution is
therefore **unavailable by this route**; the conclusion below is derived arithmetically
instead, and is labelled as such.

## Where the time goes (derived, not directly measured)

| Quantity | Value |
|---|---|
| Model MACs | ~21 M |
| Output elements across all layers | ~564,000 |
| Cycles at 240 MHz for 2,240 ms | ~538 M |
| **Cycles per MAC** | **~25** |
| **Cycles per output element** | **~950** |

~25 cycles per MAC is scalar-with-overhead territory, not SIMD. The likely cause is that
the layer shapes do not match ESP-NN's fast assembly paths, which are specialised for
particular channel multiples and kernel sizes.

## The real problem is model shape, not just kernels

| | this model | ARM DS-CNN-S reference |
|---|---|---|
| Input | 98 x 40 = 3,920 | 49 x 10 = 490 |
| After first stride | 49 x 20 x 64 | ~25 x 5 x 64 |

**The input carries 8x the data of a standard KWS network.** 40 Mel bins at a 10 ms hop
was chosen in EXP-006 to follow micro_speech, but micro_speech's model consumes far less
per inference. No amount of kernel optimisation fixes an input that is 8x too large.

## Proposed fix (needs no new recordings)

1. **MFCC instead of raw log-Mel**: apply a DCT and keep ~13 coefficients. 40 -> 13 is a
   3x reduction and is what the plan itself suggests ("13-40 MFCC coefficients").
2. **20 ms hop instead of 10 ms**: 98 -> 49 frames, a further 2x.
3. Combined input 49 x 13 = 637, a **6.2x** reduction, matching the reference architecture.
4. Optionally fewer channels (64 -> 32) for roughly another 2x.

Every raw session WAV is saved, so the dataset can be rebuilt and the model retrained
without asking the user to record anything again. The host/device parity test from EXP-006
must be re-run afterwards, since the feature definition changes on both sides.

Expected combined effect is roughly 12-25x on top of the 5.48x already banked. That would
put inference near or under the 100 ms budget, but it is a projection and must be measured.

## What remains unproven

- On-device detection accuracy: still broken by the 2.24 s inference stalling the audio
  pipeline, exactly as recorded in EXP-010. The `*** SENTINEL ***` lines remain
  untrustworthy.
- That the proposed reduction reaches budget.
- Accuracy cost of moving from 40 log-Mel to 13 MFCC (must be re-measured offline first,
  where it is cheap).

## Decision

**Rebuild the feature pipeline to 49 x 13 MFCC and retrain**, validating the accuracy cost
offline before touching firmware again.
