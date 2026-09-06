"""Dataset collection for the "Sentinel" keyword spotter.

Records a continuous stream from the ESP32 (EXP-007 firmware) and automatically
segments it into individual utterances, so the speaker can simply repeat the
word with pauses rather than following timed prompts.

Audio is saved RAW and UNFILTERED at 16 kHz mono 16-bit -- identical in
character to what the device feeds its own feature extractor at inference time.
Filtering is used ONLY to locate utterances, never to alter what is saved.

Usage:
    python tools/record_dataset.py <seconds> <label> [outdir]
    python tools/record_dataset.py <session.wav> <label> [outdir]   # re-segment offline

Example:
    python tools/record_dataset.py 60 sentinel dataset/positive
"""

import os
import sys
import time
import wave

import numpy as np
from scipy.signal import butter, sosfiltfilt

# pyserial is imported lazily inside open_stream(). The segmentation functions
# here are reused by build_continuous.py and eval_streaming.py, which run in the
# TensorFlow environment on D: where pyserial is not installed -- and which have
# no business requiring a serial library to slice a WAV file.

PORT = "COM5"
BAUD = 921600
FS = 16000

# --- Segmentation parameters -------------------------------------------------
#
# The detection band deliberately extends to 7500 Hz, not 3400 Hz. Pilot run 1
# (2026-09-06) used the 300-3400 Hz voiced band from EXP-005 and truncated the
# /s/ onset of "Sentinel" on 16 of 31 clips: a sibilant lives at 4-8 kHz and is
# invisible to a voiced-band detector, so every burst started late. Measured
# word length was 0.72-0.92 s against a 1.0 s clip, leaving a median of only
# 30 ms of leading margin.
BAND_LO, BAND_HI = 300.0, 7500.0
ENV_MS = 10              # envelope resolution
THRESH_HI = 4.0          # threshold to find the loud core of an utterance
THRESH_LO = 1.6          # hysteresis: expand outward to here, catching /s/ and /l/
MIN_BURST_S = 0.12       # shorter than this is a click, not a word
MAX_BURST_S = 1.40       # longer than this is merged utterances or noise, never
                         # one spoken word (pilot 2 measured a 0.90 s median)
MERGE_GAP_S = 0.18       # gaps smaller than this belong to one word
EDGE_GUARD_S = 0.05      # a burst touching the recording boundary is incomplete
CLIP_S = 1.5             # generous; training-time random cropping needs margin
MIN_MARGIN_S = 0.05      # QC: fail the run if a word sits closer than this to an edge

# QC plausibility band for a detected utterance. Session 6 (2026-09-06) produced
# 15 clips whose detected "words" were 0.12-0.25 s long -- fragments of syllables
# picked out of background noise -- and QC still reported PASS, because it only
# checked edge margins. Tiny detections trivially satisfy a margin test. Measured
# real utterances across five sessions span 0.57-1.47 s.
PLAUSIBLE_MIN_S = 0.45
PLAUSIBLE_MAX_S = 1.60

# Truncation is asymmetric between classes. A truncated POSITIVE corrupts its
# label -- it teaches the model that a partial sound is the keyword. A truncated
# NEGATIVE is still a valid negative: a syllable fragment genuinely is not the
# keyword, and fragments of speech are exactly what a deployed device hears. So
# negatives get a lower plausibility floor rather than being discarded.
PLAUSIBLE_MIN_NEG_S = 0.20
MIN_SNR = 6.0            # p99/p20 envelope ratio below this means segmentation
                         # cannot work; record clean speech and noise separately


def open_stream():
    """Reset the board and return a serial handle positioned at the raw stream."""
    import serial
    import serial.tools.list_ports
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
    """Read `seconds` of raw int16 from an open stream."""
    need = int(FS * seconds) * 2
    buf = bytearray(leftover)
    t0 = time.time()
    while len(buf) < need and time.time() - t0 < seconds + 30:
        d = ser.read(65536)
        if d:
            buf += d
    return np.frombuffer(bytes(buf[:need]), dtype="<i2").astype(np.float64)


def find_bursts(x):
    """Locate utterances. Returns (accepted, floor, env, t, rejected)."""
    sos = butter(2, [BAND_LO / (FS / 2), BAND_HI / (FS / 2)], btype="band", output="sos")
    band = sosfiltfilt(sos, x)

    w = int(ENV_MS / 1000 * FS)
    nb = len(band) // w
    env = np.sqrt((band[:nb * w].reshape(nb, w) ** 2).mean(axis=1))
    t = np.arange(nb) * (ENV_MS / 1000)

    floor = np.percentile(env, 20)          # robust to the words themselves
    hi = env > THRESH_HI * floor
    lo = env > THRESH_LO * floor

    # Find loud cores, then expand each outward while energy stays above the
    # lower threshold. Hysteresis is what recovers the quiet fricative onset and
    # the trailing consonant, which a single threshold cuts off.
    segs = []
    i = 0
    while i < nb:
        if hi[i]:
            j = i
            while j < nb and hi[j]:
                j += 1
            a, b = i, j - 1
            while a > 0 and lo[a - 1]:
                a -= 1
            while b < nb - 1 and lo[b + 1]:
                b += 1
            segs.append([t[a], t[b] + ENV_MS / 1000, env[i:j].max()])
            i = j
        else:
            i += 1

    merged = []
    for s in segs:
        if merged and s[0] - merged[-1][1] < MERGE_GAP_S:
            merged[-1][1] = s[1]
            merged[-1][2] = max(merged[-1][2], s[2])
        else:
            merged.append(s)

    dur_s = len(x) / FS
    good, rejected = [], []
    for s in merged:
        d = s[1] - s[0]
        if d < MIN_BURST_S:
            rejected.append((s, "too short (%.2f s)" % d))
        elif d > MAX_BURST_S:
            rejected.append((s, "too long (%.2f s) - merged utterances or noise" % d))
        elif s[0] < EDGE_GUARD_S or s[1] > dur_s - EDGE_GUARD_S:
            rejected.append((s, "touches the recording boundary - incomplete"))
        else:
            good.append(s)
    return good, floor, env, t, rejected


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


def write_wav(path, x):
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(FS)
    w.writeframes(np.clip(x, -32768, 32767).astype("<i2").tobytes())
    w.close()


def finish(x, label, outdir):
    segs, floor, env, t, rejected = find_bursts(x)

    # Refuse to segment audio whose SNR makes segmentation meaningless, rather
    # than emitting confident-looking fragments.
    snr = np.percentile(env, 99) / max(np.percentile(env, 20), 1e-9)
    print(">>> envelope dynamic range (p99/p20) = %.1fx" % snr)
    if snr < MIN_SNR:
        print("")
        print("!!! ABORTED: dynamic range %.1fx is below the %.1fx minimum." % (snr, MIN_SNR))
        print("!!! The background is too loud relative to speech for energy-based")
        print("!!! segmentation to find word boundaries. Record clean speech and")
        print("!!! background noise SEPARATELY, then mix at chosen SNRs instead.")
        print("!!! The raw session is saved and can be re-segmented later.")
        return 1

    print(">>> noise floor (band-limited envelope) = %.1f" % floor)
    print(">>> accepted %d utterances, rejected %d" % (len(segs), len(rejected)))
    for s, why in rejected:
        print("    rejected %6.2f-%6.2f s : %s" % (s[0], s[1], why))
    print("")

    existing = len([f for f in os.listdir(outdir) if f.endswith(".wav")])
    print("  #   start    end   dur     peak   x floor   lead   tail   file")
    margins, kept = [], 0
    for k, (a, b, pk) in enumerate(segs):
        centre = (a + b) / 2
        name = "%s_%04d.wav" % (label, existing + kept)
        save_clip(os.path.join(outdir, name), x, centre)
        lead = CLIP_S / 2 - (centre - a)
        tail = CLIP_S / 2 - (b - centre)
        margins.append((lead, tail))
        print("  %-3d %6.2f %6.2f %5.2f %8.0f %8.1fx %6.3f %6.3f   %s"
              % (k, a, b, b - a, pk, pk / max(floor, 1e-9), lead, tail, name))
        kept += 1

    clipped = int(np.sum(np.abs(x) >= 32767))
    print("")
    print(">>> saved %d clips of %.1f s to %s" % (kept, CLIP_S, outdir))
    print(">>> capture peak %.0f / 32767 | clipped samples %d" % (np.abs(x).max(), clipped))
    if clipped:
        print(">>> WARNING: clipping detected -- move further from the microphone")

    # Built-in QC. Pilot 1 shipped truncated clips because nothing checked this
    # automatically; the tool now refuses to call a run good without verifying.
    if margins:
        m = np.array(margins)
        durs = np.array([b - a for a, b, _ in segs])
        tight = int(np.sum(m.min(axis=1) < MIN_MARGIN_S))
        print("")
        print("=== QC ===")
        print("word length : min %.2f  median %.2f  max %.2f s"
              % (durs.min(), np.median(durs), durs.max()))
        print("lead margin : min %.3f  median %.3f s" % (m[:, 0].min(), np.median(m[:, 0])))
        print("tail margin : min %.3f  median %.3f s" % (m[:, 1].min(), np.median(m[:, 1])))
        # Plausibility, not just margins. A margin test alone passes happily on
        # 0.13 s noise fragments, which is exactly how session 6 slipped through.
        lo_bound = PLAUSIBLE_MIN_NEG_S if "neg" in label.lower() else PLAUSIBLE_MIN_S
        implausible = int(np.sum((durs < lo_bound) | (durs > PLAUSIBLE_MAX_S)))
        fails = []
        if tight:
            fails.append("%d/%d clips under %.0f ms margin" % (tight, kept, MIN_MARGIN_S * 1000))
        if implausible:
            fails.append("%d/%d detections outside the plausible %.2f-%.2f s word length"
                         % (implausible, kept, lo_bound, PLAUSIBLE_MAX_S))
        if np.median(durs) < lo_bound:
            fails.append("median detection %.2f s is not a word" % np.median(durs))
        if fails:
            print("QC: FAIL -- " + "; ".join(fails))
        else:
            print("QC: PASS -- margins >= %.0f ms and all detections are plausible words"
                  % (MIN_MARGIN_S * 1000))
    return 0


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    label = sys.argv[2]
    outdir = sys.argv[3] if len(sys.argv) > 3 else os.path.join("dataset", label)
    os.makedirs(outdir, exist_ok=True)

    # A .wav as the first argument re-segments an existing session offline
    # instead of recording. A segmentation bug must never cost another session.
    if sys.argv[1].lower().endswith(".wav"):
        w = wave.open(sys.argv[1], "rb")
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
        w.close()
        print(">>> re-segmenting %s (%.2f s), not recording" % (sys.argv[1], len(x) / FS))
        return finish(x, label, outdir)

    seconds = float(sys.argv[1])
    ser, leftover = open_stream()
    print(">>> recording %.0f s -- repeat the word with about 1 s between each" % seconds)
    x = record(ser, leftover, seconds)
    ser.close()
    print(">>> captured %d samples (%.2f s)" % (len(x), len(x) / FS))

    # Always keep the raw session. Pilot 1 had to be re-recorded purely because
    # only segmented clips were saved, so a segmentation bug could not be fixed
    # offline. Never again.
    sess_dir = os.path.join(outdir, "_sessions")
    os.makedirs(sess_dir, exist_ok=True)
    sess = os.path.join(sess_dir,
                        "%s_session_%s.wav" % (label, time.strftime("%Y%m%d_%H%M%S")))
    write_wav(sess, x)
    print(">>> raw session saved: %s" % sess)

    return finish(x, label, outdir)


if __name__ == "__main__":
    main()
