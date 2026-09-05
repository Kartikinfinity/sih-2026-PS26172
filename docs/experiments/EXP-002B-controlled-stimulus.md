# EXP-002B — Controlled-stimulus repeat of EXP-002

| Field | Value |
|---|---|
| Experiment ID | EXP-002B |
| Date | 2026-09-05 |
| Phase | 2-3 |
| Status | **PARTIAL PASS** — speech response CONFIRMED by user attribution; SNR poor |
| Artifacts | `EXP-002B-raw-capture.log` |

## Protocol

Same firmware as EXP-002 (no reflash — board already settled, avoiding the startup
transient). 50 s capture, 199 blocks, attached without reset. User protocol: 3 sync taps →
~12 s silence → ~12 s speech → ~12 s silence.

## Headline finding: raw RMS is an INVALID loudness detector on this signal

The largest raw-RMS event in the entire capture (t=32.62 s, raw RMS **1,226,722** — 4x
anything else) is **100 % DC**: its block mean is 1,225,723 and its DC-removed content is
only 49,510, i.e. ordinary background level.

That single event would have been reported as "the loudest sound of the run". It was not a
sound at all. **Raw RMS on this microphone measures the DC wander, not the audio.**

Because RMS^2 = mean^2 + variance, the DC-immune metric is available from data already
logged: `AC_RMS = sqrt(rms^2 - mean^2)` (the standard deviation).

## Re-analysis with AC (DC-removed) RMS

| Segment | n | AC mean | AC median | AC max | raw-RMS mean |
|---|---|---|---|---|---|
| 0.0-5.0 s | 19 | 40,726 | 40,436 | 58,926 | 109,900 |
| **5.0-14.5 s** | 38 | **110,185** | 75,952 | **359,274** | 136,442 |
| 14.5-32 s | 70 | 36,564 | 33,594 | 74,659 | 93,238 |
| 32-50 s | 72 | 38,395 | 35,535 | 82,292 | 143,748 |

Quiet floor (14.5-50 s): AC mean **37,492**.
Elevated window (5.0-14.5 s): AC mean 110,185 = **2.9x floor**; peak 359,274 = **9.6x floor**.

Note how the raw-RMS column is nearly flat across all four segments (93k-144k) while the AC
column separates them cleanly. The DC component was masking the real structure.

## Criteria

| # | Criterion | Result | Verdict |
|---|---|---|---|
| 1-3 | init / block accounting / liveness | unchanged from EXP-002 | PASS |
| 6 | No clipping | **0 / 199 blocks** | PASS |
| 5 | Speech >= 10x floor | 9.6x peak, 2.9x sustained (AC), speech CONFIRMED | **MARGINAL FAIL** (detected, but weak) |
| 7 | DC small vs RMS | **112/199 blocks (56 %) DC-dominated** | FAIL |
| - | Three tap spikes identifiable | not cleanly separable | FAIL |
| - | Silence-1 vs silence-3 floors within 2x | 36,564 vs 38,395 = **1.05x** | PASS |

## Interpretation

There is one unambiguous elevated acoustic window (5.0-14.5 s) roughly 9.5 s long, against
a stable, repeatable quiet floor. The floor's stability (1.05x between two widely separated
silent stretches) is strong evidence the noise floor is a genuine property of the system
and not drift.

**ATTRIBUTION CONFIRMED (user report):** the user was speaking continuously (counting)
close to the microphone during 5-14 s. The elevated window is therefore **genuine speech**,
not an ambient artifact.

Consequence: the microphone is proven to be a working acoustic sensor that responds to the
human voice. This is the central claim EXP-002 set out to establish, and it now holds.

But the *quality* of that response is poor and is now the main concern. Close-range speech
produced only **2.9x sustained / 9.6x peak** over the noise floor -- roughly 9-20 dB SNR.
A healthy INMP441 at ~10 cm should deliver far more headroom than that. Two candidate
explanations, not yet distinguished:

  (a) The noise floor is inflated by low-frequency energy. Block-mean subtraction removes
      constant DC, but a slow wave (say 5 Hz) inside a 32 ms block appears as a ramp and
      still contributes to the variance. So the "AC floor" of 37,492 may itself be mostly
      sub-audio content rather than true noise.
  (b) The microphone signal is genuinely weak (gain, distance, or a hardware issue).

These have completely different fixes, and a spectrum will separate them immediately.

Critically, the measurement instrument itself is suspect: with 56 % of blocks DC-dominated,
any threshold on raw RMS is unreliable. **Fix the instrument before re-running the
stimulus test.**

## Decision

**Do NOT repeat the stimulus test yet.** Two things must be established first, and both can
be done with a capture that requires NO user action:

1. Measure the TRUE sample rate (still never verified).
2. Dump raw PCM to the PC and inspect waveform + spectrum to identify the low-frequency
   component (mains hum at 50 Hz? sub-audio drift? artifact?).

That becomes **EXP-003**. The stimulus test returns as EXP-004 once the detector is
AC-coupled and the DC source is understood.
