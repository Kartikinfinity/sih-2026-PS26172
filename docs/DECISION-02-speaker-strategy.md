# DECISION 02 — Speaker strategy

Date: 2026-09-06 · Phase 9 · Decided by the user

## Decision

**Start speaker-dependent (one speaker), add additional speakers before the demo if time
allows.** Explicitly staged, not abandoned.

## Context

A keyword spotter trained on a single voice will usually respond poorly or not at all to
other speakers. For an SIH demonstration this is a concrete risk: a judge picking up the
board, saying "Sentinel" and getting no response is a bad outcome even though the system
is behaving exactly as trained.

## Options considered

| Option | Cost | Consequence |
|---|---|---|
| Multi-speaker from the start | 3-5 people x ~20 utterances, ~15 min of their time | Works for strangers; slightly lower peak accuracy for the primary user |
| Single speaker only | Least effort | Highest accuracy for the user; likely fails for anyone else |
| **Staged (chosen)** | One extra training cycle | Unblocks progress today; the speaker-independence risk is deferred, not resolved |

## Consequences and open risk

- Phase 10-13 (baseline model, training, quantisation, offline validation) proceed now on
  single-speaker data.
- **Open risk, tracked:** until additional speakers are recorded, any accuracy figure
  applies to one speaker only. No claim of speaker independence may be made, and the
  demo carries a live-failure risk if a third party tries it.
- Adding speakers later requires re-running training, not re-recording the existing
  positives. The staged path costs a training cycle, not the dataset.
- The false-positive characterisation (Phase 17) should ideally include other people's
  speech as negatives regardless of this decision.

## Dataset targets set alongside this decision

Measured throughput: **24 usable utterances per 60 s session**.

| Class | Target | Source |
|---|---|---|
| Positives ("Sentinel") | ~250 | primary speaker, varied conditions |
| Hard negatives | ~200 | *sentimental, essential, central, signal, single, sending, seven, sensor* |
| Unknown speech | thousands | Google Speech Commands (public) |
| Background / silence | ~10 min | the actual room + public noise corpora |

The plan's stated 500-2000 positives assumes a speaker-independent model trained across
many voices. For a single-speaker baseline with augmentation, ~250 varied utterances is
the realistic figure, and variation across distance, level, speed, axis and background
matters more than raw count.
