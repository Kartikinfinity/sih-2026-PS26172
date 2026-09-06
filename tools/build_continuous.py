"""Build a training set from CONTINUOUS session audio, including partial words.

Why this exists
---------------
EXP-013 measured 34.9 % of live inference frames firing, against 10.7 % on the
curated test set. Parity was verified, so the features were not at fault. The
cause is dataset design: every training clip is a 1.5 s file with the word
centred, and random cropping only shifts by +/-0.25 s, so **no training example
is ever a half-word**. In continuous operation the model gets a fresh 1 s window
every 200 ms, and most of those contain a partial word, a transition, or the
tail of one word plus the head of the next. Its behaviour there was never
trained, and it resolves the ambiguity towards "keyword".

This builder slides a window across the full saved session recordings and labels
each position by how much of a known utterance it actually contains:

    >= KEYWORD_OVERLAP of the utterance inside  -> keyword
    PARTIAL_MIN .. KEYWORD_OVERLAP              -> unknown   <-- the missing class
    < PARTIAL_MIN                               -> silence

The partial-overlap windows are the entire point. They teach the model that
"most of a Sentinel" is not Sentinel.

Splitting is TEMPORAL WITHIN EACH SESSION: the last 20 % of each recording by
time is test, the 15 % before it validation. Windows overlap heavily, so any
random split would scatter near-identical windows across train and test and
report a meaningless score.

Usage:
    python tools/build_continuous.py [out.npz]
"""

import glob
import os
import sys
import wave

import numpy as np

from features import FS, WINDOW_SAMPLES, N_FRAMES, N_MFCC, mfcc, mix_at_snr
from record_dataset import find_bursts

WINDOW_STEP_S = 0.10          # dense sampling; the detector runs every 0.20 s

# A DON'T-CARE BAND sits between the positive and negative rules.
#
# The first attempt used a single boundary at 0.95: a window holding 94 % of the
# utterance was "unknown" and one holding 95 % was "keyword". Those windows are
# nearly identical audio with opposite labels, which is not a learnable
# distinction. The model resolved it by almost never firing -- false-fire fell
# to 0.55 % but the miss rate rose to 92.1 %, which is not a keyword spotter.
#
# Windows in the ambiguous band are now EXCLUDED from the dataset entirely
# rather than forced into a class. This is standard practice for sliding-window
# detection labelling.
KEYWORD_OVERLAP = 0.90        # >= this much of the utterance inside -> keyword
AMBIGUOUS_MIN = 0.50          # 0.50 .. 0.90 -> excluded, neither class
PARTIAL_MIN = 0.05            # 0.05 .. 0.50 -> unknown (clearly a partial word)
                              # < 0.05       -> silence

POS_SESSIONS = "dataset/positive/_sessions/*.wav"
NEG_SESSIONS = "dataset/hard_negative/_sessions/*.wav"
NOISE_DIR = "dataset/noise"

# The noisy session's segmentation failed (EXP-006 note: dynamic range 3.9x), so
# its utterance boundaries are not trustworthy and it must not be auto-labelled.
EXCLUDE = ("sentinel-noisy",)

TEST_FRAC, VAL_FRAC = 0.20, 0.15
MAX_PER_CLASS_PER_SESSION = 400
# Keyword windows are rare in continuous audio by construction -- only a narrow
# band of window positions contains a whole utterance. Emitting several
# differently-augmented copies of each keeps the class from being starved
# WITHOUT widening the label rule, which would reintroduce partial words as
# positives and defeat the point of this dataset.
TRAIN_COPIES = {0: 4, 1: 1, 2: 1}
AUG_SNRS = [None, None, 20.0, 15.0, 10.0, 5.0]
SEED = 20260906


def read_wav(p):
    w = wave.open(p, "rb")
    x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    w.close()
    return x


def label_windows(x, utterances, positive):
    """Yield (start_sample, label) for each window position."""
    step = int(WINDOW_STEP_S * FS)
    n = len(x)
    for s in range(0, n - WINDOW_SAMPLES + 1, step):
        w0, w1 = s / FS, (s + WINDOW_SAMPLES) / FS
        best = 0.0
        for a, b, _pk in utterances:
            dur = b - a
            if dur <= 0:
                continue
            inside = max(0.0, min(b, w1) - max(a, w0))
            best = max(best, inside / dur)
        if best >= KEYWORD_OVERLAP:
            yield s, (0 if positive else 1)
        elif best >= AMBIGUOUS_MIN:
            continue                        # don't-care band: not a training example
        elif best >= PARTIAL_MIN:
            yield s, 1                      # partial word -> unknown
        else:
            yield s, 2                      # background


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "dataset/kws_continuous.npz"
    rng = np.random.default_rng(SEED)
    noises = [read_wav(p) for p in sorted(glob.glob(os.path.join(NOISE_DIR, "*.wav")))]

    sessions = [(p, True) for p in sorted(glob.glob(POS_SESSIONS))]
    sessions += [(p, False) for p in sorted(glob.glob(NEG_SESSIONS))]
    sessions = [(p, pos) for p, pos in sessions
                if not any(e in os.path.basename(p) for e in EXCLUDE)]

    buckets = {"train": [[], []], "val": [[], []], "test": [[], []]}
    print("%-42s %8s %7s %7s %7s" % ("session", "utts", "keyword", "unknown", "silence"))

    for path, positive in sessions:
        x = read_wav(path)
        utts, _floor, _env, _t, _rej = find_bursts(x)
        dur = len(x) / FS
        t_test = dur * (1 - TEST_FRAC)
        t_val = dur * (1 - TEST_FRAC - VAL_FRAC)

        per = {"train": [0, 0, 0], "val": [0, 0, 0], "test": [0, 0, 0]}
        items = list(label_windows(x, utts, positive))
        rng.shuffle(items)
        for s, lab in items:
            centre = (s + WINDOW_SAMPLES / 2) / FS
            split = "test" if centre >= t_test else ("val" if centre >= t_val else "train")
            if per[split][lab] >= MAX_PER_CLASS_PER_SESSION:
                continue
            per[split][lab] += 1
            base = x[s:s + WINDOW_SAMPLES].astype(np.float64)
            copies = TRAIN_COPIES.get(lab, 1) if split == "train" else 1
            for _ in range(copies):
                c = base
                if split == "train":
                    c = c * float(rng.uniform(0.6, 1.5))
                    snr = AUG_SNRS[int(rng.integers(0, len(AUG_SNRS)))]
                    if snr is not None and noises:
                        c = mix_at_snr(c, noises[int(rng.integers(0, len(noises)))],
                                       snr, rng)
                buckets[split][0].append(mfcc(c, n_frames=N_FRAMES))
                buckets[split][1].append(lab)
        tot = [per["train"][i] + per["val"][i] + per["test"][i] for i in range(3)]
        print("%-42s %8d %7d %7d %7d"
              % (os.path.basename(path)[:42], len(utts), tot[0], tot[1], tot[2]))

    data = {}
    for split, key in (("train", "tr"), ("val", "va"), ("test", "te")):
        X = np.asarray(buckets[split][0], np.float32)
        y = np.asarray(buckets[split][1], np.int64)
        if split == "train":
            p = rng.permutation(len(X))
            X, y = X[p], y[p]
        data["X" + key], data["y" + key] = X, y
        c = np.bincount(y, minlength=3)
        print("%-6s %6d windows  | keyword %d  unknown %d  silence %d"
              % (split, len(X), c[0], c[1], c[2]))

    np.savez_compressed(out, n_frames=N_FRAMES, n_mel=N_MFCC,
                        classes=np.array(["keyword", "unknown", "silence"]), **data)
    print("")
    print("saved %s (%.1f MB)" % (out, os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main()
