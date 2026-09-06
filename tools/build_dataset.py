"""Build the training set: split, augment, extract features.

Classes:
    0  keyword    "Sentinel"
    1  unknown    hard negatives (phonetically near-miss and ordinary words)
    2  silence    background / room noise, no speech

Split policy
------------
Clips are held out TEMPORALLY within each recording session, not at random.
Utterances recorded seconds apart share room state, mic placement, voice warmth
and background. A random split scatters near-duplicates across train and test
and reports an accuracy that will not survive contact with reality. Sorting by
filename recovers recording order, so the last 20 % of each session becomes
test and the 15 % before it validation.

This reduces leakage; it does not eliminate it. Every clip still comes from one
speaker in one room on one day. See DECISION-02 for the speaker-independence
risk that no split policy can fix.

Augmentation applies to TRAINING data only. Validation and test use a
deterministic centre crop and a fixed SNR ladder, so scores are comparable
across runs.

Usage:
    python tools/build_dataset.py [outfile.npz]
"""

import glob
import os
import sys
import wave

import numpy as np

from features import FS, WINDOW_SAMPLES, N_FRAMES, N_MFCC, mfcc, mix_at_snr

POS_DIR = "dataset/positive"
NEG_DIR = "dataset/hard_negative"
NOISE_DIR = "dataset/noise"

TEST_FRAC = 0.20
VAL_FRAC = 0.15

AUG_PER_POS = 8          # augmented copies of each training positive
AUG_PER_NEG = 6
SILENCE_TRAIN = 700      # synthesised background windows
TRAIN_SNRS = [None, 25.0, 20.0, 15.0, 10.0, 5.0]   # None = clean
EVAL_SNRS = [None, 20.0, 10.0, 5.0]

SEED = 20260906


def read_wav(path):
    w = wave.open(path, "rb")
    x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    w.close()
    return x


def session_of(path):
    """'sentinel-close_0042.wav' -> 'sentinel-close'."""
    return os.path.basename(path).rsplit("_", 1)[0]


def split_by_session(files):
    """Temporal hold-out within each session. Returns (train, val, test)."""
    groups = {}
    for f in files:
        groups.setdefault(session_of(f), []).append(f)
    train, val, test = [], [], []
    for name in sorted(groups):
        g = sorted(groups[name])
        n = len(g)
        n_test = max(1, int(round(n * TEST_FRAC)))
        n_val = max(1, int(round(n * VAL_FRAC)))
        test += g[n - n_test:]
        val += g[n - n_test - n_val:n - n_test]
        train += g[:n - n_test - n_val]
    return train, val, test


def crop(x, rng=None):
    """1.5 s clip -> 1.0 s window. Random offset when training, else centred."""
    if len(x) <= WINDOW_SAMPLES:
        out = np.zeros(WINDOW_SAMPLES)
        out[:len(x)] = x
        return out
    slack = len(x) - WINDOW_SAMPLES
    off = int(rng.integers(0, slack + 1)) if rng is not None else slack // 2
    return x[off:off + WINDOW_SAMPLES]


def featurise(x):
    return mfcc(x, n_frames=N_FRAMES)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "dataset/kws_dataset.npz"
    rng = np.random.default_rng(SEED)

    pos = sorted(glob.glob(os.path.join(POS_DIR, "*.wav")))
    neg = sorted(glob.glob(os.path.join(NEG_DIR, "*.wav")))
    noises = [read_wav(p) for p in sorted(glob.glob(os.path.join(NOISE_DIR, "*.wav")))]
    if not pos or not neg:
        sys.exit("[ERROR] no positives or negatives found")
    if not noises:
        sys.exit("[ERROR] no background noise found -- run tools/record_noise.py")
    print("positives %d | hard negatives %d | noise files %d" % (len(pos), len(neg), len(noises)))

    ptr, pva, pte = split_by_session(pos)
    ntr, nva, nte = split_by_session(neg)
    print("positives  train %d  val %d  test %d" % (len(ptr), len(pva), len(pte)))
    print("negatives  train %d  val %d  test %d" % (len(ntr), len(nva), len(nte)))

    def build(files, label, augs, training):
        X, y = [], []
        for f in files:
            x = read_wav(f)
            if training:
                for _ in range(augs):
                    c = crop(x, rng)
                    c = c * float(rng.uniform(0.5, 1.6))          # gain jitter
                    snr = TRAIN_SNRS[int(rng.integers(0, len(TRAIN_SNRS)))]
                    if snr is not None:
                        c = mix_at_snr(c, noises[int(rng.integers(0, len(noises)))], snr, rng)
                    X.append(featurise(c))
                    y.append(label)
            else:
                for snr in EVAL_SNRS:
                    c = crop(x)
                    if snr is not None:
                        c = mix_at_snr(c, noises[0], snr, rng)
                    X.append(featurise(c))
                    y.append(label)
        return X, y

    def silence(n, training):
        X, y = [], []
        for _ in range(n):
            src = noises[int(rng.integers(0, len(noises)))]
            start = int(rng.integers(0, len(src) - WINDOW_SAMPLES))
            c = src[start:start + WINDOW_SAMPLES].astype(np.float64)
            c = c * float(rng.uniform(0.15, 1.4)) if training else c
            X.append(featurise(c))
            y.append(2)
        return X, y

    print("building train...")
    Xtr, ytr = [], []
    for fn, lb, ag in ((ptr, 0, AUG_PER_POS), (ntr, 1, AUG_PER_NEG)):
        a, b = build(fn, lb, ag, True)
        Xtr += a
        ytr += b
    a, b = silence(SILENCE_TRAIN, True)
    Xtr += a
    ytr += b

    print("building val/test...")
    Xva, yva, Xte, yte = [], [], [], []
    for fn, lb in ((pva, 0), (nva, 1)):
        a, b = build(fn, lb, 0, False)
        Xva += a
        yva += b
    for fn, lb in ((pte, 0), (nte, 1)):
        a, b = build(fn, lb, 0, False)
        Xte += a
        yte += b
    a, b = silence(int(0.2 * SILENCE_TRAIN), False)
    Xva += a
    yva += b
    a, b = silence(int(0.2 * SILENCE_TRAIN), False)
    Xte += a
    yte += b

    Xtr = np.asarray(Xtr, np.float32); ytr = np.asarray(ytr, np.int64)
    Xva = np.asarray(Xva, np.float32); yva = np.asarray(yva, np.int64)
    Xte = np.asarray(Xte, np.float32); yte = np.asarray(yte, np.int64)

    perm = rng.permutation(len(Xtr))
    Xtr, ytr = Xtr[perm], ytr[perm]

    print("")
    for name, X, y in (("train", Xtr, ytr), ("val", Xva, yva), ("test", Xte, yte)):
        counts = np.bincount(y, minlength=3)
        print("%-6s %6d examples  shape %s  | keyword %d  unknown %d  silence %d"
              % (name, len(X), X.shape[1:], counts[0], counts[1], counts[2]))

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    np.savez_compressed(out, Xtr=Xtr, ytr=ytr, Xva=Xva, yva=yva, Xte=Xte, yte=yte,
                        n_frames=N_FRAMES, n_mel=N_MFCC, classes=np.array(
                            ["keyword", "unknown", "silence"]))
    print("")
    print("saved %s (%.1f MB)" % (out, os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main()
