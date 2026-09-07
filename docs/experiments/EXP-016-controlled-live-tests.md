# EXP-016 — Controlled live tests: the system is not usable, and we now know the number

| Field | Value |
|---|---|
| Experiment ID | EXP-016 |
| Date | 2026-09-07 |
| Phase | 15-17 (on-device inference / false-positive characterisation) |
| Status | **FAIL — measured honestly** |
| Artifacts | `EXP-016-live-capture.log`, `code/exp016_deployed_detector.cpp` |

## What was deployed first

Three gaps between measurement and hardware, found by the Understanding Report, were closed:

1. `model_cont` had never been quantised or flashed. The board was running the
   centred-clip model EXP-014 measured firing on 49.7 % of non-keyword windows.
   Quantised to 24,536 bytes: int8 accuracy 0.6654 vs float 0.6616, 97.59 % agreement.
2. Temporal smoothing existed only in Python. Now in firmware.
3. `tools/emit_params.py` was itself broken by the backslash-mangling bug, so
   `src/kws_params.h` still held the PREVIOUS model's quantisation constants (input
   zero-point 9 instead of 3, normalisation mean 100.02 instead of 93.78). Deploying
   without regenerating would have been the exact silent mismatch that tool exists to
   prevent.

Removing the dead MicroProfiler freed **82 KB of RAM** (47.3 % -> 22.3 %) and raised free
heap from 168,048 to 249,152 bytes.

## Two controlled tests, no attribution ambiguity

Earlier live tests mixed keywords and other speech, so firings could not be attributed.
These two are unambiguous by construction: in Test 1 every event is a keyword, in Test 2
no event is.

### Test 1 — "Sentinel" x 10, nothing else

| Smoothing rule | Firings | Detection rate |
|---|---|---|
| strict 2-consecutive | 5 | **50 %** |
| **2-of-3 vote + 1.2 s refractory** | 5 | **50 %** |

Changing the rule did not change the result. Offline predicted 60.5 % and 68.4 %
respectively; live delivered 50 % under both.

A probable double-fire survived: firings 1.0 s apart at 11.1 s and 12.1 s with both
surrounding buckets elevated. Host timestamps arrive in ~1 s bursts so the device-side gap
cannot be resolved from the PC; distinct detections are **4 or 5**, not definitively 5.

Several confident peaks never fired at all: 6-8 s (0.565), 14-16 s (0.706), 32-34 s
(0.638) -- single windows crossing 0.50 with no second crossing nearby.

### Test 2 — 40 s of speech with the keyword never spoken

| Measurement | Value |
|---|---|
| **False fires** | **8 in 40.4 s** |
| **False activations per minute** | **11.88** |
| Windows at kw >= 0.50 | 30 / 185 (16.2 %) |
| Peak confidence on non-keyword speech | **0.999** |

## The headline

| | offline prediction | **measured live** |
|---|---|---|
| Detection rate | 68.4 % | **50 %** |
| False activations / min | 1.00 | **11.88** |

**The system is not usable.** It misses half the keywords and fires roughly twelve times a
minute on ordinary speech. No threshold or smoothing setting fixes this: strict-consecutive
and 2-of-3 voting were both measured on hardware and gave identical detection.

## Why the offline evaluation was optimistic — again

This is the second time, and for the same structural reason.

EXP-014 found that a test set of centred clips did not represent sliding windows. The fix
(training and testing on continuous windows) was correct but incomplete: **the held-out
windows are still drawn from the same narrow recording sessions as the training data.**
Those sessions were the user reading a fixed word list in one room. Test 2 was natural,
unconstrained speech.

So the negative class in evaluation is far narrower than the negative class in reality, and
the measured false rate is correspondingly optimistic -- by roughly **12x** here.

Peak confidence of 0.999 on words the model has never encountered is the signature: it is
not uncertain about unfamiliar speech, it is confidently wrong about it.

## What this proves

- The deployment path is correct and complete: features, quantisation constants, model and
  decision logic all reach the device faithfully, and the firmware behaves exactly as the
  Python evaluator describes.
- Detection genuinely works: 0.999-confidence firings on real spoken keywords.
- **Decision-logic tuning is exhausted.** Two different smoothing rules, measured on
  hardware, gave the same 50 %.

## What this does NOT prove

- Anything about a different speaker (DECISION-02 remains open).
- The plan's section-7 metric still is not measured; 40 s is not an hour, though at 11.88/min
  the direction is not in doubt.

## Decision

**Stop tuning. The single remaining lever is data**, and three independent measurements now
point at the same gap:

- EXP-008: 100 % of meaningful errors lay on the keyword/unknown boundary, from 68 source
  negative clips.
- EXP-014: the evaluation distribution did not match deployment.
- EXP-016: the negative class in evaluation is far narrower than real speech, by ~12x in
  measured false rate.

The negative class needs to represent *speech in general*, not sixteen chosen near-miss
words recorded in one sitting.
