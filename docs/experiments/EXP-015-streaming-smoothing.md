# EXP-015 — Temporal smoothing, and the first streaming metric

| Field | Value |
|---|---|
| Experiment ID | EXP-015 |
| Date | 2026-09-06 |
| Phase | 16 (detection logic) / 17 (false-positive characterisation) |
| Status | **Smoothing works as predicted. Detection rate is now the binding problem.** |

## The first time-ordered evaluation

Every figure before this was per-window or per-clip. `tools/eval_streaming.py` runs the
model over held-out audio **in time order** at the detector's real 200 ms cadence, applies
threshold plus N-consecutive smoothing exactly as firmware would, and matches firings to
known utterances.

Held-out audio: **120.0 s across 7 sessions, containing 38 true keyword utterances.**

| threshold | N | detected | missed | false activations/min |
|---|---|---|---|---|
| 0.50 | 1 | 30/38 (78.9 %) | 8 | **4.00** |
| **0.50** | **2** | **23/38 (60.5 %)** | 15 | **0.00** |
| 0.50 | 3 | 19/38 (50.0 %) | 19 | 0.00 |
| 0.70 | 1 | 24/38 (63.2 %) | 14 | 4.50 |
| 0.70 | 2 | 15/38 (39.5 %) | 23 | 0.00 |
| 0.90 | 1 | 19/38 (50.0 %) | 19 | 0.00 |

## Smoothing behaves exactly as the asymmetry predicts

N=1 to N=2 at threshold 0.50 removed **every** false activation (4.00/min to 0.00) while
costing 18.4 points of detection. False windows are largely isolated; a real utterance
spans several consecutive windows. Requiring two in a row suppresses the former far faster
than the latter.

This is why the same technique was **refused** in EXP-013. Against the deployed model's
~50 % per-window false rate, consecutive false windows would have been common, so smoothing
would have improved the reported number without the model having learned anything.

## Honest reading of "0.00 false activations per minute"

**2 minutes is a small sample.** Observing zero events in 120 s does not establish a low
rate; it bounds it loosely (roughly under 1.5/min at 95 % confidence). It is **not** the
plan's section-7 metric, which asks for false activations per **hour** of continuous
negative audio. That measurement still has not been made and would need hours of recording.

## The binding constraint is now detection, not false fires

**60.5 % detection is not a usable wake word.** Missing two utterances in five would make
the device feel broken. Every remaining lever points at the same root cause: the model has
1,576 keyword training windows derived from roughly 180 utterances by a single speaker in
one room on one day.

This is not something threshold tuning, smoothing or architecture changes can fix. It is a
data problem, and it has been visible since EXP-008 flagged that 100 % of the meaningful
errors involved the keyword/unknown boundary.

## Fixes in order of expected value

1. **More keyword recordings**, and from more speakers (DECISION-02 deferred this).
2. **Google Speech Commands** for the unknown class -- justified since EXP-008 and
   reinforced three times since.
3. Only then revisit architecture or thresholds.

## Also fixed here

`tools/record_dataset.py` imported pyserial at module scope, which broke
`build_continuous.py` and `eval_streaming.py` in the TensorFlow environment on D:. The
import is now lazy: slicing a WAV file has no business requiring a serial library.

## Decision

**Stop tuning and collect more data.** The end-to-end system is complete and measured; its
accuracy is limited by dataset size, and further tuning would only move along a curve
already characterised.
