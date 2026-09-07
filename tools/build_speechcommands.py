"""Add Google Speech Commands to the "unknown" class and rebuild the dataset.

Why
---
Three independent measurements point at the same gap:

  EXP-008  100 % of meaningful errors lay on the keyword/unknown boundary, with
           only 68 source negative clips.
  EXP-014  the evaluation distribution did not match deployment.
  EXP-016  the negative class is ~12x narrower than real speech. Measured live:
           11.88 false activations per minute, with peak confidence 0.999 on
           words the model had never encountered.

Sixteen near-miss words recorded in one sitting cannot teach a model what "not
the keyword" means across a language. Speech Commands supplies ~105,000
one-second utterances of 35 words from ~2,000 speakers.

Format note: Speech Commands clips are 1 s, 16 kHz, mono, 16-bit -- identical to
our window, so they need no resampling.

THE HAZARD, and what is done about it
-------------------------------------
These clips were recorded on other people's microphones in other rooms. If every
Speech Commands clip is "unknown" and every INMP441 clip is "keyword", the model
can get a good score by learning **which microphone recorded the clip** instead
of which word was spoken. That would look excellent offline and fail completely
on the device, which only ever hears one microphone.

Three defences:

  1. Our own recordings already supply thousands of "unknown" examples (hard
     negatives and partial words), so channel does not separate the classes.
  2. Every imported clip is rescaled to the RMS distribution measured from our
     own recordings, removing loudness as a shortcut cue.
  3. Every imported clip is mixed with OUR room noise, so the background matches.

This reduces the shortcut; it does not eliminate it. The honest test is the
on-device measurement, not the offline score.

Usage:
    python tools/build_speechcommands.py <speech_commands_dir> [out.npz]
"""

import glob
import os
import random
import sys
import wave

import numpy as np

from features import FS, WINDOW_SAMPLES, N_FRAMES, N_MFCC, mfcc, mix_at_snr

CONTINUOUS_NPZ = "dataset/kws_continuous.npz"
NOISE_DIR = "dataset/noise"

PER_WORD_TRAIN = 110      # clips sampled per word for training
PER_WORD_EVAL = 22        # and for val/test
AUG_SNRS = [None, 20.0, 15.0, 10.0]
SEED = 20260907

# Speech Commands ships these as background, not speech.
SKIP_DIRS = {"_background_noise_"}


def read_wav(p):
    w = wave.open(p, "rb")
    n, sw, fr = w.getnchannels(), w.getsampwidth(), w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    w.close()
    if n != 1 or sw != 2 or fr != FS:
        return None
    return x


def fit_window(x):
    """Pad or centre-crop to exactly WINDOW_SAMPLES."""
    if len(x) == WINDOW_SAMPLES:
        return x
    if len(x) < WINDOW_SAMPLES:
        out = np.zeros(WINDOW_SAMPLES)
        off = (WINDOW_SAMPLES - len(x)) // 2
        out[off:off + len(x)] = x
        return out
    off = (len(x) - WINDOW_SAMPLES) // 2
    return x[off:off + WINDOW_SAMPLES]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sc_dir = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "dataset/kws_augmented.npz"
    rng = np.random.default_rng(SEED)
    random.seed(SEED)

    d = np.load(CONTINUOUS_NPZ, allow_pickle=True)
    noises = [read_wav(p) for p in sorted(glob.glob(os.path.join(NOISE_DIR, "*.wav")))]
    noises = [n for n in noises if n is not None]
    if not noises:
        sys.exit("[ERROR] no room noise found -- needed to match backgrounds")

    # Loudness reference measured from OUR OWN recordings, so imported clips
    # cannot be told apart by level alone.
    our_pos = sorted(glob.glob("dataset/positive/*.wav"))
    ref = [np.sqrt((read_wav(p) ** 2).mean()) for p in our_pos[:120]]
    ref = np.array([r for r in ref if r and np.isfinite(r)])
    print("our recordings: RMS median %.0f  (p10 %.0f .. p90 %.0f)"
          % (np.median(ref), np.percentile(ref, 10), np.percentile(ref, 90)))

    words = sorted(x for x in os.listdir(sc_dir)
                   if os.path.isdir(os.path.join(sc_dir, x)) and x not in SKIP_DIRS)
    print("Speech Commands words found: %d" % len(words))
    if not words:
        sys.exit("[ERROR] no word directories in %s" % sc_dir)

    def import_split(per_word, training):
        X, y = [], []
        for w in words:
            files = sorted(glob.glob(os.path.join(sc_dir, w, "*.wav")))
            if not files:
                continue
            pick = random.sample(files, min(per_word, len(files)))
            for f in pick:
                x = read_wav(f)
                if x is None:
                    continue
                x = fit_window(x)
                r = np.sqrt((x ** 2).mean())
                if r < 1e-6:
                    continue
                # match our loudness distribution
                target = float(rng.choice(ref)) * float(rng.uniform(0.7, 1.4))
                x = x * (target / r)
                # match our room background
                snr = AUG_SNRS[int(rng.integers(0, len(AUG_SNRS)))]
                if snr is not None:
                    x = mix_at_snr(x, noises[int(rng.integers(0, len(noises)))], snr, rng)
                X.append(mfcc(x, n_frames=N_FRAMES))
                y.append(1)          # everything imported is "unknown"
        return X, y

    print("importing training clips (%d per word)..." % PER_WORD_TRAIN)
    Xa, ya = import_split(PER_WORD_TRAIN, True)
    print("  %d clips" % len(Xa))
    print("importing val/test clips (%d per word)..." % PER_WORD_EVAL)
    Xb, yb = import_split(PER_WORD_EVAL, False)
    half = len(Xb) // 2
    print("  %d clips (split %d val / %d test)" % (len(Xb), half, len(Xb) - half))

    def cat(base_X, base_y, add_X, add_y):
        if not add_X:
            return base_X, base_y
        return (np.concatenate([base_X, np.asarray(add_X, np.float32)]),
                np.concatenate([base_y, np.asarray(add_y, np.int64)]))

    Xtr, ytr = cat(d["Xtr"], d["ytr"], Xa, ya)
    Xva, yva = cat(d["Xva"], d["yva"], Xb[:half], yb[:half])
    Xte, yte = cat(d["Xte"], d["yte"], Xb[half:], yb[half:])

    perm = rng.permutation(len(Xtr))
    Xtr, ytr = Xtr[perm], ytr[perm]

    print("")
    for name, X, y in (("train", Xtr, ytr), ("val", Xva, yva), ("test", Xte, yte)):
        c = np.bincount(y, minlength=3)
        print("%-6s %6d  | keyword %d  unknown %d  silence %d"
              % (name, len(X), c[0], c[1], c[2]))

    np.savez_compressed(out, Xtr=Xtr, ytr=ytr, Xva=Xva, yva=yva, Xte=Xte, yte=yte,
                        n_frames=N_FRAMES, n_mel=N_MFCC,
                        classes=np.array(["keyword", "unknown", "silence"]))
    print("")
    print("saved %s (%.1f MB)" % (out, os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main()
