"""Log-Mel feature extraction -- the host-side twin of the on-device pipeline.

This is deliberately a line-for-line mirror of computeFrame() in
docs/experiments/code/exp006_logmel.cpp. EXP-006 verified the two against each
other on 48,000 real samples: max absolute difference 0.000427 log-Mel units,
mean 0.0000077, correlation 1.0000000000. The residual is float32-on-device vs
float64-on-host and nothing more.

That parity is load-bearing. If training-time and inference-time features drift
apart, the model does not crash -- it silently gets worse, and the cause is very
hard to find later. Any change to the constants or the maths below MUST be
mirrored in the firmware and re-verified.
"""

import numpy as np

FS = 16000
FRAME_LEN = 400          # 25 ms
FRAME_HOP = 320          # 20 ms
NFFT = 512
NBINS = NFFT // 2 + 1    # 257
N_MEL = 40               # filterbank size (internal)
N_MFCC = 13              # coefficients kept after the DCT -- the model input
MEL_LO = 125.0
MEL_HI = 7500.0
LOG_OFFSET = 1e-6

# EXP-011 rebuilt this stage. The previous 98 x 40 log-Mel input carried 8x the
# data of ARM's DS-CNN-S reference (49 x 10) and made on-device inference 22x
# over budget even with ESP-NN enabled. Two changes, both suggested by the plan
# itself ("13-40 MFCC coefficients x 40-50 frames"):
#   hop 10 ms -> 20 ms   : 98 -> 49 frames
#   40 log-Mel -> 13 MFCC: a DCT decorrelates the bands and keeps the low
#                          cepstral coefficients that carry phonetic identity
# Combined input 49 x 13 = 637 vs 3,920, a 6.2x reduction.
#
# 1.0 s of audio: (16000 - 400) / 320 + 1 = 49 frames.
WINDOW_S = 1.0
WINDOW_SAMPLES = int(WINDOW_S * FS)
N_FRAMES = (WINDOW_SAMPLES - FRAME_LEN) // FRAME_HOP + 1


def _hz_to_mel(f):
    return 2595.0 * np.log10(1.0 + f / 700.0)


def _mel_to_hz(m):
    return 700.0 * (10.0 ** (m / 2595.0) - 1.0)


def _build_window():
    # Periodic Hann (divide by N, not N-1) -- matches the firmware.
    return 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(FRAME_LEN) / FRAME_LEN)


def _build_melbank():
    edges = _mel_to_hz(np.linspace(_hz_to_mel(MEL_LO), _hz_to_mel(MEL_HI), N_MEL + 2))
    bin_hz = FS / NFFT
    f = np.arange(NBINS) * bin_hz
    fb = np.zeros((N_MEL, NBINS))
    for m in range(N_MEL):
        lo, mid, hi = edges[m], edges[m + 1], edges[m + 2]
        left = (f >= lo) & (f <= mid)
        fb[m, left] = (f[left] - lo) / (mid - lo)
        right = (f > mid) & (f <= hi)
        fb[m, right] = (hi - f[right]) / (hi - mid)
    return fb


def _build_dct():
    """Orthonormal DCT-II, N_MEL -> N_MFCC. Standard MFCC definition."""
    d = np.zeros((N_MFCC, N_MEL))
    for k in range(N_MFCC):
        for n in range(N_MEL):
            d[k, n] = np.cos(np.pi * k * (2 * n + 1) / (2 * N_MEL))
    d[0, :] *= np.sqrt(1.0 / N_MEL)
    d[1:, :] *= np.sqrt(2.0 / N_MEL)
    return d


_WINDOW = _build_window()
_MELBANK = _build_melbank()
_DCT = _build_dct()


def log_mel(x, n_frames=None, hop=None):
    """int16-scale audio -> (n_frames, N_MEL) float32 log-Mel features."""
    x = np.asarray(x, dtype=np.float64)
    hop = FRAME_HOP if hop is None else hop
    if n_frames is None:
        n_frames = (len(x) - FRAME_LEN) // hop + 1
    out = np.empty((n_frames, N_MEL), dtype=np.float32)
    for i in range(n_frames):
        frame = x[i * hop:i * hop + FRAME_LEN] * _WINDOW
        spec = np.fft.rfft(frame, NFFT)
        power = spec.real ** 2 + spec.imag ** 2
        out[i] = np.log(_MELBANK @ power + LOG_OFFSET)
    return out


def mfcc(x, n_frames=None):
    """int16-scale audio -> (n_frames, N_MFCC) float32 MFCC features.

    log-Mel then an orthonormal DCT-II, keeping the first N_MFCC coefficients.
    The DCT decorrelates the strongly-overlapping Mel bands, so the low
    coefficients concentrate the spectral-envelope shape that distinguishes one
    word from another, and the high ones (mostly fine pitch detail) are dropped.
    """
    lm = log_mel(x, n_frames=n_frames)
    return (lm @ _DCT.T).astype(np.float32)


def inband_rms(x, lo_hz=MEL_LO, hi_hz=MEL_HI):
    """RMS restricted to the band the Mel filterbank actually sees.

    Broadband SNR is misleading for this microphone: EXP-003 measured 64.6 % of
    ambient energy below 100 Hz, and the recorded background noise was 86.9 %
    below 100 Hz. Since the filterbank starts at 125 Hz, most of that energy
    never reaches the model. Mixing to a broadband SNR target would produce
    training examples that look difficult on paper and are nearly clean in the
    feature domain.
    """
    x = np.asarray(x, dtype=np.float64)
    n = len(x)
    spec = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1.0 / FS)
    keep = (f >= lo_hz) & (f <= hi_hz)
    # Parseval: energy in the retained bins, normalised back to a per-sample RMS.
    energy = 2.0 * np.sum(np.abs(spec[keep]) ** 2) / (n * n)
    return float(np.sqrt(max(energy, 0.0)))


def mix_at_snr(speech, noise, snr_db, rng):
    """Mix noise into speech at a target IN-BAND SNR, returning int16-scale audio."""
    if len(noise) < len(speech):
        reps = int(np.ceil(len(speech) / len(noise)))
        noise = np.tile(noise, reps)
    start = rng.integers(0, len(noise) - len(speech) + 1)
    seg = noise[start:start + len(speech)].astype(np.float64)

    s_rms = inband_rms(speech)
    n_rms = inband_rms(seg)
    if n_rms < 1e-9 or s_rms < 1e-9:
        return speech
    target_n = s_rms / (10.0 ** (snr_db / 20.0))
    return speech + seg * (target_n / n_rms)
