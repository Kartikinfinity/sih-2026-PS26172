# EXP-014 — Training on continuous windows: the evaluation was the bug

| Field | Value |
|---|---|
| Experiment ID | EXP-014 |
| Date | 2026-09-06 |
| Phase | 9 rebuilt / 17 (false-positive characterisation) |
| Status | **Diagnosis confirmed. Neither model is deployable yet.** |

## The headline

Both models evaluated on the **same** 789 realistic sliding windows (keyword 123,
unknown 441, silence 225), held out by time from every session:

| @ threshold 0.90 | centred-clip trained (EXP-012, on device) | continuous-window trained |
|---|---|---|
| miss rate | **0.1301** | 0.6829 |
| **false-fire rate** | **0.4970** | **0.0015** |
| accuracy | 0.3181 | 0.6616 |

**The deployed model fires on 49.7 % of non-keyword windows.** Its previously reported
false-fire of 10.7 % was measured on curated centred clips, a distribution that does not
resemble deployment. The 49.7 % figure is consistent with the **34.9 %** observed live in
EXP-013 (the live audio contained more silence, which is the easy class).

**The evaluation was wrong before the model was.** Every accuracy figure from EXP-008
onward was computed against a test set of centred, complete words, and that is not the
input a sliding-window detector receives.

## What the new dataset does

`tools/build_continuous.py` slides a 1 s window in 100 ms steps across the full saved
session recordings and labels by how much of a known utterance the window contains. Every
raw session WAV was kept, so this required no new recordings.

## A label-design failure, and its fix

The first attempt used a single boundary at 0.95 overlap. A window containing 94 % of the
utterance was `unknown`; one containing 95 % was `keyword`. Those are nearly identical
audio with opposite labels -- not a learnable distinction.

Result: the model resolved it by almost never firing.

| First attempt, threshold 0.90 | |
|---|---|
| false-fire | 0.0055 |
| **miss** | **0.9205** |

A 92 % miss rate is not a keyword spotter. Fixed by introducing a **don't-care band**:

| Overlap | Label |
|---|---|
| >= 0.90 | keyword |
| 0.50 .. 0.90 | **excluded from the dataset entirely** |
| 0.05 .. 0.50 | unknown (clearly a partial word) |
| < 0.05 | silence |

Excluding ambiguous windows rather than forcing them into a class is standard practice for
sliding-window detection labelling.

## Where that leaves us

| Continuous-trained, on realistic windows | miss | false-fire |
|---|---|---|
| threshold 0.50 | 0.3415 | **0.0210** |
| threshold 0.70 | 0.4553 | 0.0090 |
| threshold 0.90 | 0.6829 | 0.0015 |

**Neither model is deployable.** The old one fires constantly; the new one misses a third
of keywords at its best operating point. They are two operating points of the same
limited underlying capability, and moving along that curve does not create accuracy.

What is genuinely better: the new model's failures are now *conservative*. A wake word
that occasionally misses is annoying; one that fires on half of all speech is unusable.

## Now-legitimate next steps

1. **Temporal smoothing (Phase 16).** Requiring N consecutive windows above threshold was
   deliberately refused in EXP-013, because applying it to a model with a ~50 % per-window
   false-fire rate would have hidden the defect. At 2.1 % per window it is now a genuine
   improvement rather than concealment: false fires are largely independent between
   windows, while a real utterance spans several consecutive ones.
2. **More keyword data.** 1,576 training windows derive from ~180 recorded utterances by
   one speaker. This is the binding constraint on the miss rate.
3. **Google Speech Commands** to broaden the unknown class -- justified since EXP-008 and
   reinforced twice.

## What remains unmeasured

- False activations per hour of continuous audio (the plan's section-7 metric). The
  per-window rates above are still not that number.
- Any speaker other than the one who recorded the data.
- On-device behaviour of the continuous-trained model: it has not been quantised or
  flashed. The board still runs the EXP-012 model.

## Decision

**Apply temporal smoothing and re-measure**, then address the miss rate with more data.
