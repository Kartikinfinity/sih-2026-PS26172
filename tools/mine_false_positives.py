"""Hard-negative mining: find what the CURRENT model actually falsely fires on.

This closes the loop the project has been missing:

    TRAIN -> DEPLOY-LIKE TEST -> FIND FALSE TRIGGERS -> MINE -> RETRAIN -> TEST

Every negative used so far was chosen by a human guessing what might confuse the
model ("sentimental", "essential", "central"...). That guess was reasonable but
it is not evidence. This script instead runs the trained model over every second
of audio we possess and records the windows where it actually fires when it
should not. Those windows ARE the confusable set, discovered rather than assumed.

Sources searched, all already on disk:
  * our own session recordings  -- windows far from any true utterance
  * our hard-negative sessions  -- the keyword is never spoken, so ANY firing is false
  * Google Speech Commands      -- 35 words, ~2,000 speakers, none is the keyword

A window counts as a false positive when the model's keyword probability crosses
the mining threshold and the window does NOT overlap a genuine utterance. The
mining threshold is deliberately LOWER than the deployment threshold: windows
that came close are the informative ones, not only those that crossed the line.

Output is an .npz of mined features plus a provenance report saying where each
false trigger came from -- which is the diagnostic the project has never had.

Usage:
    python tools/mine_false_positives.py <modeldir> [out.npz] [speech_commands_dir]
"""

import glob
import json
import os
import sys
import wave
from collections import Counter

import numpy as np
import tensorflow as tf

from features import FS, WINDOW_SAMPLES, N_FRAMES, N_MFCC, mfcc
from record_dataset import find_bursts

MINE_THRESHOLD = 0.35     # below the 0.70 deployment threshold on purpose
STEP_S = 0.20             # the detector's real cadence
SAFE_MARGIN_S = 0.80      # a window this far from any utterance is definitely negative
SC_PER_WORD = 60          # Speech Commands clips to scan per word
BATCH = 256
SEED = 20260908

POS_SESSIONS = "dataset/positive/_sessions/*.wav"
NEG_SESSIONS = "dataset/hard_negative/_sessions/*.wav"
EXCLUDE = ("sentinel-noisy",)


def read_wav(p):
    try:
        w = wave.open(p, "rb")
        if w.getnchannels() != 1 or w.getsampwidth() != 2 or w.getframerate() != FS:
            w.close()
            return None
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
        w.close()
        return x
    except Exception:
        return None


def score(model, mean, std, feats):
    X = ((np.asarray(feats, np.float32) - mean) / std).astype(np.float32)[..., None]
    return tf.nn.softmax(model.predict(X, verbose=0, batch_size=BATCH)).numpy()[:, 0]


def main():
    mdir = sys.argv[1] if len(sys.argv) > 1 else "model_bal"
    out = sys.argv[2] if len(sys.argv) > 2 else "dataset/mined_negatives.npz"
    sc_dir = sys.argv[3] if len(sys.argv) > 3 else "D:/speech_commands/data"
    rng = np.random.default_rng(SEED)

    nrm = json.load(open(os.path.join(mdir, "norm.json")))
    mean, std = np.array(nrm["mean"]), np.array(nrm["std"])
    model = tf.keras.models.load_model(os.path.join(mdir, "kws_float.keras"))
    print("model: %s | mining threshold %.2f (deployment uses 0.70)" % (mdir, MINE_THRESHOLD))

    mined, provenance, scanned = [], [], 0

    # ---- our own sessions -------------------------------------------------
    sessions = [(p, True) for p in sorted(glob.glob(POS_SESSIONS))]
    sessions += [(p, False) for p in sorted(glob.glob(NEG_SESSIONS))]
    sessions = [(p, pos) for p, pos in sessions
                if not any(e in os.path.basename(p) for e in EXCLUDE)]

    for path, positive in sessions:
        x = read_wav(path)
        if x is None:
            continue
        utts, _f, _e, _t, _r = find_bursts(x) if positive else ([], 0, 0, 0, 0)
        step = int(STEP_S * FS)
        starts, feats = [], []
        for s in range(0, len(x) - WINDOW_SAMPLES + 1, step):
            w0, w1 = s / FS, (s + WINDOW_SAMPLES) / FS
            # keep only windows safely clear of any genuine utterance
            near = any(not (w1 < a - SAFE_MARGIN_S or w0 > b + SAFE_MARGIN_S)
                       for a, b, _pk in utts)
            if near:
                continue
            starts.append(s)
            feats.append(mfcc(x[s:s + WINDOW_SAMPLES], n_frames=N_FRAMES))
        if not feats:
            continue
        p = score(model, mean, std, feats)
        scanned += len(feats)
        hit = np.where(p >= MINE_THRESHOLD)[0]
        for i in hit:
            mined.append(feats[i])
            provenance.append("%s @ %.1fs (p=%.3f)"
                              % (os.path.basename(path)[:34], starts[i] / FS, p[i]))
        print("  %-46s scanned %4d  mined %3d" % (os.path.basename(path)[:46],
                                                  len(feats), len(hit)))

    # ---- Speech Commands ---------------------------------------------------
    words = sorted(w for w in os.listdir(sc_dir)
                   if os.path.isdir(os.path.join(sc_dir, w)) and not w.startswith("_"))
    print("scanning Speech Commands (%d words, %d clips each)..." % (len(words), SC_PER_WORD))
    per_word = Counter()
    for w in words:
        files = sorted(glob.glob(os.path.join(sc_dir, w, "*.wav")))
        if not files:
            continue
        idx = rng.choice(len(files), size=min(SC_PER_WORD, len(files)), replace=False)
        feats = []
        for i in idx:
            x = read_wav(files[i])
            if x is None:
                continue
            if len(x) < WINDOW_SAMPLES:
                x = np.pad(x, (0, WINDOW_SAMPLES - len(x)))
            feats.append(mfcc(x[:WINDOW_SAMPLES], n_frames=N_FRAMES))
        if not feats:
            continue
        p = score(model, mean, std, feats)
        scanned += len(feats)
        for i in np.where(p >= MINE_THRESHOLD)[0]:
            mined.append(feats[i])
            provenance.append("speechcommands/%s (p=%.3f)" % (w, p[i]))
            per_word[w] += 1

    print("")
    print("=== MINING RESULT ===")
    print("windows scanned      : %d" % scanned)
    print("false positives mined: %d  (%.2f %% of scanned)"
          % (len(mined), 100.0 * len(mined) / max(scanned, 1)))
    if per_word:
        print("")
        print("WHICH WORDS ACTUALLY FOOL THE MODEL (top 15):")
        for w, c in per_word.most_common(15):
            print("   %-12s %3d / %d clips  (%.0f %%)" % (w, c, SC_PER_WORD,
                                                          100.0 * c / SC_PER_WORD))
    if mined:
        np.savez_compressed(out, X=np.asarray(mined, np.float32),
                            provenance=np.array(provenance),
                            n_frames=N_FRAMES, n_mel=N_MFCC)
        print("")
        print("saved %s (%d windows, %.1f MB)"
              % (out, len(mined), os.path.getsize(out) / 1e6))
    else:
        print("no false positives found at threshold %.2f" % MINE_THRESHOLD)


if __name__ == "__main__":
    main()
