"""Record background noise (no speech) for mixing augmentation.

Session 6 of positive collection (2026-09-06) tried to record speech IN noise and
segment it. That is self-defeating: energy-based segmentation needs SNR, and the
point of the session was to remove SNR. Measured dynamic range collapsed from
45.4x (quiet room) to 3.9x, and only 15 syllable fragments were recovered from
90 seconds.

The correct method is to record clean speech and background noise SEPARATELY,
then mix them at chosen SNRs during training. That yields unlimited noisy
examples at SNRs we control, instead of a handful at whatever SNR happened to
occur.

This script records continuous audio and saves it whole -- no segmentation, no
QC on word boundaries, because there are no words in it.

Usage:
    python tools/record_noise.py <seconds> <name> [outdir]

Example:
    python tools/record_noise.py 90 fan dataset/noise
"""

import os
import sys

import numpy as np

from record_dataset import FS, open_stream, record, write_wav


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    seconds = float(sys.argv[1])
    name = sys.argv[2]
    outdir = sys.argv[3] if len(sys.argv) > 3 else "dataset/noise"
    os.makedirs(outdir, exist_ok=True)

    ser, leftover = open_stream()
    print(">>> recording %.0f s of BACKGROUND ONLY -- do not speak" % seconds)
    x = record(ser, leftover, seconds)
    ser.close()

    path = os.path.join(outdir, "%s.wav" % name)
    write_wav(path, x)

    rms = np.sqrt((x ** 2).mean())
    peak = np.abs(x).max()
    clipped = int(np.sum(np.abs(x) >= 32767))
    print(">>> saved %s (%.1f s)" % (path, len(x) / FS))
    print(">>> rms %.0f | peak %.0f / 32767 | clipped %d" % (rms, peak, clipped))

    # A recording meant to contain no speech should have low dynamic range. A
    # large one means something transient got in -- a cough, a door, a word.
    k = int(0.01 * FS)
    nb = len(x) // k
    env = np.sqrt((x[:nb * k].reshape(nb, k) ** 2).mean(axis=1))
    dyn = np.percentile(env, 99) / max(np.percentile(env, 20), 1e-9)
    print(">>> envelope dynamic range (p99/p20) = %.1fx" % dyn)
    if clipped:
        print(">>> WARNING: clipping -- reduce the noise source level")
    if dyn > 8.0:
        print(">>> WARNING: high dynamic range for a noise recording. Something")
        print(">>>          transient may have been captured (speech, a door, a cough).")
    else:
        print(">>> OK: consistent with steady background noise")
    return 0


if __name__ == "__main__":
    main()
