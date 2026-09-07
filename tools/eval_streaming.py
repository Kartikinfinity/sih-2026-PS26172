"""Streaming evaluation: detections per utterance and FALSE ACTIVATIONS PER MINUTE.

Every accuracy figure in this project so far has been per-window or per-clip.
The plan's section 7 asks for something different: "false activations per hour
of continuous negative audio". Those are not the same quantity, and EXP-014
measured how far apart they can be (10.7 % per-clip vs 49.7 % per-window for the
same model).

This script runs the model over held-out audio IN TIME ORDER at the detector's
real cadence, applies threshold + N-consecutive smoothing exactly as the
firmware would, and reports:

  * detection rate  -- fraction of true utterances that produced a firing
  * false activations per minute -- firings not attributable to an utterance

Smoothing works because the two error types have different temporal structure: a
real utterance spans several consecutive windows, while isolated false windows
mostly do not. Requiring N in a row therefore suppresses false fires much faster
than true ones. That asymmetry only helps once the per-window false rate is
already low -- applying it earlier would have hidden a defect rather than fixed
one (EXP-013).

Usage:
    python tools/eval_streaming.py <modeldir> [dataset_region]
"""

import glob
import json
import os
import sys
import wave

import numpy as np
import tensorflow as tf

from features import FS, WINDOW_SAMPLES, N_FRAMES, mfcc
from record_dataset import find_bursts

HOP_S = 0.20            # the firmware runs inference every 200 ms
TEST_FRAC = 0.20        # same held-out region as build_continuous.py
MATCH_TOL_S = 0.60      # a firing counts as detecting an utterance if it lands
                        # within this of the utterance's span
POS_SESSIONS = "dataset/positive/_sessions/*.wav"
NEG_SESSIONS = "dataset/hard_negative/_sessions/*.wav"
EXCLUDE = ("sentinel-noisy",)


def read_wav(p):
    w = wave.open(p, "rb")
    x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    w.close()
    return x


def stream_probs(model, mean, std, x):
    """kw probability at each detector position, in time order."""
    step = int(HOP_S * FS)
    starts = list(range(0, len(x) - WINDOW_SAMPLES + 1, step))
    feats = np.stack([mfcc(x[s:s + WINDOW_SAMPLES], n_frames=N_FRAMES) for s in starts])
    X = ((feats - mean) / std).astype(np.float32)[..., None]
    p = tf.nn.softmax(model.predict(X, verbose=0, batch_size=256)).numpy()
    return np.array([s / FS for s in starts]), p[:, 0]


def fire_times(t, kw, thresh, m, n=None, refractory_s=1.2):
    """Fire when M of the last N windows are above threshold.

    Strict consecutiveness (the original rule) is brittle. A real utterance
    produces a probability that OSCILLATES around the threshold: the live test
    recorded 0.836, 0.481, 0.926, 0.805 across one spoken keyword. A single dip
    resets a consecutive counter, so a confidently-detected word was missed.
    Elsewhere the wobble happened to land two-in-a-row twice and fired twice for
    one utterance.

    M-of-N tolerates the dip; the refractory period stops one utterance being
    counted more than once.
    """
    if n is None:
        n = m
    above = (kw >= thresh).astype(int)
    fires = []
    last = -1e9
    for i in range(len(above)):
        lo = max(0, i - n + 1)
        if above[lo:i + 1].sum() >= m and (t[i] - last) >= refractory_s:
            fires.append(t[i])
            last = t[i]
    return fires


def main():
    mdir = sys.argv[1] if len(sys.argv) > 1 else "model_cont"
    n = json.load(open(os.path.join(mdir, "norm.json")))
    mean, std = np.array(n["mean"]), np.array(n["std"])
    model = tf.keras.models.load_model(os.path.join(mdir, "kws_float.keras"))

    sessions = [(p, True) for p in sorted(glob.glob(POS_SESSIONS))]
    sessions += [(p, False) for p in sorted(glob.glob(NEG_SESSIONS))]
    sessions = [(p, pos) for p, pos in sessions
                if not any(e in os.path.basename(p) for e in EXCLUDE)]

    segs = []       # (t, kw, utterances, positive, duration)
    total_s = 0.0
    total_utts = 0
    for path, positive in sessions:
        x = read_wav(path)
        utts, _f, _e, _tt, _r = find_bursts(x)
        cut = int(len(x) * (1 - TEST_FRAC))
        t0 = cut / FS
        seg = x[cut:]
        if len(seg) < WINDOW_SAMPLES:
            continue
        u = [(a - t0, b - t0) for a, b, _pk in utts if a >= t0]
        t, kw = stream_probs(model, mean, std, seg)
        segs.append((t, kw, u, positive, len(seg) / FS))
        total_s += len(seg) / FS
        if positive:
            total_utts += len(u)

    print("model: %s" % mdir)
    print("held-out audio: %.1f s (%.2f min) across %d sessions" % (total_s, total_s / 60, len(segs)))
    print("true keyword utterances in that audio: %d" % total_utts)
    print("")
    print("%-8s %-9s %10s %14s %16s" % ("thresh", "M-of-N", "detected", "missed", "false/min"))
    for thresh in (0.4, 0.5, 0.6):
        for (m_, n_) in ((1, 1), (2, 2), (2, 3), (2, 4), (3, 4), (3, 5)):
            det = miss = false = 0
            for t, kw, u, positive, dur in segs:
                f = fire_times(t, kw, thresh, m_, n_)
                used = [False] * len(f)
                if positive:
                    for (a, b) in u:
                        hit = None
                        for i, ft in enumerate(f):
                            if not used[i] and (a - MATCH_TOL_S) <= ft <= (b + MATCH_TOL_S):
                                hit = i
                                break
                        if hit is None:
                            miss += 1
                        else:
                            used[hit] = True
                            det += 1
                false += sum(1 for i in range(len(f)) if not used[i])
            rate = 60.0 * false / max(total_s, 1e-9)
            print("%-8.2f %-9s %8d/%d %13d %16.2f"
                  % (thresh, "%d-of-%d" % (m_, n_), det, total_utts, miss, rate))
    print("")
    print("NOTE: %.2f minutes of held-out audio is a small sample. A per-hour figure"
          % (total_s / 60))
    print("      extrapolated from it carries wide error bars and is not the plan's")
    print("      section-7 metric, which needs hours of genuine negative audio.")


if __name__ == "__main__":
    main()
