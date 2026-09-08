# Sentinel — Technical Audit Report

**Date:** 2026-09-08 · **Commit:** `81b4dfc` · **Experiments:** 19 · **Commits:** 29
**Scope:** exact specifications, all ML work performed, measured results, conclusion.

Every number below is a measurement with a named source. Where something was not
measured, this report says so rather than estimating.

---

## 1. VERDICT — read this first

| Question | Answer |
|---|---|
| Does it run on the ESP32-S3? | **Yes** — 84.17 ms/inference, 48 % of one core, 15,460-byte arena |
| Does it detect the keyword? | **Yes** — 11 firings from 10 spoken keywords, on hardware |
| Is the false-activation rate acceptable? | **Not proven.** 7.42/min measured at a validated operating point |
| Is the best operating point validated? | **NO.** 0.90/3-of-3 shows 11/11 + 0.00 false/min, but that was swept on the same 80 s it would be judged on. **Fresh validation was not run** |
| Is it speaker-independent? | **No, and never tested.** One speaker |
| Is it deployable today? | **As a demonstration, yes. As a product, no** |

**The single most important caveat:** the headline result (100 % detection, zero false
activations) is a *tuned* figure, not a *validated* one. It requires two 40-second runs to
confirm or refute. Until then it must be quoted as provisional.

---

## 2. EXACT SPECIFICATIONS

### 2.1 Hardware

| Item | Value | Source |
|---|---|---|
| MCU | ESP32-S3 (QFN56) rev v0.2, dual LX7 @ 240 MHz | EXP-000, on-device readout |
| Flash | 16 MB, quad-SPI @ 3.3 V (eFuse) | EXP-000 |
| PSRAM | 8 MB octal | EXP-000 |
| Module | **ESP32-S3-WROOM-1-N16R8** (not WROOM-2) | eFuse: quad flash @3.3 V |
| Microphone | INMP441 MEMS I2S, mono, left channel | EXP-001 |
| Wiring | SCK→GPIO 6, WS→GPIO 5, SD→GPIO 4, VDD→3V3, GND→GND, L/R→GND | EXP-001, verified against all reserved GPIO ranges |
| Link | Native USB-Serial/JTAG, `303A:1001`, COM5 @ 921600 | EXP-000 |

### 2.2 Audio front end

| Parameter | Value |
|---|---|
| Sample rate | **16,001.60 Hz measured** (+0.010 % vs 16 kHz configured) — EXP-003 |
| Bit depth | 24-bit in 32-bit I2S slot → stored as 16-bit signed |
| Channels | 1 (left) |
| Frame length | 400 samples (25 ms) |
| Frame hop | 320 samples (20 ms) |
| FFT | 512-point, periodic Hann window |
| Mel filterbank | 40 triangular filters, 125–7500 Hz |
| Output features | **13 MFCC** (orthonormal DCT-II of log-Mel) |
| Model input | **49 frames × 13 MFCC = 637 values**, 1.0 s context |
| Normalisation | Per-coefficient (13 means, 13 std), generated into `src/kws_params.h` |

**Host/device parity verified twice:**

| Pipeline | Max abs diff | Correlation | Source |
|---|---|---|---|
| log-Mel 98×40 | 0.000427 | 1.0000000000 | EXP-006 |
| MFCC 49×13 | **0.000112** | **1.0000000000** | EXP-013 |

### 2.3 Model — currently deployed

| Property | Value |
|---|---|
| Directory | `model_mined/` |
| Architecture | DS-CNN: Conv2D(32, 10×4, stride 2×2) → 4× depthwise-separable blocks (one with stride 2) → GlobalAvgPool → Dropout(0.3) → Dense(3) |
| **Parameters** | **7,779** (31 layers) |
| Quantisation | Full-integer int8 (weights, activations, input, output) |
| **Model size** | **24,536 bytes** |
| Input quantisation | scale 0.07823640, zero-point 35 |
| Output quantisation | scale 0.05696274, zero-point −12 |
| Classes | keyword / unknown / silence |
| Runtime | TensorFlow Lite Micro + **ESP-NN** (`nickjgniklu/ESP_TF@2.0.1`) |

### 2.4 On-device performance

| Metric | Measured |
|---|---|
| **Inference latency** | **84.17 ms** |
| Feature extraction | 1.11 ms/frame (50 frames/s) |
| Inference cadence | every 200 ms |
| **CPU load** | **48.0 % of one core** (features 5.9 %, inference 42.1 %) |
| **Tensor arena** | **15,460 bytes** (internal SRAM) |
| Firmware RAM | 72,924 B = 22.3 % of 327,680 |
| Firmware flash | 374,653 B = 5.7 % of 6,553,600 |
| Free heap | 249,144 B |

### 2.5 Detection logic (deployed)

```
threshold 0.90 | 3-of-3 vote | 6-window (1.2 s) refractory
```

---

## 3. DATASET

### 3.1 Our recordings

| Class | Count | Detail |
|---|---|---|
| Positives ("Sentinel") | **199 clips** | 6 sessions: baseline, close ~10 cm, far ~1 m, soft, fast/casual, day-2 baseline |
| Hard negatives | **105 clips** | *sentimental, essential, central, signal, single, sending, seven, sensor, sentence, settle, censor, cinnamon, centre, certain, standard, special* |
| Raw sessions (uncut) | 9 files, **13.0 min** | the source material — every clip is cut from these |
| Background noise | 7 files, **8.2 min** | our fan + 6 Speech Commands backgrounds |
| Rejected (auditable) | 23 clips | 7 partial, 1 degenerate, 15 low-SNR |
| **Speakers** | **1** | **the hard limit of this project** |

### 3.2 Public data

| Dataset | Used | Detail |
|---|---|---|
| Google Speech Commands v0.02 | Yes | 105,829 clips, 35 words, **2,618 unique speakers**, 1 s / 16 kHz / mono — identical format, no resampling |
| — imported as "unknown" | 3,850 train + 770 eval | rescaled to our RMS distribution and mixed with our room noise to suppress a channel shortcut |
| — background noise | 6 files, 399 s | dishes, miaowing, exercise bike, pink noise, running tap, white noise |
| Mozilla Common Voice | **No** | not downloaded; not required once mining showed Speech Commands is already solved (§4.6) |

### 3.3 Training set as deployed (`dataset/kws_mined.npz`)

| Split | Total | keyword | unknown | silence |
|---|---|---|---|---|
| train | 13,866 | 6,230 | 6,510 | 1,126 |
| val | 1,134 | 93 | 721 | 320 |
| test | 1,326 | 139 | 852 | 335 |

Includes 56 mined false positives × 20 copies = 1,120 windows, **train only**.

### 3.4 Split policy

**Temporal within each session**, not random: the last 20 % of each recording by time is
test, the 15 % before it validation. Utterances recorded seconds apart share room state,
mic placement and voice warmth; a random split scatters near-duplicates across train and
test and reports an accuracy that does not survive deployment.

**Limitation:** this reduces leakage, it does not eliminate it. With one speaker,
**speaker-independent splitting is impossible.** No accuracy figure in this project may be
described as speaker-independent.

### 3.5 Augmentation

| Technique | Range |
|---|---|
| Random crop | 1.0 s window from a 1.5 s clip |
| Gain jitter | ×0.6 – ×1.5 |
| Noise mixing | 5 / 10 / 15 / 20 / 25 dB, **in-band SNR (125–7500 Hz)** |
| Keyword oversampling | ×14 |
| Mined-negative oversampling | ×20 |

In-band SNR matters: 86.9 % of our recorded noise sits below 100 Hz where the filterbank
cannot see it, so mixing to a broadband target would produce examples that look punishing
on paper and are nearly clean in the feature domain.

**Not implemented:** room impulse responses / synthetic reverberation, microphone-response
simulation, codec artefacts, speed/pitch perturbation.

---

## 4. ALL ML WORK PERFORMED

Six models trained. Every one deployed to hardware except the first.

### 4.1 Model lineage and results

| # | Model | Training data | Test acc | Hardware detection | Hardware false/min |
|---|---|---|---|---|---|
| 1 | `model` | centred clips, 98×40 log-Mel | 0.9505 | not deployed | — |
| 2 | `model_mfcc` | centred clips, 49×13 MFCC | 0.8956 | 34.9 % of frames firing | — |
| 3 | `model_cont` | continuous windows | 0.6616 | **5/10** | **11.88** |
| 4 | `model_aug` | + Speech Commands | 0.6550 | **0–2/10** | **0.00** |
| 5 | `model_bal` | + keyword oversampling | 0.8245 | **8/10** | **7.43** |
| 6 | **`model_mined`** | + mined false positives | 0.7662 | **11 firings / 10 spoken** | **7.42** |

Rows 3–6 measured at fixed operating points on hardware with controlled tests where every
event has a known label (Test 1 = keyword only; Test 2 = keyword never spoken).

### 4.2 Latency optimisation

| Stage | Inference | Change |
|---|---|---|
| Reference TFLM kernels (EXP-010) | 12,280 ms | baseline, 123× over budget |
| + ESP-NN enabled (EXP-011) | 2,240 ms | **5.48×** — two build flags |
| + MFCC 49×13, 32 channels (EXP-012) | **84.17 ms** | **26.6×**, total **146×** |

ESP-NN was bundled in the library but gated behind `#if ESP_NN` with no build flags
declared, so reference kernels were compiling. `-DESP_NN=1` and
`-DCONFIG_IDF_TARGET_ESP32S3=1` fixed it without a toolchain migration.

### 4.3 Quantisation cost — measured every time

| Model | float32 | int8 | Δ | agreement |
|---|---|---|---|---|
| `model` | 0.9505 | 0.9505 | +0.0000 | 98.90 % |
| `model_mfcc` | 0.8956 | 0.8984 | +0.0027 | 99.73 % |
| `model_cont` | 0.6616 | 0.6654 | +0.0038 | 97.59 % |
| `model_bal` | 0.8245 | 0.8254 | +0.0009 | 98.98 % |
| **`model_mined`** | **0.7662** | **0.7730** | **+0.0068** | **98.57 %** |

Quantisation has never cost accuracy in this project. It does shift the operating point:
for `model` the int8 version was measurably more trigger-happy (miss 0.0214→0.0071,
false-fire 0.0625→0.0759) with accuracy unchanged only because the shifts cancelled.

### 4.4 Decision-logic work

| Rule | Result |
|---|---|
| Single threshold | baseline |
| N-consecutive | brittle — one dip below threshold resets the counter and loses a confident detection |
| **M-of-N vote + refractory** | deployed; tolerates the dip, prevents double-firing |

Measured directly: with `model_bal`, strict 2-consecutive and 2-of-3 voting gave **identical**
5/10 on hardware. Decision-logic tuning moves along the frontier; it does not create
detection capability.

### 4.5 Threshold sweeps

`model_bal` Pareto frontier (fresh captures):

| threshold | rule | detection | false/min |
|---|---|---|---|
| 0.50 | 1-of-1 | 8/10 | 7.43 |
| 0.50 | 2-of-3 | 5/10 | 2.97 |
| 0.70 | 2-of-2 | 4/10 | 0.00 |

`model_mined` frontier (tuning captures — **not fresh**):

| threshold | rule | detection | false/min |
|---|---|---|---|
| **0.90** | **3-of-3** | **11/11** | **0.00** |

A single point dominating everything is a strong claim and is exactly why it needs fresh
validation.

### 4.6 Hard-negative mining (EXP-019)

Ran `model_bal` over every second of audio we possess at a mining threshold of 0.35,
recording windows where it fires when it should not.

| Source | Windows scanned | False positives | Rate |
|---|---|---|---|
| Speech Commands (35 words, 2,100 clips) | 2,100 | **3** | **0.14 %** |
| Our hard-negative sessions | 892 | 35 | 3.9 % |
| Our positive sessions (clear of utterances) | 211 | 18 | 8.5 % |
| **Total** | **3,203** | **56** | 1.75 % |

**The key finding of this audit.** Speech Commands is essentially solved — only *cat*,
*happy* and *off* fooled the model, one clip each. The remaining false positives come
almost entirely from **our own microphone and our own voice**. Different speakers on
different mics are easy to separate; the same voice on the same mic saying non-keyword
words is the hard case — and that is exactly what the live Test 2 is.

**Consequence:** more general public speech data will not help. That lever is spent.

---

## 5. WHAT IS VALIDATED, AND WHAT IS NOT

### 5.1 Validated (measured, reproducible, recorded)

| Item | Evidence |
|---|---|
| Hardware identity | eFuse + on-device readout |
| Wiring safety | checked against every reserved GPIO range from SoC headers |
| Sample rate | 16,001.60 Hz, frames counted against `esp_timer` |
| Bit alignment | low byte zero on all 80,000 sampled words |
| Zero clipping | every capture, every session |
| Audio correctness | confirmed by ear — natural pitch and speed |
| Host/device feature parity | 0.000112 max diff, correlation 1.0000000000 |
| Inference latency | 84.17 ms, repeated |
| Memory | arena 15,460 B; RAM 22.3 %; flash 5.7 % |
| Quantisation cost | measured on all five deployed models |
| Detection works | 11 firings from 10 spoken keywords |

### 5.2 NOT validated

| Gap | Status |
|---|---|
| **Deployed operating point (0.90/3-of-3)** | Swept on the same 80 s used to judge it. **Fresh runs not performed** |
| **False activations per hour** | The plan's §7 metric. Longest sample is **40 s**. Never measured |
| **Any other speaker** | One speaker. Speaker-independent splitting impossible |
| Detection latency | Never measured |
| Power consumption | Never measured |
| Long-run stability | Never measured |
| Far-field (>1 m) | Only one far session, and the Lombard effect meant level barely changed |
| Post-detection streaming / ASR | **No code exists** |
| Dual-core split | Single core |
| CPU target | 48 % measured vs plan's <10 % goal — **missed** |

### 5.3 Findings where the evaluation itself was wrong

Twice, and worth recording because they are the most transferable lessons here.

1. **EXP-014.** Every accuracy figure from EXP-008 onward was measured on curated,
   centred, complete words. The deployed model was firing on **49.7 %** of realistic
   sliding windows while its test set reported 10.7 %. *The evaluation was wrong before
   the model was.*
2. **EXP-016.** After fixing that, the held-out windows still came from the same narrow
   recording sessions as training. Measured false rate was optimistic by **~12×** against
   natural speech.

A third near-miss: **EXP-017** would have been reported as a success (false fires
11.88 → 0.00/min, prediction confirmed in advance). Fresh validation measured **0/10
detection** — the model had solved false fires by refusing to say "keyword" at all.

---

## 6. CONCLUSION

### 6.1 What was built

A complete, working on-device keyword spotter. Sound enters an INMP441, is captured over
I2S at a verified 16 kHz, converted to 49×13 MFCC features that provably match the host
pipeline to seven decimal places, classified by a 7,779-parameter int8 DS-CNN in 84 ms
using 15 KB of arena, and gated by an M-of-N vote with a refractory period. It needs no
network, no PC and no cloud at detection time.

### 6.2 Where it actually stands

**Best fully-validated configuration** (`model_bal`, fixed operating point, non-tuning
audio): **80 % detection at 7.43 false activations/min**, or **40 % detection at
0.00 false/min**.

**Best measured configuration** (`model_mined` at 0.70/2-of-2, non-tuning audio):
**11 firings from 10 spoken keywords at 7.42 false/min** — same false rate as `model_bal`
but with roughly complete detection instead of 80 %.

**Best tuned configuration, unvalidated** (`model_mined` at 0.90/3-of-3): 11/11 and
0.00 false/min. **Provisional.**

### 6.3 Why it is not better

Not architecture, not features, not quantisation, not decision logic — each was measured
and eliminated:

- Decision logic: two different smoothing rules gave *identical* hardware results.
- Negative data: mining shows Speech Commands now contributes 0.14 % of false positives.
- Latency and memory: comfortably within budget.

**The binding constraint is the positive class.** 199 clips from ~180 utterances by
**one speaker, in essentially one room, on two days**. The negative class has 2,618
speakers. Oversampling copies the positives; it does not add information. Every measured
failure traces back to this asymmetry.

### 6.4 The honest one-line summary

> Sentinel detects its keyword reliably on an ESP32-S3 within a 24.5 KB int8 model at
> 84 ms per inference, with a fully-characterised accuracy/false-activation trade-off —
> but it is trained on a single voice, its best operating point is not yet validated on
> fresh audio, and its per-hour false-activation rate has never been measured.

### 6.5 Immediate outstanding action

**Two 40-second recordings** would convert the headline result from provisional to
validated:

1. "Sentinel" × 10, nothing else → detection rate at 0.90/3-of-3
2. 40 s of speech with the keyword never spoken → false rate at 0.90/3-of-3

Everything else in this report is already measured.
