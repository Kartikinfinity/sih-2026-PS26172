# EXP-018 — Class balance, and the honest ceiling of this dataset

| Field | Value |
|---|---|
| Experiment ID | EXP-018 |
| Date | 2026-09-07 |
| Phase | 11-17 |
| Status | **Best model so far. Still not usable. The limit is now measured, not argued.** |

## Three models, identical controlled live tests

Test 1 = "Sentinel" x10 and nothing else. Test 2 = 40 s of speech with the keyword
never spoken. Every event has a known label by construction.

| Model | Negatives in training | Detection | False activations/min |
|---|---|---|---|
| `model_cont` | 1,431 (ours only) | 5/10 | **11.88** |
| `model_aug` | 5,281 (+ Speech Commands) | **0-2/10** | 0.00 |
| **`model_bal`** | 5,281, keyword oversampled to 5,516 | **8/10** | 7.43 |

`model_bal` beats `model_cont` on **both** axes. That is a real improvement, not a trade.

## What EXP-017 got wrong, and how it showed

Importing Speech Commands eliminated false fires (11.88 -> 0.00/min) and was declared a
success on that evidence. Fresh validation then measured **0/10 detection with keyword
probability never exceeding 0.205**, against 0.776 in the earlier run.

The diagnostic that settled it: 125 of 185 windows were classified `unknown`, *more* speech
than the previous run rather than less. The microphone heard every utterance; the model
called them all unknown. It had solved false fires by becoming unwilling to say "keyword"
at all -- a degenerate solution, and the mirror of EXP-012's collapse to `silence`.

Cause: the unknown class held 5,281 windows from ~2,000 speakers while keyword held 1,576
from one. Class weights did not compensate for that diversity gap. Oversampling the keyword
class (TRAIN_COPIES 4 -> 14, giving 5,516) keeps all the negative variety while restoring
comparable representation.

**This is why a single measurement is not a result.** EXP-017's 0.00 false/min was real and
also meaningless on its own.

## The measured Pareto frontier

Swept on the fresh `model_bal` captures. Nothing beats these on both axes:

| threshold | rule | detection | false/min |
|---|---|---|---|
| 0.50 | 1-of-1 | **8/10** | 7.43 |
| 0.50 | 2-of-3 | 5/10 | 2.97 |
| **0.70** | **2-of-2** | 4/10 | **0.00** |

**There is no operating point that is good on both axes.** 80 % detection costs 7.4 false
activations per minute; zero false activations costs 60 % of detections.

Deployed: **0.70, 2-of-2** -- a device that never fires unprompted and occasionally needs
the word repeated is usable for a demonstration; one that fires seven times a minute is not.

## What the numbers say about the cause

`model_bal`, Test 1: keyword peaks reach 0.995, 0.987, 0.957, 0.954 while the between-word
median sits at 0.021. Separation is genuinely there.

Test 2: false fires peaked at 0.967, 0.912, 0.881, 0.868. **The overlap is in the tail** --
some non-keyword speech produces responses as strong as real keywords.

That is what a thin positive class looks like. The model has 5,281 negative windows from
~2,000 speakers and 1,576 *distinct* keyword windows derived from roughly 180 utterances by
**one person, one room, one day** -- oversampling copies them, it does not add information.

## What is now proven

Three interventions, each measured on hardware:

1. More negative data alone -> false fires solved, detection destroyed.
2. Rebalancing -> detection restored to 80 %, false fires partly returned.
3. Threshold and smoothing sweeps -> move along the frontier, never past it.

**Decision-logic tuning and negative-data collection are both exhausted.** The remaining
lever is keyword recordings, and ideally from more than one speaker (DECISION-02, still
open).

## What remains unmeasured

- Any speaker other than the one who recorded the data.
- The plan's section-7 metric (per hour). These are 40-second samples.
- Both operating points were tuned on the same audio they were validated against.
  Provisional until re-measured on fresh captures.
