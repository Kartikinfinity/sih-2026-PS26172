# EXP-004 — On-device high-pass + AC detector + first WAV

| Field | Value |
|---|---|
| Experiment ID | EXP-004 |
| Date | 2026-09-05 |
| Phase | 5-6 (signal analysis / preprocessing) |
| Status | **PARTIAL PASS — stated prediction FAILED** |
| Artifacts | `EXP-004-capture-truelevel.wav`, `EXP-004-capture-normalized.wav`, `EXP-004-speech-detection.png`, `code/exp004_highpass_detector.cpp` |

## Pre-declared prediction

> Speech will exceed **10x** the filtered noise floor.

## RESULT: 9.6x — PREDICTION FAILED

Measured with the floor defined as the median 50 ms envelope across BOTH quiet stretches
(0.5-5.4 s and 10.5-23.9 s), floor = 64.1:

| Metric | Value |
|---|---|
| Speech peak / floor | **9.6x** |
| Speech p90 / floor | 6.8x |
| Speech mean / floor | 3.8x |
| Speech median / floor | 3.1x |
| Pre- vs post-speech floor stability | 1.22x |

The prediction is not met. Note that an alternative floor definition (20th percentile of
the whole record, 54) would have yielded 11.4x and "passed" — **that definition was not
adopted, because choosing the floor estimator after seeing the data is exactly how one
fakes a result.** The median-of-quiet-stretches definition is the defensible one and it
gives 9.6x.

Coincidentally EXP-002B also measured 9.6x peak. The high-pass therefore did NOT improve
the broadband peak-to-floor ratio, even though it demonstrably removed drift.

## Why the broadband ratio did not improve

Band energy, measured on the filtered signal:

| Band | quiet (12-20 s) | speech (5.5-10.3 s) |
|---|---|---|
| 0-100 Hz | **41.42 %** | 2.28 % |
| 100-300 Hz | 29.80 % | 52.87 % |
| **300-3400 Hz (speech)** | 22.26 % | **43.76 %** |
| 3400-8000 Hz | 6.52 % | 1.09 % |

The filter cut sub-100 Hz content in the quiet stretches from EXP-003's 64.6 % to 41.4 %,
but did not eliminate it. A 12 dB/octave corner at 100 Hz still passes much of the
50-100 Hz band, which EXP-003 measured at 30.5 % of energy. That residual keeps the
broadband floor high, so a broadband RMS ratio stays stuck near 9-10x no matter how clear
the speech is.

**Conclusion: broadband RMS is the wrong detector — not because the filter failed, but
because the metric integrates across bands where the signal is not.**

## The decisive evidence (stronger than the failed criterion)

During speech the **300-3400 Hz band holds 43.76 %** of energy. In EXP-003's ambient
capture that band held **1.43 %**. That is roughly a **30x** increase in speech-band
occupancy, and sub-100 Hz collapses from 41 % to 2 % during speech.

The envelope also shows ~8 discrete bursts between 5.6 s and 10.2 s separated by short
gaps — the signature of counting aloud ("one, two, three..."), matching the user's
reported action. Floors before and after agree to within 1.22x, so the rise is caused by
the speech and reverses when it stops.

**The microphone unambiguously detects speech.** The failed criterion measured the wrong
quantity; it does not indicate a hardware problem.

## Data integrity note

The serial transfer delivered 770,790 of 800,000 bytes (96.3 %); the capture was truncated
at ~24.1 s of the intended 25 s. The apparent "peak sample 30068" was located at
t = 24.075 s — inside the final 13 ms, at the truncation boundary — and is a transfer
artifact, not audio. After trimming to 24.0 s the true peak is **1452**, consistent with
the 616 envelope maximum. All analysis above uses the trimmed signal. **0 clipped samples.**

The receive-side read deadline should be lengthened before the next dump.

## First listenable audio

`EXP-004-capture-truelevel.wav` — honest levels, no gain (quiet: peak 1452 of 32767).
`EXP-004-capture-normalized.wav` — same audio at 19.3x gain for listening.

## Decision

**PROCEED to band-limited detection.** Replace broadband RMS with energy measured inside
300-3400 Hz. This is not a workaround: it is the first step of the Mel/MFCC feature
pipeline the roadmap already specifies for Phase 7, and EXP-004 has now produced the
experimental justification for it rather than adopting it on authority.

Open: the residual 41 % sub-100 Hz in quiet suggests a steeper filter or higher corner
should also be evaluated.
