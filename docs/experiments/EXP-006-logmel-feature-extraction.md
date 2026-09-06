# EXP-006 — Log-Mel feature extraction + host/device parity

| Field | Value |
|---|---|
| Experiment ID | EXP-006 |
| Date | 2026-09-06 |
| Phase | 7 (feature extraction) |
| Status | **PASS** (parity), with one target missed (CPU) |
| Artifacts | `EXP-006-logmel-parity.png`, `EXP-006-capture.wav`, `code/exp006_logmel.cpp` |

## Objective

Build the real KWS front end — framing, windowing, FFT, Mel filterbank, log — and verify
that features computed on the ESP32 are numerically identical to a host reference on the
same samples. The plan (section 4.2) requires the feature pipeline be identical on host and
device; a silent mismatch degrades a trained model without any visible failure.

## Parameters

Following the TFLite Micro `micro_speech` convention the plan cites:

| Parameter | Value |
|---|---|
| Sample rate | 16 kHz |
| Frame length | 400 samples (25 ms) |
| Frame hop | 160 samples (10 ms) |
| FFT size | 512 (zero-padded), 257 bins |
| Window | periodic Hann |
| Mel filters | 40 triangular, evenly spaced on the Mel scale |
| Mel range | 125–7500 Hz |
| Output | `log(mel_energy + 1e-6)` |
| Frames per 3 s capture | 298 |

The 125 Hz lower band edge also excludes the sub-audio drift measured in EXP-003, so no
separate high-pass stage is required in this pipeline.

## Objective 1 — host/device parity: PASS

Pre-declared prediction: **max absolute difference < 0.01** log-Mel units.

| Measurement | Value |
|---|---|
| **max abs diff** | **0.000427** |
| mean abs diff | 0.00000766 |
| 99th percentile abs diff | 0.0000730 |
| correlation | **1.0000000000** |
| device value range | [8.335, 20.408] |
| host value range | [8.335, 20.408] |

Worst single cell (frame 14, mel 20): host 11.688442 vs device 11.688869.

The residual is consistent with float32 on-device versus float64 on host, which is exactly
what should remain when the algorithms match. It is **23x below** the declared tolerance.

Transfers were 96,000/96,000 bytes of PCM and 47,680/47,680 bytes of features — both 100 %.

## Objective 2 — the features are meaningful

The log-Mel spectrogram shows horizontal banded structure at Mel bins 5–15 during roughly
1.2–1.9 s and 2.3–2.8 s. That banding is formant/harmonic structure, the signature of
voiced speech, and it is exactly the information a scalar energy detector discarded.

Compare with EXP-005: there, one number per frame could report only "something loud is
happening". Here each frame carries a 40-element vector describing spectral *shape*, which
is what allows one word to be distinguished from another.

Capture peak 3,055 of 32,767; **0 clipped samples**.

## Objective 3 — CPU cost: TARGET MISSED

Measured on-device with `esp_timer`:

```
298 frames in 329.89 ms  =>  1.107 ms/frame
real-time budget 10.000 ms/frame  =>  CPU load 11.07 %
```

The plan targets **idle CPU well under 10 %** during continuous listening. Feature
extraction alone consumes **11.07 %**, before any inference is added. **The target is
currently missed and is recorded as missed.**

This is a baseline, not a final number. The FFT here is a plain radix-2 implementation
written for clarity and correctness. **ESP-DSP is installed and available**
(`dsps_fft2r.h`, `libespressif__esp-dsp.a` linked in the Arduino core) and is the Phase 19
optimisation target. It was deliberately not used yet: without a measured baseline, any
later speedup claim would be unfalsifiable.

Other levers not yet applied: computing features only on a duty cycle rather than every
frame, and moving extraction to the second core.

## What this proves

The feature pipeline is correct, numerically identical across host and device to within
float precision, produces recognisably speech-shaped output, and runs in real time on one
core with margin (1.107 ms against a 10 ms budget).

## What this does NOT prove

- No keyword spotting exists. Nothing identifies *which* word was spoken.
- No model, no training data, no dataset.
- Parity was verified on a single 3-second capture, not across varied inputs.
- The CPU figure covers feature extraction only; inference cost is unmeasured.
- Fixed-point/int8 quantisation of the feature path has not been attempted; the current
  pipeline is float32 throughout.

## Decision

**PROCEED to Phase 8–9**: KWS fundamentals and dataset creation. The front end that turns
sound into model input is now built and verified, so the next question is what the model
consumes and how training data gets collected.

Carry forward as an open item: reduce feature-extraction CPU below the 10 % budget using
ESP-DSP, and measure the improvement against the 1.107 ms/frame baseline recorded here.
