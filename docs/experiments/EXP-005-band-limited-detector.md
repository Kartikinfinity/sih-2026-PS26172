# EXP-005 — Band-limited (300–3400 Hz) energy detector

| Field | Value |
|---|---|
| Experiment ID | EXP-005 |
| Date | 2026-09-05 |
| Phase | 6 (preprocessing) |
| Status | **PASS — prediction met**, with an attribution caveat |
| Artifacts | `EXP-005-capture-truelevel.wav`, `EXP-005-capture-normalized.wav`, `EXP-005-band-detector.png`, `code/exp005_band_detector.cpp` |

## Pre-declared prediction

> Band-limited speech-to-floor **>= 10x**. Expected value ~13x from EXP-004's band table.

## RESULT: 51.2x — PASS

Two independent filter chains ran per sample on identical audio, so the comparison
between metrics is controlled rather than cross-run:

| Metric | floor | speech peak | **peak/floor** | p90/floor | mean/floor |
|---|---|---|---|---|---|
| wideband, 100 Hz HP | 50.0 | 1848.2 | **37.0x** | 16.5x | 6.7x |
| **band 300–3400 Hz** | 21.5 | 1099.0 | **51.2x** | 16.9x | 6.8x |

Transfer was 640,000 / 640,000 bytes (**100 %**) — the EXP-004 truncation bug is fixed.
0 clipped samples, peak 4,895 of 32,767.

## Every word cleared the bar, not just the peak

Seven discrete bursts, 0.20–0.45 s each, spaced ~1 s apart — counting aloud. Burst
duration rules out taps, which would be under 100 ms.

| Burst | Peak / floor |
|---|---|
| 1.65–2.10 s | 51.2x |
| 2.35–2.70 s | 19.3x |
| 3.25–3.60 s | 20.0x |
| 4.25–4.60 s | 31.6x |
| 5.30–5.60 s | 19.7x |
| 6.40–6.60 s | 16.1x |
| 7.45–7.85 s | 18.4x |

**The weakest word still reached 16.1x.** All seven exceed the 10x bar, which is a
stronger statement than a single peak clearing it.

Floor stability: 16.8 before speech, 21.7 after = 1.29x.

## CAVEAT — do not attribute 9.6x -> 51.2x to band-limiting

EXP-004 measured 9.6x on the wideband chain. EXP-005 measured **37.0x on that same
wideband chain**. The chain did not change; the speech did — peak 1848 here versus 616
in EXP-004, against a similar floor. **The user simply spoke louder or closer this time.**

Comparing 9.6x (EXP-004) with 51.2x (EXP-005) would therefore be dishonest: most of that
gap is speech level, not signal processing.

**The honest measure of what band-limiting bought is the within-run comparison:
37.0x -> 51.2x, a 1.39x improvement.** Real, but modest — and far short of what the
headline number suggests.

## The more important finding: the benefit is peak-only

| Statistic | wideband | band | gain |
|---|---|---|---|
| peak / floor | 37.0x | 51.2x | **1.39x** |
| p90 / floor | 16.5x | 16.9x | 1.02x |
| mean / floor | 6.7x | 6.8x | 1.01x |

Band-limiting improves the **peak** by 1.39x and leaves p90 and mean essentially
untouched. The bandpass attenuates signal and floor in nearly equal proportion except at
transient peaks.

**Implication:** a scalar energy detector — even a well-chosen band-limited one — has
limited discriminative power. It answers "is something loud happening in the speech band"
but cannot distinguish a spoken keyword from a door slam, a cough, or any other
band-occupying sound. Pushing this metric further has low ceiling.

This is the experimental justification for moving to real features. A Mel/MFCC front end
does not produce one number per frame; it produces a *vector* describing the spectral
shape, which is what makes one word distinguishable from another. That is Phase 7 of the
roadmap, and EXP-005 has now earned the transition rather than assuming it.

## What this proves

Band-limited energy detection works and clears its bar with margin on every word.
Transfer integrity restored. Filter chains run in real time on-device alongside capture
with no dropped frames.

## What this does NOT prove

- Nothing distinguishes *which* word was spoken. There is no keyword spotting.
- No feature vectors exist. No model exists.
- Not tested against non-speech sounds (claps, door slams, music) — false-positive
  behaviour is entirely uncharacterised.
- Detection thresholds were derived post-hoc from this recording, not validated on
  held-out audio.

## Decision

**PROCEED to Phase 7: framing + windowing + FFT + Mel filterbank**, producing feature
vectors instead of scalar energy.
