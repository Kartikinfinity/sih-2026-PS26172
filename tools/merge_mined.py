"""Fold mined false positives into the training set, weighted.

Mined windows are not ordinary negatives. Each one is a case where the CURRENT
model is demonstrably wrong -- discovered by running it over every second of
audio we possess, rather than guessed by a human choosing plausible-sounding
confusable words. They are the most informative negatives available, and there
are few of them, so they are repeated to carry real weight in the loss.

They go into TRAIN ONLY. Putting mined examples in validation or test would
measure the model against the very windows it was corrected on, which inflates
the score and hides whether the correction generalises.

Usage:
    python tools/merge_mined.py <base.npz> <mined.npz> <out.npz> [copies]
"""

import os
import sys

import numpy as np

DEFAULT_COPIES = 20


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    base_p, mined_p, out = sys.argv[1], sys.argv[2], sys.argv[3]
    copies = int(sys.argv[4]) if len(sys.argv) > 4 else DEFAULT_COPIES

    d = dict(np.load(base_p, allow_pickle=True))
    m = np.load(mined_p, allow_pickle=True)
    Xm = m["X"]
    if len(Xm) == 0:
        sys.exit("[ERROR] no mined windows")

    prov = m["provenance"] if "provenance" in m else np.array([])
    print("base train: %d | mined windows: %d | copies: %d" % (len(d["Xtr"]), len(Xm), copies))
    if len(prov):
        from collections import Counter
        src = Counter(p.split(" ")[0].split("/")[0] for p in prov)
        print("mined provenance:")
        for k, v in src.most_common():
            print("   %-46s %3d" % (k, v))

    rep = np.tile(Xm, (copies, 1, 1)).astype(np.float32)
    ytr = np.concatenate([d["ytr"], np.ones(len(rep), np.int64)])   # class 1 = unknown
    Xtr = np.concatenate([d["Xtr"], rep])

    rng = np.random.default_rng(20260908)
    perm = rng.permutation(len(Xtr))
    d["Xtr"], d["ytr"] = Xtr[perm], ytr[perm]

    c = np.bincount(d["ytr"], minlength=3)
    print("")
    print("train after merge: %d  | keyword %d  unknown %d  silence %d"
          % (len(d["Xtr"]), c[0], c[1], c[2]))
    print("  (%d of the unknown examples are mined false positives)" % len(rep))
    np.savez_compressed(out, **d)
    print("saved %s (%.1f MB)" % (out, os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main()
