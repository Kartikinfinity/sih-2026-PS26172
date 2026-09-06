"""Dataset collection for the "Sentinel" keyword spotter.

Records a continuous stream from the ESP32 (EXP-007 firmware) and automatically
segments it into individual utterances, so the speaker can simply repeat the
word with pauses rather than following timed prompts.

Segmentation reuses the band-limited (300-3400 Hz) energy detector validated in
EXP-005, which caught all seven spoken words at >= 16x the noise floor.

Audio is saved RAW and UNFILTERED at 16 kHz mono 16-bit -- identical in
character to what the device feeds its own feature extractor at inference time.
The bandpass is used ONLY to locate utterances, never to alter what is saved.

Usage:
    python tools/record_dataset.py <seconds> <label> [outdir]

Example:
    python tools/record_dataset.py 60 sentinel dataset/positive
"""

import os
import sys
import time
import wave

import numpy as np
import serial
import serial.tools.list_ports
from scipy.signal import butter, sosfiltfilt

PORT = "COM5"
BAUD = 921600
FS = 16000

# Segmentation parameters
BAND_LO, BAND_HI = 300.0, 3400.0
ENV_MS = 25              # envelope resolution
THRESH_MULT = 4.0        # burst threshold, as a multiple of the noise floor
MIN_BURST_S = 0.12       # shorter than this is a click, not a word
MERGE_GAP_S = 0.18       # gaps smaller than this belong to one word
CLIP_S = 1.0             # saved clip length, centred on each burst


def open_stream():
    """Reset the board and return a serial handle positioned at the raw stream."""
    s = serial.Serial(PORT, BAUD, timeout=0.1)
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.12)
    s.setRTS(False)
    try:
        s.close()
    except Exception:
        pass

    t0 = time.time()
    ser = None
    while time.time() - t0 < 15:
        if any(p.device == PORT for p in serial.tools.list_ports.comports()):
            try:
                ser = serial.Serial(PORT, BAUD, timeout=2.0)
                break
            except Exception:
                pass
        time.sleep(0.01)
    if ser is None:
        sys.exit("[ERROR] serial port never came back after reset")

    buf = b""
    deadline = time.time() + 30
    while time.time() < deadline:
        d = ser.read(4096)
        if d:
            buf += d
        i = buf.find(b"BEGIN_STREAM")
        if i != -1:
            j = buf.find(b"\n", i)
            if j != -1:
                print(">>>", buf[i:j].decode("ascii", "replace").strip())
                return ser, buf[j + 1:]
    sys.exit("[ERROR] never saw BEGIN_STREAM")


def record(ser, leftover, seconds):
    need = int(FS * seconds) * 2
    buf = bytearray(leftover)
    t0 = time.time()
    while len(buf) < need and time.time() - t0 < seconds + 30:
        d = ser.read(65536)
        if d:
            buf += d
    return np.frombuffer(bytes(buf[:need]), dtype="<i2").astype(np.float64)


def find_bursts(x):
    """Locate utterances via band-limited energy. Returns (segments, floor, env, t)."""
    sos = butter(2, [BAND_LO / (FS / 2), BAND_HI / (FS / 2)], btype="band", output="sos")
    band = sosfiltfilt(sos, x)

    w = int(ENV_MS / 1000 * FS)
    nb = len(band) // w
    env = np.sqrt((band[:nb * w].reshape(nb, w) ** 2).mean(axis=1))
    t = np.arange(nb) * (ENV_MS / 1000)

    floor = np.median(env)
    loud = env > THRESH_MULT * floor

    segs = []
    i = 0
    while i < nb:
        if loud[i]:
            j = i
            while j < nb and loud[j]:
                j += 1
            segs.append([t[i], t[j - 1] + ENV_MS / 1000, env[i:j].max()])
            i = j
        else:
            i += 1

    # merge bursts separated by less than MERGE_GAP_S (one word, two syllable groups)
    merged = []
    for s in segs:
        if merged and s[0] - merged[-1][1] < MERGE_GAP_S:
            merged[-1][1] = s[1]
            merged[-1][2] = max(merged[-1][2], s[2])
        else:
            merged.append(s)

    return [s for s in merged if (s[1] - s[0]) >= MIN_BURST_S], floor, env, t


def save_clip(path, x, centre_s):
    half = int(CLIP_S * FS / 2)
    c = int(centre_s * FS)
    lo, hi = c - half, c + half
    clip = np.zeros(2 * half, dtype=np.float64)
    a, b = max(lo, 0), min(hi, len(x))
    clip[a - lo:b - lo] = x[a:b]
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(FS)
    w.writeframes(np.clip(clip, -32768, 32767).astype("<i2").tobytes())
    w.close()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    seconds = float(sys.argv[1])
    label = sys.argv[2]
    outdir = sys.argv[3] if len(sys.argv) > 3 else os.path.join("dataset", label)
    os.makedirs(outdir, exist_ok=True)

    ser, leftover = open_stream()
    print(">>> recording %.0f s -- repeat the word with about 1 s between each" % seconds)
    x = record(ser, leftover, seconds)
    ser.close()
    print(">>> captured %d samples (%.2f s)" % (len(x), len(x) / FS))

    segs, floor, env, t = find_bursts(x)
    print(">>> noise floor (band-limited envelope) = %.1f" % floor)
    print(">>> detected %d utterances\n" % len(segs))

    existing = len([f for f in os.listdir(outdir) if f.endswith(".wav")])
    print("  #   start    end   dur     peak   x floor   file")
    kept = 0
    for k, (a, b, pk) in enumerate(segs):
        centre = (a + b) / 2
        name = "%s_%04d.wav" % (label, existing + kept)
        save_clip(os.path.join(outdir, name), x, centre)
        print("  %-3d %6.2f %6.2f %5.2f %8.0f %8.1fx   %s"
              % (k, a, b, b - a, pk, pk / max(floor, 1e-9), name))
        kept += 1

    clipped = int(np.sum(np.abs(x) >= 32767))
    print("\n>>> saved %d clips of %.1f s to %s" % (kept, CLIP_S, outdir))
    print(">>> capture peak %.0f / 32767 | clipped samples %d" % (np.abs(x).max(), clipped))
    if clipped:
        print(">>> WARNING: clipping detected -- move further from the microphone")
    np.save(os.path.join(outdir, "_last_env.npy"), np.vstack([t, env]))


if __name__ == "__main__":
    main()
