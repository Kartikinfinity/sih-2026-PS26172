# DECISION 01 — Keyword selection

Date: 2026-09-06 · Phase 8 · Decided by the user after criteria review

## Decision

**Keyword: "Sentinel"** · **Language: English**

## Why this is recorded as a decision, not an experiment

Changing the keyword after Phase 9 begins invalidates the recorded dataset, the trained
model, every tuned threshold, and the entire false-positive characterisation. The wiring,
filters and feature pipeline survive a keyword change; the data does not. This is the
least reversible choice in the project.

## Criteria applied

| # | Criterion | How "Sentinel" scores |
|---|---|---|
| 1 | Length (3–4 syllables) | 3 syllables, ~0.7 s → ~70 feature frames of evidence at our 100 fps |
| 2 | Phonetic variety | Four distinct consonant classes: fricative `/s/`, plosive `/t/`, nasal `/n/`, liquid `/l/` |
| 3 | Rarity in ordinary speech | Effectively never spoken casually; not a fragment of a common word |
| 4 | Strong onset | `/s/` gives a crisp boundary, which also helps end-of-keyword timestamping for the §7 latency measurement |
| 5 | Fit to *our measured* hardware | see below |
| 6 | Repeatable under recording fatigue | Easy to articulate consistently several hundred times |

## Criterion 5 — grounded in our own measurements, not generic advice

EXP-003 measured this microphone's ambient noise as **64.6 % of energy below 100 Hz** and
only **0.02 % above 3400 Hz**. The high band is the quietest part of our noise spectrum,
and our Mel filterbank extends to 7500 Hz, so we capture it.

A sibilant `/s/` places strong energy at roughly 4–8 kHz — precisely where our measured
noise floor is lowest. The keyword's most distinctive feature therefore lands in our
highest-SNR region essentially for free.

**Caveat:** that 0.02 % figure comes from a single 5-second capture in one room. It is
directionally reliable, not a universal constant, and has not been re-measured across
environments.

## Language rationale

English was chosen for a practical, not acoustic, reason. The pipeline is
language-agnostic, but large public English corpora (Google Speech Commands, standard
noise datasets) supply hard negatives and background audio, substantially reducing the
volume of negative data the user must record personally. A Hindi or regional-language
keyword would have been acoustically fine but would have required considerably more
manual recording.

## Rejected alternatives

| Candidate | Reason rejected |
|---|---|
| "Computer" | A common noun — anyone discussing computers could trigger it |
| "Activate" | Sound choice, but no sibilant, so it forgoes the high-band SNR advantage |
| "Navigator" | 4 syllables gives the lowest false-accept risk, but higher recording fatigue and slower in a live demo |
| "Hey", "Go", "On", "Start", "Open", "Hello" | Too short, common in speech, and fragments of other words — a permanent false-positive source |

## Consequences

- The positive class is utterances of "Sentinel".
- Hard negatives must include phonetically nearby words. Candidates to record later:
  *sentimental, essential, central, single, signal, sending, seven*. Similarity to the
  keyword is what makes a negative "hard" and therefore useful.
- All dataset recordings use the verified capture path (16 kHz, mono, 16-bit, raw
  unfiltered) so that training-time audio matches what the device feeds its feature
  extractor at inference time.
