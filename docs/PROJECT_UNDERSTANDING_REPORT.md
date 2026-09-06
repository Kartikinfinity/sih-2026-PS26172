# Project Understanding Report
### SIH 2026 · PS26172 — "Sentinel" Voice Activator on ESP32-S3 + INMP441

**Written:** 2026-09-06 · **Covers:** the current completed state only
**Source of truth:** the Git repository and the local project files, inspected directly.

---

## How to read this report

Every claim below carries one of four labels. They are not decoration — they are the
difference between "we built it" and "we talked about it".

| Label | Meaning |
|---|---|
| **[IMPLEMENTED]** | Code exists in the repo and runs |
| **[VALIDATED]** | Implemented **and** measured, with numbers recorded in `docs/experiments/` |
| **[ARTIFACT]** | A generated output file (a model, a WAV, a figure) — not source code |
| **[NOT IMPLEMENTED]** | Discussed or planned, but no code exists. Do not claim it |

If you remember something from the build process that is not in this report, assume it was
discussed and not built until you find the file.

---

# PART 0 — The one-page overview

**What we built:** a keyword spotter that runs entirely on an ESP32-S3 microcontroller. It
listens continuously through a digital microphone, and when it hears the word **"Sentinel"**
it raises a flag. No internet, no phone, no PC involved at detection time.

```
you speak  →  INMP441 mic  →  I2S  →  ESP32-S3  →  MFCC features  →  int8 neural net  →  "keyword / unknown / silence"
                                        └──────────── all of this runs ON the chip ────────────┘
```

| Layer | State | Key measured numbers |
|---|---|---|
| Hardware wiring | **[VALIDATED]** | GPIO 6/5/4, cross-checked against ESP32-S3 reserved pins |
| Audio capture | **[VALIDATED]** | 16,001.60 Hz measured (+0.010 % error), 0 clipped samples |
| Feature pipeline | **[VALIDATED]** | host vs device max difference 0.000112, correlation 1.0000000000 |
| Dataset | **[ARTIFACT]**, local only | 176 positives, 105 hard negatives, 90 s noise, 8 raw sessions |
| Trained model | **[ARTIFACT]** | DS-CNN, 7,779 parameters, int8, 24,536 bytes |
| On-device inference | **[VALIDATED]** | 84.17 ms per inference, 48 % of one core, 15,460-byte arena |
| Detection | **[VALIDATED]** | fires on the keyword on real hardware |
| **Accuracy** | **[VALIDATED — and poor]** | deployed model fires on **49.7 %** of non-keyword windows |
| Temporal smoothing | **[NOT IMPLEMENTED in firmware]** | measured in Python only: 61 % detection, 0 false/min |
| Wi-Fi / streaming / remote ASR | **[NOT IMPLEMENTED]** | no networking code exists anywhere |

**The honest summary:** every layer from microphone to neural network is built, measured and
working on the physical device. The system reliably *detects* the keyword. It is **not yet
accurate enough to use**, because it also fires on things that are not the keyword. We know
exactly why (dataset size, single speaker) because we measured it.

---

# PART 1 — Three things that differ from what you might remember

I inspected the repo rather than trusting our conversation. Three corrections:

### 1. The model on the device is the *worse* one

Two models were trained. The one **compiled into `src/`** is `model_mfcc` — the
centred-clip-trained model, which EXP-014 measured firing on **49.7 %** of non-keyword
windows.

The better-behaved model, `model_cont`, exists only as `model_cont/kws_float.keras` plus its
normalisation file. It was **never quantised and never flashed**. There is no
`model_cont/kws_int8.tflite`.

### 2. Temporal smoothing is not in the firmware

`grep -c "consec\|smooth" src/main.cpp` returns **0**. EXP-015's smoothing result (61 %
detection, 0 false activations/min) was produced by `tools/eval_streaming.py` running on the
PC. The ESP32 fires on a **single** window crossing threshold 0.90.

### 3. The firmware's own banner prints wrong values

`src/main.cpp` still carries an `EXP-010` header comment though it is the EXP-012 code, and
its startup banner prints `49 frames x 40 mel | inference every 100 ms`. The true values are
**13 MFCC** and **200 ms**. Cosmetic — the computed values are correct — but if you read the
serial output at a demo you will see numbers that are wrong.

---

# PART 2 — The complete pipeline, stage by stage

Follow one word from your mouth to the answer.

```
 ┌──────────┐   sound     ┌──────────┐  3 wires   ┌───────────────────────────────┐
 │  YOU     │ ─ pressure ─│ INMP441  │ ── I2S ──▶ │        ESP32-S3               │
 │ "Sentinel"│    waves    │   mic    │            │                               │
 └──────────┘             └──────────┘            │  ① I2S DMA → 16-bit samples   │
                                                  │  ② 25 ms frame, every 20 ms   │
                                                  │  ③ FFT → 40 Mel → DCT → 13    │
                                                  │  ④ 49 frames buffered (1 s)   │
                                                  │  ⑤ normalise + quantise int8  │
                                                  │  ⑥ DS-CNN, TFLite Micro       │
                                                  │  ⑦ 3 probabilities            │
                                                  │  ⑧ threshold 0.90 → fire      │
                                                  └───────────────┬───────────────┘
                                                                  │ USB serial (text)
                                                                  ▼
                                                            your PC monitor
```

**The PC is not in the detection loop.** It is used for *building* the system (recording the
dataset, training, flashing) and for *watching* results. Unplug it and the ESP32 keeps
detecting.

---

### Stage ① — Sound becomes numbers

| | |
|---|---|
| **What** | The INMP441 converts air-pressure vibrations into a stream of digital numbers |
| **In** | Sound waves |
| **Out** | 16,000 numbers per second, each a 16-bit signed integer |
| **Who** | The microphone chip itself, clocked by the ESP32 |
| **Why this way** | A digital microphone does its own conversion inside a shielded package. An analog mic would need the ESP32's ADC, and the analog wire between them would pick up noise from the board |
| **Without it** | Nothing. There is no signal |

**Term:** *Sample* → one measurement of air pressure at one instant. **In our project:** one
signed number roughly between −32,768 and +32,767, produced 16,000 times a second.

---

### Stage ② — Chopping into frames

| | |
|---|---|
| **What** | The continuous stream is cut into overlapping 25 ms slices |
| **In** | The rolling sample stream |
| **Out** | A 400-sample frame, produced every 320 samples (20 ms) |
| **Why** | Speech changes constantly. Over 25 ms a vowel is roughly steady, so its frequency content is meaningful. Over a whole second it is not |
| **Why overlap** | Frames step forward 20 ms but are 25 ms long, so they overlap by 5 ms. A word boundary landing exactly on a frame edge would otherwise be smeared |
| **Without it** | You would analyse a whole second as one lump and lose all time structure — you could not tell "Sentinel" from the same sounds in a different order |

---

### Stage ③ — Frame becomes a fingerprint (the feature pipeline)

This is the most important stage to understand. Four steps inside it:

**3a. Windowing.** The frame is multiplied by a *Hann window* — a bell shape that fades to
zero at both ends. Cutting a frame out with sharp edges creates artificial discontinuities,
and the next step interprets sharp edges as *energy at every frequency*, smearing a pure tone
across the whole spectrum. Fading the edges removes that lie.

**3b. FFT.** Converts 25 ms of *amplitude over time* into *energy per frequency* — from "how
did the pressure wiggle" to "how much 300 Hz, how much 3 kHz". We use 512 points, giving 257
frequency bins.

**3c. Mel filterbank.** 257 numbers is too many and wrongly shaped. Human hearing separates
200 Hz from 300 Hz easily but 5,000 Hz from 5,100 Hz barely. The **Mel scale** encodes that
curve. We group 257 bins into **40 triangular bands spaced evenly in Mel**, so low
frequencies get narrow bands and high frequencies wide ones. Then take the logarithm, because
loudness perception is logarithmic and a small network learns better from a compressed range.

*A free bonus:* the lowest band starts at **125 Hz**. EXP-003 measured that 64.6 % of this
microphone's ambient noise sits below 100 Hz. The feature design throws that away without
needing a separate filter.

**3d. DCT → 13 MFCC.** A Discrete Cosine Transform decorrelates the 40 heavily-overlapping
Mel bands and keeps the first **13 coefficients**. These carry the shape of the spectral
envelope — which is what distinguishes one word from another — and discard fine pitch detail.

| | |
|---|---|
| **In** | 400 audio samples |
| **Out** | **13 numbers** |
| **Who** | `computeFrameInto()` in `src/main.cpp` on the device; `tools/features.py` on the PC |
| **Why 13, not 40** | EXP-011 measured that a 98×40 input made inference 22× too slow. 49×13 is 6.2× smaller and matches ARM's reference KWS design |
| **Without it** | The model would have to learn from raw audio — far more data and far more compute than this chip has |

---

### Stage ④ — Building one second of context

Thirteen numbers describe 25 ms. A whole word takes about 0.9 s. So the device keeps a
**rolling buffer of 49 frames** (49 × 20 ms ≈ 1 second) — a small picture, 49 wide and 13
tall, that always shows the most recent second.

**This is what the model actually sees.** Not audio: a 49×13 grid.

---

### Stage ⑤ — Normalise and quantise

Two conversions, both of which must **exactly** match what happened during training:

1. **Per-coefficient normalisation.** Each of the 13 coefficients is centred and scaled by
   its own mean and standard deviation (`KWS_NORM_MEAN[]`, `KWS_NORM_STD[]` in
   `src/kws_params.h`). Coefficient c0 is log-energy with a mean around 100; c1–c12 sit near
   zero. Using one shared scale for all of them destroys the model — we proved this, see
   EXP-012.
2. **int8 quantisation.** Floating-point values become whole numbers in the range −128…127,
   using a scale and zero-point emitted by the converter.

**Why quantise:** int8 arithmetic is what the chip's optimised kernels are built for, and it
quarters the memory. **Without matching constants:** nothing crashes. The model just quietly
gets worse — which is the hardest kind of bug to find.

---

### Stage ⑥ — The neural network

The 49×13×1 int8 grid goes into a **DS-CNN** running under **TensorFlow Lite Micro**.
Detailed in Part 6.

**Out:** three numbers — the model's confidence that this second of audio was
`keyword`, `unknown`, or `silence`.

---

### Stage ⑦–⑧ — Decision

The three raw outputs are converted to probabilities summing to 1. If
`P(keyword) ≥ 0.90`, the firmware prints `*** SENTINEL ***`.

**[NOT IMPLEMENTED]** — anything after this. There is no relay, no Wi-Fi, no streaming, no
remote speech recognition. The plan PDF describes those; no code exists.

---

# PART 3 — The hardware

## 3.1 ESP32-S3 — what it is doing here

The ESP32-S3 is a **microcontroller**: a complete small computer on one chip — two 240 MHz
processor cores, memory, and hardware for talking to peripherals. Unlike a PC it runs exactly
one program, from the moment it powers on, forever.

**Its job in this project is all four of these at once:**

1. **Clock master** — it generates the timing signal that makes the microphone speak
2. **Data collector** — its I2S+DMA hardware pulls samples into memory without CPU help
3. **Signal processor** — it computes the MFCC features
4. **Inference engine** — it runs the neural network

**Why this chip:** the model plus its working memory needs about 40 KB of RAM and the maths
needs real speed. An Arduino Uno has 2 KB of RAM and no such instructions. The S3 has 512 KB
of internal SRAM plus 8 MB of external PSRAM, and vector instructions that ESP-NN uses.

**Measured on your board (EXP-000):**

| Property | Value | How we know |
|---|---|---|
| Chip | ESP32-S3 (QFN56) rev v0.2 | esptool + on-device `esp_chip_info()` |
| Flash | 16 MB, quad-SPI at 3.3 V | eFuse readout |
| PSRAM | 8 MB octal | `psramFound()` returned true |
| Module identity | **WROOM-1-N16R8** | eFuse says quad flash at 3.3 V, so **not** a WROOM-2 N16R8V |

That last row mattered: it decided a build setting (`qio_opi`) that would otherwise have
prevented the board from booting.

## 3.2 INMP441 — and why it is not a normal microphone

A cheap analog microphone outputs a **tiny wiggling voltage**. To use it you must amplify it
and convert it with an ADC — and every centimetre of wire between mic and chip is an antenna
for noise from the board's own power supply and digital switching.

The **INMP441 is a MEMS microphone with the converter built in.** Inside one metal package it
senses sound, amplifies it, and converts it to numbers. What leaves the package is already
digital — immune to the noise that would corrupt an analog signal.

**This is why the plan calls it "eliminating analogue noise and codec complexity", and it is
why our measured noise floor is as clean as it is.**

## 3.3 I2S — the three wires that matter

**Term:** *I2S* (Inter-IC Sound) → a small digital protocol for moving audio between chips.
**In our project:** the only language the INMP441 speaks.

Three signal wires. The direction matters:

| Wire | Full name | Direction | What it does |
|---|---|---|---|
| **SCK** | Serial Clock (BCLK) | ESP32 **→** mic | Ticks once per bit. **The mic has no clock of its own** |
| **WS** | Word Select (LRCLK) | ESP32 **→** mic | Says which channel is being read: low = left, high = right |
| **SD** | Serial Data | mic **→** ESP32 | The only wire carrying audio |

The consequence worth remembering: **the microphone is a slave.** It emits one bit each time
the ESP32 ticks SCK, and nothing at all otherwise. If your clock is wrong or absent, the mic
is not broken — it is simply silent. "No data" almost never means "dead microphone".

## 3.4 The actual wiring — verified against the repo

`docs/experiments/EXP-001-wiring-confirmation.md` and the constants at the top of
`src/main.cpp` agree:

```
INMP441            ESP32-S3
──────────────────────────────
SCK   ────────────  GPIO 6      bit clock   (ESP32 → mic)
WS    ────────────  GPIO 5      word select (ESP32 → mic)
SD    ────────────  GPIO 4      audio data  (mic → ESP32)
VDD   ────────────  3V3
GND   ────────────  GND
L/R   ────────────  GND         ← selects LEFT channel
```

**This matches the wiring in your question exactly.** No difference to report.

### Why *these* GPIO numbers

Not arbitrary, and not aesthetic. EXP-001 checked GPIO 4/5/6 against every range the chip
reserves, reading the numbers out of the SoC header files rather than from memory:

| Reserved for | GPIOs | Conflict? |
|---|---|---|
| Flash / PSRAM main SPI bus | 27–32 | No |
| Octal PSRAM extension (the "R8") | 33–37 | No |
| USB D− / D+ | 19, 20 | No |
| UART0 | 43, 44 | No |
| Strapping pins (read at boot) | 0, 3, 45, 46 | No |

GPIO 4/5/6 appear in none of them. They do double as ADC and touch inputs (`A3/A4/A5`,
`T4/T5/T6`), but those are *alternate functions*, not reservations — using them as digital
I2S pins is fine.

**Had we picked GPIO 33–37**, we would have fought the PSRAM bus, and the 8 MB of PSRAM this
project depends on would have failed in confusing ways.

### Why the other three wires

- **VDD → 3V3, never 5 V.** The INMP441 is a 1.8–3.3 V part. 5 V risks destroying it.
- **GND → GND.** Both chips need the same reference for "zero volts", or the digital levels
  mean nothing.
- **L/R → GND selects the LEFT channel.** This one has a direct software consequence: the
  firmware must be told to read the left slot (`I2S_CHANNEL_FMT_ONLY_LEFT`). If it read the
  right slot instead, the mic would be electrically perfect and return **a stream of zeros** —
  a failure that looks exactly like dead hardware.

---

# PART 4 — ESP32 ↔ PC communication

## 4.1 The physical link

One USB-C cable. Your board enumerates as **`VID:PID = 303A:1001`** — Espressif's own ID for
the S3's **built-in USB-Serial/JTAG** peripheral. That is *not* a separate USB-to-serial chip;
it is inside the ESP32-S3 itself.

Windows presents it as **COM5**. Baud rate **921600**.

### The trap this created, and why it is worth knowing

Early on, firmware uploaded successfully and the board ran — and emitted **absolutely
nothing**. The cause: Arduino's `Serial` object defaults to **UART0**, which lives on GPIO 43
and 44 — different physical pins from the native USB you are plugged into.

The fix is one build flag, `-DARDUINO_USB_CDC_ON_BOOT=1`, which rebinds `Serial` to the USB
interface. It is in `platformio.ini` today.

**The lesson generalises:** a successful flash plus silence is not evidence of failure, and a
successful flash plus output is not evidence of correct behaviour.

## 4.2 What actually travels over the cable

Three different things at three different times:

| Purpose | Direction | Format |
|---|---|---|
| Flashing firmware | PC → ESP32 | esptool protocol, binary |
| Detection results (today) | ESP32 → PC | plain text lines |
| Dataset recording | ESP32 → PC | raw binary audio |

**During normal detection**, the ESP32 sends only human-readable text:

```
kw 0.996  unk 0.004  sil 0.000  -> keyword | feat 1.18 ms  infer 84.17 ms  cpu 48.0%   *** SENTINEL ***
```

**During dataset recording** [IMPLEMENTED: `docs/experiments/code/exp007_pcm_streamer.cpp`],
different firmware streams raw audio continuously: 16,000 samples/s × 2 bytes = **32,000
bytes per second**, well within the link's capacity.

## 4.3 The audio format, and why each value

| Parameter | Value | Why |
|---|---|---|
| Sample rate | 16,000 Hz | Speech energy lives below 8 kHz. 16 kHz captures it (sampling must exceed twice the highest frequency). 44.1 kHz would nearly triple the work for nothing |
| Bit depth on the wire | 32-bit slots | The chip's I2S moves fixed-width slots; the mic sends 24 real bits then zero padding |
| Bit depth stored | 16-bit signed | The top 16 bits of the 24, giving the standard PCM format every tool understands |
| Channels | 1 (mono, left) | One microphone. `L/R → GND` selects left |
| Format | **PCM**, little-endian | Raw numbers, no compression |

**Term:** *PCM* (Pulse Code Modulation) → the plainest possible way to store sound: a list of
numbers, one per sample. **In our project:** exactly what the mic produces and what the WAV
files contain.

**Term:** *WAV* → PCM plus a small header saying "16,000 Hz, 1 channel, 16-bit". **In our
project:** produced only on the PC, so humans and Python can play the audio. The ESP32 never
writes WAV files.

## 4.4 The measurement that made all of this trustworthy

We **measured** the true sample rate rather than trusting the configuration
(EXP-003), by counting frames the DMA actually delivered against the hardware timer:

```
frames = 80,384   elapsed = 5.0235 s
configured = 16000 Hz
MEASURED   = 16001.60 Hz   (error +0.010 %)
```

If this had been wrong, every filter, every Mel spectrogram and the entire trained model
would have been silently wrong, and nothing else we measured would have revealed it.

---

# PART 5 — Why there is no Arduino IDE anywhere in this project

## 5.1 What was actually used

| Tool | Role |
|---|---|
| **PlatformIO Core 6.1.19** | The build system. Command line only — no GUI |
| **espressif32 @ 7.1.1** | The platform package: compiler, board definitions, flashing tool |
| **Arduino framework (core 2.0.17)** | Still used! But as a *library*, not an IDE |
| **esptool.py** | Does the actual flashing over USB |
| **Python + pyserial** | All PC-side recording and analysis |
| **Git** | Version control |

**An important distinction:** we did not abandon *Arduino*. `src/main.cpp` has `setup()` and
`loop()` and calls `Serial.printf()`. What we abandoned is the **Arduino IDE** — the
graphical application. The Arduino *framework* is still the layer we program against.

## 5.2 What the Arduino IDE would normally do, and what replaced it

| Arduino IDE does | Our workflow does instead |
|---|---|
| Board chosen from a dropdown | `board = esp32-s3-devkitc-1` in `platformio.ini` |
| Port guessed from a menu | `upload_port = COM5`, verified with `pio device list` |
| Settings hidden in menus | Every setting is a line in `platformio.ini`, in Git |
| Libraries installed by clicking | `lib_deps = nickjgniklu/ESP_TF@^2.0.1` |
| Compile with a button | `pio run` |
| Upload with a button | `pio run -t upload` |
| Serial monitor window | `pio device monitor` or a Python script |

## 5.3 Why this mattered concretely — three cases

This is not a style preference. Three real problems in this project were only solvable in
this workflow:

**1. The board definition was wrong, and we could read it.** The stock
`esp32-s3-devkitc-1` definition describes an **N8 board with no PSRAM**. Our board is N16R8.
In the IDE you would pick a menu item and hope. Here we opened the JSON, saw
`"Espressif ESP32-S3-DevKitC-1-N8 (8 MB QD, No PSRAM)"`, and wrote explicit overrides:

```ini
board_upload.flash_size = 16MB
board_build.partitions = default_16MB.csv
board_build.arduino.memory_type = qio_opi
build_flags = -DBOARD_HAS_PSRAM
```

Without those, the 8 MB of PSRAM simply does not exist at runtime.

**2. ESP-NN was switched off, and we could switch it on.** The TFLite Micro library ships
Espressif's optimised kernels, but hides them behind `#if ESP_NN` and declares no build
flags. Two lines in `platformio.ini` made inference **5.48× faster** (EXP-011). There is no
IDE menu for that.

**3. Everything is reproducible.** `platformio.ini` is 40 lines in Git. Anyone cloning the
repo gets byte-identical build settings. IDE menu selections live in one person's
installation and cannot be reviewed or shared.

## 5.4 How flashing actually works

```
pio run            → compiles → .pio/build/.../firmware.bin
pio run -t upload  → esptool talks to the ROM bootloader over USB
                     writes 4 regions, verifies a SHA hash of each
                     resets the board
```

Verification is not by inspection — **esptool reads back a hash of every region** and prints
`Hash of data verified.` four times. That is how we know the upload was correct.

---

# PART 6 — The machine learning model

## 6.1 What it is

A **DS-CNN** — Depthwise-Separable Convolutional Neural Network. The exact architecture is in
`tools/train_kws.py`:

```
Input 49 × 13 × 1
   ↓ Conv2D(32 filters, 10×4 kernel, stride 2×2) + BatchNorm + ReLU
   ↓ DS block (32)
   ↓ DS block (32, stride 2)
   ↓ DS block (32)
   ↓ DS block (32)
   ↓ GlobalAveragePooling
   ↓ Dropout(0.3)
   ↓ Dense(3)
Output: 3 numbers
```

Each **DS block** = a 3×3 depthwise convolution (one filter per channel, looking for local
patterns) followed by a 1×1 convolution (mixing channels together).

**Why depthwise-separable:** a normal convolution does both jobs at once and costs roughly an
order of magnitude more multiplications. Splitting them keeps most of the accuracy at a
fraction of the compute — which is what lets this run on a microcontroller at all.

**Why a CNN for keyword spotting:** the 49×13 feature grid really is an image — time across,
frequency down — and the patterns that identify a word (a burst of high-frequency energy for
`/s/`, then formant bands for the vowels) are *local shapes* in that image. Convolutions are
built to find local shapes regardless of exactly where they sit.

## 6.2 Size

| | Value |
|---|---|
| Parameters | **7,779** |
| int8 model file | **24,536 bytes** |
| RAM needed to run it (arena) | **15,460 bytes** |

Small enough that the model is a C array compiled into the firmware (`src/kws_model.cc`) —
there is no file system and no SD card.

## 6.3 What the three outputs mean

| Class | Means |
|---|---|
| **keyword** | "This second contained the word Sentinel" |
| **unknown** | "This second contained speech, but not the keyword" |
| **silence** | "This second contained background, not speech" |

The raw outputs are converted to probabilities that sum to 1. `kw 0.996` means the model
assigns 99.6 % of its belief to keyword.

**What confidence is NOT:** it is not a probability that the model is right. It is how
strongly *this particular model, trained on this particular data*, matches the input against
what it was shown. A confidently wrong model is entirely possible — and EXP-014 measured
exactly that: the deployed model reports high confidence on windows that are not the keyword.

## 6.4 "The user said the keyword" vs "the model predicted the keyword"

These are different events, and the gap between them is the whole engineering problem.

| | |
|---|---|
| **You said it, model detected it** | True positive — the system works |
| **You said it, model missed it** | **Miss.** Device feels broken |
| **You did not say it, model fired** | **False fire.** Device is unusable |
| **You did not say it, model stayed quiet** | True negative |

The model has never heard the word "Sentinel" as a concept. It has seen 49×13 grids labelled
`keyword`, and learned what grids of that shape look like. **Anything else producing a
similar grid will also fire.** That is not a bug — it is what a statistical model is.

## 6.5 Decision logic on the device

| Mechanism | State |
|---|---|
| Threshold at 0.90 | **[IMPLEMENTED]** — `DETECT_THRESHOLD` in `src/main.cpp` |
| N-consecutive smoothing | **[NOT IMPLEMENTED in firmware]** — measured in Python only |
| Voting / debounce / hysteresis | **[NOT IMPLEMENTED]** |

**Term:** *Inference* → running a trained model on new input to get a prediction. **In our
project:** one `Invoke()` call on the TFLite Micro interpreter, taking **84.17 ms**, executed
every 200 ms.

---

# PART 7 — The dataset

## 7.1 The keyword, and why

**"Sentinel"**, English — recorded in `docs/DECISION-01-keyword.md`.

| Criterion | How Sentinel scores |
|---|---|
| Length | 3 syllables, ~0.9 s measured — about 45 feature frames of evidence |
| Phonetic variety | Four consonant classes: fricative `/s/`, plosive `/t/`, nasal `/n/`, liquid `/l/` |
| Rarity in ordinary speech | Essentially never said casually, and not a fragment of a common word |
| Fit to *our measured* hardware | see below |

The last one is the interesting one, and it came from our own measurement rather than
general advice. EXP-003 measured this microphone's ambient noise as **64.6 % below 100 Hz**
and only **0.02 % above 3400 Hz**. Our Mel filterbank reaches 7500 Hz. A sibilant `/s/` puts
its energy at roughly 4–8 kHz — **precisely where our measured noise floor is lowest**.

Words rejected: *"Computer"* (a common noun — anyone discussing computers triggers it),
*"Hey"/"Go"/"On"/"Start"* (too short, and fragments of other words).

## 7.2 What was recorded

**[ARTIFACT — local only, not on GitHub]**

| Class | Count | Recording conditions |
|---|---|---|
| Positives ("Sentinel") | **176** | baseline 24, close ~10 cm 42, far ~1 m 41, soft voice 34, fast/casual 35 |
| Hard negatives | **105** | *sentimental, essential, central, signal, single, sending, seven, sensor, sentence, settle, censor, cinnamon, centre, certain, standard, special* |
| Background noise | 90 s | one source |
| Raw sessions | **8 files** | the complete uncut recordings |
| Rejected (kept for audit) | 23 | 7 partial detections, 1 degenerate, 15 low-SNR |

**Why the conditions vary:** the model learns exactly what it is shown. Record only
close-and-quiet and it works only close-and-quiet. **Variation is not noise in the data — it
is the data.**

**Why hard negatives specifically:** a model trained only on "Sentinel" versus silence learns
*"someone spoke"* and fires on everything. Easy negatives ("elephant") teach almost nothing.
**Near-misses force the model to learn where the boundary actually is.** EXP-008 later
confirmed this was the right instinct: 100 % of its meaningful errors were on the
keyword/unknown boundary.

## 7.3 How it was collected

`tools/record_dataset.py` **[IMPLEMENTED]** records a continuous stream and **automatically
finds each utterance** using a band-limited energy detector, so you simply repeat the word
with pauses rather than following timed prompts.

Two design rules baked in after they were learned the hard way:

1. **Saved audio is raw and unfiltered.** Filtering is used only to *locate* utterances,
   never to alter what is saved — otherwise training audio would differ in character from
   what the device feeds its own feature extractor.
2. **The full uncut session is always saved.** This is why the entire dataset could later be
   rebuilt (EXP-014) without you recording anything again.

## 7.4 Augmentation

`tools/build_dataset.py` and `tools/build_continuous.py` **[IMPLEMENTED]**:

| Technique | What it simulates |
|---|---|
| Random crop of the 1.0 s window from a 1.5 s clip | The word arriving slightly early or late |
| Gain jitter (×0.5 to ×1.6) | Speaking louder or quieter |
| Noise mixing at 5/10/15/20/25 dB | A noisier room |

**Noise is mixed at *in-band* SNR** (125–7500 Hz), not broadband. Because 86.9 % of our
recorded noise sits below 100 Hz where the filterbank cannot see it, mixing to a broadband
target would produce examples that look punishing on paper and are nearly clean in the
feature domain — a self-deception dressed as rigour.

**Why augment at all:** 176 recordings is a small dataset. Augmentation multiplies *examples*
to roughly 2,000. It cannot manufacture *variety* — which is why the model still overfits.

## 7.5 The split — and why it is not random

Training data must be separated from test data, or you measure memorisation instead of
learning. **We split by time within each session**, not at random.

**Why:** utterances recorded seconds apart share room state, mic placement, voice warmth and
background. A random split scatters near-duplicates across train and test and reports an
accuracy that evaporates in the real world.

**Honest limitation:** this reduces leakage; it does not eliminate it. Every clip is still
one speaker, one room, one day.

---

# PART 8 — The experiments

Fifteen experiment records live in `docs/experiments/`. This is what each was *for*.

## EXP-000 — Is this board really what it claims?

**Question:** before writing a line of audio code, is the hardware what we think?
**Result:** ESP32-S3 rev v0.2, 16 MB flash, 8 MB PSRAM confirmed at runtime. eFuse reported
**quad-SPI flash at 3.3 V**, which identified the module as **WROOM-1-N16R8**, not WROOM-2.
**Decision:** that one fact set `memory_type = qio_opi`. The wrong choice would have
prevented booting. **PASS.**

## EXP-001 — Is the wiring safe?

**Question:** do GPIO 4/5/6 collide with anything the chip reserves?
**Method:** read the SoC header files, not memory. **Result:** clear of flash/PSRAM (27–37),
USB (19/20), UART0 (43/44) and strapping pins. **PASS.**
**Did not prove:** that the microphone works — only that the wiring is plausible.

## EXP-002 — First capture, and a withdrawn result

**Question:** does I2S produce real data?
**Result:** driver initialised, 126/126 blocks returned exactly 2048 bytes, and the 24-bit-in-
32-bit alignment was **proven from evidence** — every sampled raw word ended in `0x00`.

**Then it was downgraded to INCONCLUSIVE.** The 16.3× "response to sound" was recorded during
a capture in which no stimulus was performed. The number was real but unattributable.
**What this teaches:** a measurement without a controlled stimulus proves nothing, however
good the number looks.

## EXP-002B — The same test, done properly

**Result:** speech confirmed by user attribution. And a discovery: **the single loudest event
in the run was 100 % DC** — its block mean was 1,225,723 and its actual sound content was
ordinary. **Raw RMS was measuring drift, not audio.** The instrument was wrong.

## EXP-003 — Two questions that had been assumed

**Question 1:** is the sample rate really 16 kHz? **Measured: 16,001.60 Hz, +0.010 % error.**
Previously an assumption in every experiment; now a measurement.

**Question 2:** what is that low-frequency contamination?
**Result:** **64.6 % of energy below 100 Hz**, with peaks at **0.2–2.4 Hz** — *sub-audio
drift*, not DC offset and not mains hum. A 0.5 Hz wave inside a 32 ms window looks like a
constant offset, which is exactly why earlier block means swung wildly.
**Why it mattered:** it **exonerated the microphone.** The poor SNR was a wrong measurement
band, not weak hardware. **PASS.**

## EXP-004 — A prediction that failed, and was recorded as failed

**Prediction stated in advance:** speech ≥ 10× the filtered noise floor. **Result: 9.6×.**

An alternative floor estimator would have given 11.4× and "passed". **It was rejected** —
choosing the estimator after seeing the data is how a result gets faked.

**The failure was informative.** Band analysis showed why: during speech the 300–3400 Hz band
holds 43.8 % of energy versus 1.4 % ambient, while broadband RMS integrates over bands where
the signal is not. **Broadband RMS is the wrong instrument.**

Also produced the first listenable WAV, and caught a fake result: an apparent near-clipping
peak of 30,068 that turned out to be a truncated-transfer artifact at the cut boundary.

## EXP-005 — Measuring in the right band

**Question:** does measuring inside 300–3400 Hz fix it? **Result: 51.2× peak-to-floor**, all
seven spoken words above 16× the floor.

**But the honest part:** the wideband chain measured 9.6× in EXP-004 and **37.0× here on
unchanged code** — the speech was simply louder. Comparing 9.6× to 51.2× would be dishonest.
The within-run comparison, on identical audio, shows band-limiting bought **1.39×**.

**And the finding that mattered:** the gain is peak-only. p90 and mean ratios were unchanged.
**A scalar energy detector cannot tell a keyword from a door slam** — which is the
experimental justification for moving to real features.

## EXP-006 — Features, and the parity proof

**Question:** do the ESP32's features exactly match the PC's?
**Why it matters:** if training-time and inference-time features differ, the model does not
crash — it silently degrades, and the cause is very hard to find later.

**Result:** max absolute difference **0.000427** against a pre-declared 0.01 tolerance,
correlation **1.0000000000**. The residual is float32-on-device vs float64-on-host.
**Also measured:** 1.107 ms/frame = **11.07 % CPU**, above the plan's <10 % target — recorded
as missed rather than glossed.

## MILESTONE-01 — Audio capture declared validated

Fourteen separate checks closed, including the one that carries the most weight: **you
listened to the recording and confirmed it sounded natural in pitch and speed.**

**Why that mattered more than any number:** a wrong sample rate or channel misread would
still produce healthy-looking statistics — non-zero signal, sensible RMS, a stable floor.
Only a human ear catches it. Your confirmation corroborated the measured 16,001.60 Hz through
a completely different channel of evidence.

## EXP-008 — The first trained model

**Result:** test accuracy 0.9505 — **and immediately qualified.** Silence is 38 % of the test
set and was classified 140/140 perfectly. Excluding it: **0.9196**.

**Every one of the 18 errors was keyword ↔ unknown.** Silence was never confused with
anything. The model's only real weakness is telling "Sentinel" from *sentimental, essential,
central* — exactly the axis the hard negatives were recorded to probe.

**Decided:** the unknown class has 68 source clips and 100 % of the meaningful errors. That
became the *measured* justification for wanting more data — not a guess.

## EXP-009 — Quantisation

**Result:** 46.3 KB int8, accuracy delta **+0.0000**.

**But "no accuracy loss" would have been misleading.** The confusion matrices differ: the
int8 model is measurably **more trigger-happy** (miss 0.0214→0.0071, false-fire
0.0625→0.0759). Accuracy is unchanged only because the two shifts cancel. For a wake word,
that trade-off *is* the product.

## EXP-010 — It runs on the chip, 123× too slow

**Result:** TFLM compiles and runs on the existing toolchain — so deployment did *not* force
an ESP-IDF migration. Arena 132,004 B, features 1.11 ms/frame.

**Inference: 12,280 ms against a 100 ms budget.**

**Why that was diagnosed as anomalous rather than "unoptimised":** the model is ~21 M MACs.
At 240 MHz even a naive 1 MAC/cycle finishes in ~87 ms. 12,280 ms implies **~0.007 MACs per
cycle** — 140× below a scalar lower bound. Missing SIMD costs 4–8×, not 140×.

**And a result that was explicitly refused:** the log showed keyword probability rising to
0.997 with the detection message firing. **That was not counted as evidence.** At 12 s per
inference the audio pipeline stalls and the DMA overflows, so the audio-to-result timing
relationship is broken.

## EXP-011 — ESP-NN was present and switched off

**Found:** the library bundles Espressif's ESP-NN including S3 assembly, but gates it behind
`#if ESP_NN` and declares no build flags. Two flags: **12,280 ms → 2,240 ms, a 5.48×
speedup.** Inside the predicted 3–10× band, and — as also predicted — not sufficient alone.

**Also:** TFLM's own profiler is non-functional in this port (0 ticks for every operator), so
attribution came from arithmetic instead, labelled as such.

**Root cause identified:** the 98×40 input carries **8× the data** of ARM's reference
DS-CNN-S (49×10). No kernel optimisation fixes an input that large.

## EXP-012 — MFCC, and a training collapse

**Result: 84.17 ms — a 26.6× speedup, 146× from the original**, inside budget. Arena
132,004 → **15,460 B**. CPU **48.0 %** of one core.

**The training failure worth studying:** the first MFCC run collapsed to test accuracy
**0.3846 — exactly the silence fraction.** It predicted one class for everything.

Cause: normalisation inherited from the log-Mel model. A single global mean/std was fine
there because all 40 bands shared a scale. MFCC c0 is log-energy with mean 100.02 while
c1–c12 sit near zero. Under a global std of 26.6, **c2–c12 collapsed to std 0.04–0.08** —
eleven near-constant channels. The network kept the only signal left (overall energy) and
degenerated.

**Fixed** with per-coefficient normalisation, and firmware constants are now **generated**
into `src/kws_params.h` rather than transcribed — 26 hand-copied floats is precisely how a
silent mismatch enters.

**The cost, not hidden:** excluding silence 0.9196 → **0.8304**, false-fire 0.0625 →
**0.1473**. Latency was bought with accuracy.

## EXP-013 — The first valid live test, and the parity re-check

**Two things happened.**

**First**, EXP-011 had recorded that the parity proof *must* be re-run once features changed
— and it had not been. The live behaviour looked exactly like a feature mismatch, so parity
was checked **before** blaming the model. The parity firmware was **generated by transforming
the detector source**, not retyped, so the code path under test is the real one.

**Result: max abs diff 0.000112, correlation 1.0000000000. Parity holds.** Features were not
at fault.

**Second**, the live test: the keyword **is** detected on hardware, with probabilities
reaching 0.997–1.000, and silence handled correctly. **But 57 % of inferences predicted
keyword and 34.9 % would fire**, against 10.7 % measured offline.

**Decision:** smoothing was *refused* at this point. Applying it to a model with ~50 %
per-window false-fire would have improved the number while hiding the defect.

## EXP-014 — The evaluation was the bug

**The single most important experiment in the project.**

Both models on the **same** 789 realistic sliding windows:

| @ threshold 0.90 | centred-clip trained (deployed) | continuous-window trained |
|---|---|---|
| miss | 0.1301 | 0.6829 |
| **false-fire** | **0.4970** | **0.0015** |

**The deployed model fires on 49.7 % of non-keyword windows.** Its reported 10.7 % came from
a test set of curated, centred, complete words — a distribution a sliding-window detector
never sees. 49.7 % is consistent with the 34.9 % observed live.

**Every accuracy figure from EXP-008 onward was measured that way.** The evaluation was wrong
before the model was.

**Why:** in continuous operation the model gets a fresh 1 s window every 200 ms, and most
contain a *partial* word. **Not one training example was ever a half-word.**

**A label-design failure recorded honestly:** the first fix used a single 0.95 overlap
boundary, so a window holding 94 % of the utterance was `unknown` and 95 % was `keyword` —
nearly identical audio, opposite labels. The model resolved it by almost never firing:
false-fire 0.0055, **miss 0.9205**. Fixed with a **don't-care band** excluding ambiguous
windows entirely.

## EXP-015 — The first streaming metric

**Question:** how does the system behave on time-ordered audio, which is what deployment is?

Held-out: 120.0 s, 38 true utterances.

| threshold | N consecutive | detected | false activations/min |
|---|---|---|---|
| 0.50 | 1 | 30/38 (78.9 %) | 4.00 |
| **0.50** | **2** | **23/38 (60.5 %)** | **0.00** |
| 0.90 | 1 | 19/38 (50.0 %) | 0.00 |

**Smoothing behaved exactly as the asymmetry predicts** — N=1→2 removed every false
activation while costing 18.4 points of detection. False windows are isolated; real
utterances span several consecutive ones.

**And the caveat attached to the good number:** zero events in 120 s does not establish a low
rate. It bounds it loosely (roughly under 1.5/min at 95 % confidence), and it is **not** the
plan's per-hour metric.

**Conclusion:** 60.5 % detection is not a usable wake word, and no threshold or smoothing
change fixes it. It is a data problem.

---

# PART 9 — Honest validation status

## What is proven

| Area | Status | Evidence |
|---|---|---|
| **Hardware** | ✅ | Chip, flash, PSRAM confirmed at runtime. Wiring checked against every reserved GPIO range |
| **Audio capture** | ✅ | 14-point checklist closed in MILESTONE-01. Sample rate measured. Bit alignment proven on all 80,000 samples. 0 clipped samples in every capture. Confirmed by ear |
| **Feature pipeline** | ✅ | Host/device parity proven **twice**: 0.000427 (log-Mel), 0.000112 (MFCC), correlation 1.0000000000 both times |
| **Training pipeline** | ✅ | Runs, converges, split by time. Two failures found and fixed |
| **Quantisation** | ✅ | int8, 24,536 bytes, 99.73 % prediction agreement with float |
| **On-device inference** | ✅ | 84.17 ms, 48 % of one core, 15,460-byte arena |
| **Detection** | ✅ | Fires on the keyword on real hardware, valid timing |

## What is NOT proven

| Gap | Detail |
|---|---|
| **Accuracy** | Deployed model fires on **49.7 %** of non-keyword windows. Not usable |
| **Best model not deployed** | `model_cont` was never quantised or flashed |
| **Smoothing not in firmware** | Measured in Python only |
| **False activations per hour** | The plan's actual metric. Never measured — 2 minutes is not an hour |
| **Any other speaker** | One speaker, one room, one day. No claim of speaker independence is permitted |
| **Latency to detection** | End-to-end latency never measured |
| **Power / long-run stability** | Never measured |
| **Post-detection streaming** | No code exists |
| **CPU target** | 48 % measured vs the plan's <10 % goal. Missed |

---

# PART 10 — Repository structure

```
Hardware (your desk)
   │
   ├── platformio.ini ............... build config: board, overrides, ESP-NN flags, COM5
   │
Firmware (runs ON the ESP32)
   ├── src/main.cpp ................. the detector: I2S → MFCC → TFLM → threshold
   ├── src/kws_model.cc/.h .......... the trained model as a C array (24,536 bytes)
   ├── src/kws_params.h ............. GENERATED constants — never edit by hand
   │
PC-side tooling (runs on your computer)
   ├── tools/record_dataset.py ...... record + auto-segment utterances
   ├── tools/record_noise.py ........ record background only
   ├── tools/features.py ............ THE feature definition — host twin of the firmware
   ├── tools/build_dataset.py ....... centred clips → training arrays
   ├── tools/build_continuous.py .... sliding windows incl. partial words
   ├── tools/train_kws.py ........... the DS-CNN and training loop
   ├── tools/quantize_kws.py ........ float → int8 → C array
   ├── tools/emit_params.py ......... model_meta.json → src/kws_params.h
   └── tools/eval_streaming.py ...... time-ordered eval + false activations/min
   │
Model artifacts
   ├── model/ ....................... the old log-Mel model (superseded)
   ├── model_mfcc/ .................. THE DEPLOYED MODEL (float + int8 + metadata)
   └── model_cont/ .................. the better model — float only, NOT deployed
   │
Data  [LOCAL ONLY — gitignored, NOT on GitHub]
   └── dataset/ ..................... positives, hard negatives, noise, raw sessions
   │
Documentation
   ├── README.md .................... entry point
   ├── docs/PROJECT_STATUS.md ....... live status
   ├── docs/DECISION-01/02 .......... keyword and speaker-strategy decisions
   └── docs/experiments/ ............ 15 experiment records + logs, figures, WAVs, code
```

**The most important structural idea:** `tools/features.py` and the feature code in
`src/main.cpp` are **twins**. They must compute identical numbers. `tools/emit_params.py`
exists so the 26 normalisation constants are *generated* rather than copied.

---

# PART 11 — Why did we do it this way?

| Decision | Reason | Benefit | Trade-off |
|---|---|---|---|
| ESP32-S3 | Needs ~40 KB RAM + fast int8 maths | 512 KB SRAM, 8 MB PSRAM, vector instructions | More complex than an 8-bit MCU |
| INMP441 digital mic | Analog wiring picks up board noise | Clean signal, no ADC needed | Must speak I2S; cannot just read a voltage |
| GPIO 4/5/6 | Verified clear of all reserved ranges | No conflict with PSRAM/USB/UART/strapping | None |
| L/R → GND | Forces a single known channel | Deterministic mono | Firmware **must** read the left slot |
| 16 kHz sampling | Speech lives below 8 kHz | Captures everything needed | Cannot represent music-quality audio |
| PlatformIO, not Arduino IDE | Board definition was wrong and had to be overridden | Every setting in Git; ESP-NN switchable | Steeper learning curve |
| `qio_opi` memory type | eFuse showed quad flash at 3.3 V | Board boots; PSRAM works | Wrong value would prevent booting |
| `ARDUINO_USB_CDC_ON_BOOT=1` | Native USB, not a UART bridge | Serial output actually arrives | Output can truncate at reset |
| **MFCC 49×13**, not log-Mel 98×40 | 98×40 made inference 22× too slow | 26.6× faster, inside budget | Accuracy fell 0.9196 → 0.8304 |
| Per-coefficient normalisation | Global scaling collapsed c2–c12 | Training works at all | 26 constants must reach firmware exactly |
| int8 quantisation | ESP-NN kernels need it | 4× smaller, much faster | Slightly more trigger-happy |
| DS-CNN | Separable convs cut compute ~10× | Fits in 24.5 KB | Less capacity than a full CNN |
| Time-based split | Neighbouring clips are near-duplicates | Honest accuracy | Smaller effective test set |
| In-band SNR for mixing | 86.9 % of our noise is below the filterbank | Augmentation is genuinely hard | More complex than broadband |
| Hard negatives | Easy negatives teach nothing | Forces a real boundary | Costs recording time |
| Threshold 0.90 | From the measured miss/false-fire table | Conservative | Raises the miss rate |

---

# PART 12 — What Claude Code did, and what remains yours

**What the AI produced:** all firmware in `src/`, all nine Python tools, the experiment
documents, the commit history, the model architecture, and the analysis scripts. It ran the
builds, the flashing, the captures, the training, and the measurements.

**What it also did — and this matters more than the code:** it stated predictions *before*
running experiments, and recorded them as failed when they failed (EXP-004's 9.6 % versus a
declared ≥10 %). It withdrew EXP-002's verdict when the stimulus turned out to be
uncontrolled. It refused smoothing in EXP-013 because it would have hidden a defect. It
rejected a floor estimator that would have turned a fail into a pass.

**What was yours, and could not have been anyone else's:**

- Every physical action: wiring, plugging in, moving the microphone
- **Every voice recording** — 281 utterances, all of them yours
- **The listening test** that validated the sample rate. No statistic could substitute
- The keyword choice, the speaker-strategy decision, the drive choice
- Confirming what you were actually doing during each capture — which is what turned EXP-002
  from an unattributable number into a real result

**Why AI-generated code still needed validation — five concrete cases from this project:**

1. The segmenter truncated the `/s`, on 16 of 31 clips. Every summary statistic looked fine
2. The CPU figure was computed with a stale 10 ms hop and read 95.9 % when it was 48 %
3. The parity re-check was skipped after the features changed, exactly as it had been noted
4. A 94 %-vs-95 % label rule produced a model with a 92 % miss rate
5. The evaluation itself was wrong for seven experiments

**None of those five produced an error message.** Every one produced confident, plausible,
wrong output. That is the argument for validation, and it is why this repository contains
fifteen experiment records instead of one "it works".

---

# PART 13 — How the project runs today

## Detection (needs the hardware)

| Step | Automatic or manual |
|---|---|
| 1. Plug the board in via USB | **Manual** |
| 2. Confirm it appears: `pio device list` → COM5 | Manual |
| 3. `pio run -t upload` — build and flash | Automatic once started |
| 4. Board boots, settles 1.5 s, starts listening | **Fully automatic, forever** |
| 5. Watch: `pio device monitor -b 921600` | Manual |

Once flashed, **step 4 needs no computer.** Power it from any USB charger and it runs.

## Recording more data (needs hardware + PC)

```bash
pio run -t upload                                   # flash the streamer firmware first
python tools/record_dataset.py 90 sentinel dataset/positive
```
You speak; segmentation, quality checks and file naming are automatic.

## Retraining (PC only, no hardware)

```bash
python tools/build_continuous.py dataset/kws_continuous.npz
D:/sih-ml-env/Scripts/python.exe tools/train_kws.py dataset/kws_continuous.npz model_cont
D:/sih-ml-env/Scripts/python.exe tools/quantize_kws.py dataset/kws_continuous.npz model_cont
python tools/emit_params.py model_cont src/kws_params.h
cp model_cont/kws_model.cc model_cont/kws_model.h src/
pio run -t upload
```

Note the two different Pythons: the system Python for recording/analysis, and the
TensorFlow environment on **D:** for anything touching the model. C: was 99 % full.

---

# PART 14 — Moving to another computer, and the phone question

## What is on GitHub versus what is not

| | On GitHub | Local only |
|---|---|---|
| Firmware source, `platformio.ini` | ✅ | |
| All nine Python tools | ✅ | |
| **Trained models** (`model_mfcc/`, `model_cont/`) | ✅ | |
| All experiment docs, logs, figures, WAVs | ✅ | |
| **`dataset/` — every recording you made** | ❌ | ✅ **only on this PC** |

**This is the single most important portability fact.** `dataset/` is gitignored. If this
computer dies, **281 recordings and 8 raw sessions are gone**, and the model could never be
retrained. The `.tflite` and `.keras` files survive, so the *existing* model would still
work — but you could not improve it.

## Scenario A — another Windows laptop, hardware in hand

**Works fully.** You need:

1. Clone the repo
2. Install Python 3.11 (not 3.14 — TensorFlow does not support it) and PlatformIO Core
3. `pip install pyserial numpy scipy matplotlib`
4. First `pio run` downloads ~1 GB of toolchain automatically
5. Plug in the board, check the COM port (**it will probably not be COM5**) and update
   `platformio.ini`

**Drivers:** the S3 uses native USB-Serial/JTAG, so Windows 10/11 needs no extra driver.

**Can do without copying `dataset/`:** build, flash, run detection, monitor.
**Cannot do:** retrain — you would have to record a new dataset from scratch.

## Scenario B — a friend's laptop

Same as A. They will be recording *their* voice, and per DECISION-02 this model is
speaker-dependent — **it will likely respond poorly or not at all to them.**

## Scenario C — no hardware, software only

| Possible | Not possible |
|---|---|
| Read all code and documentation | Capture audio |
| Retrain **if you copied `dataset/`** | Flash firmware |
| Re-run offline evaluation on saved data | Run detection |
| Compile firmware (`pio run`) | Verify anything on hardware |

## Scenario D — Claude Code from your phone

This needs care, because "Claude Code Pro" does not mean the project runs anywhere.

**What "cloud" does and does not mean here:**

- **The AI runs remotely.** The reasoning happens on Anthropic's servers
- **Your project does not.** Every build, flash, capture and training run executed on *this
  computer*, driven by that AI
- **The ESP32 is physically attached to this PC by a USB cable.** No cloud service reaches it

**From a phone you CAN:** read code, read reports, ask questions, plan changes, and — if
Claude Code is connected to a machine that has the repo — edit files and commit.

**From a phone you CANNOT:** flash the ESP32, record audio, run detection, or verify
anything on hardware. **A USB cable does not extend to the cloud.**

**Nothing in this project runs in the cloud.** Detection runs on the ESP32-S3.
Training ran on this PC. GitHub stores files; it does not execute them.

---

# PART 15 — Hackathon relevance (PS26172)

## What I could and could not verify

**I could not verify the official problem statement text.** I searched the repository: the
only occurrence of "PS26172" is the title line of `README.md`, which was written from your own
framing of the project. There is no official problem statement document in the repo, and I
have no verified access to the SIH 2026 statement text.

**I will not invent its requirements.** What follows maps the implementation against the
**engineering plan that is in the repo** — `Low_Latency_Efficient_Voice_Activator_Implementation_Plan.pdf`
— which is the only written specification this project actually has.

## Implementation versus that plan

| Plan requirement | Status |
|---|---|
| ESP32-S3 + INMP441, I2S capture at 16 kHz | ✅ **[VALIDATED]** |
| Circular PCM buffer | ✅ rolling feature buffer |
| Feature extraction (log-Mel or MFCC) | ✅ **[VALIDATED]** — parity proven |
| Tiny int8 TFLite Micro model | ✅ 24,536 bytes |
| ESP-NN optimisation | ✅ 5.48× measured |
| Custom keyword, not a pre-trained wake word | ✅ "Sentinel", your voice |
| Model size 40–150 KB | ✅ 24.5 KB (under) |
| Inference every 100–200 ms | ✅ every 200 ms |
| Inference 15–60 ms | ⚠️ **84.17 ms** — over |
| Idle CPU well under 10 % | ❌ **48 %** |
| Temporal smoothing | ❌ **[NOT IN FIRMWARE]** |
| Near-zero false positives | ❌ **49.7 % of windows** |
| Post-detection streaming to remote ASR | ❌ **[NOT IMPLEMENTED]** |
| Dual-core split | ❌ single core |
| Wi-Fi modem-sleep / power strategy | ❌ never measured |
| End-to-end latency instrumentation | ❌ never measured |

## Honest assessment

**Strongly satisfied:** the complete sensing-and-inference chain — microphone through
features through a quantised neural network — is built, deployed on the physical chip, and
measured at every stage.

**Partially satisfied:** latency and CPU are measured but miss their targets.

**Not addressed:** everything after detection (streaming, ASR, power, dual-core).

**The genuinely distinctive evidence** is not the working detector — it is
`docs/experiments/`. Fifteen records with predictions stated before results, four withdrawn
or failed verdicts, and a documented case (EXP-014) where the *evaluation method itself* was
found to be wrong and corrected. Most submissions can show a demo. Few can show that they
know which of their own numbers to distrust.

---

# PART 16 — Answers to the questions you wanted to be able to answer

**Why is the INMP441 connected this way?** SCK and WS are outputs from the ESP32 because the
mic has no clock of its own; SD is an input because only the mic produces audio. L/R goes to
ground to force a single known channel. GPIO 4/5/6 were checked clear of the flash/PSRAM bus,
USB, UART0 and strapping pins.

**Why does it need I2S?** It is a digital microphone. It does not output a voltage you can
measure — it outputs bits, and I2S is the protocol for clocking them out.

**Why does the ESP32 need firmware?** A microcontroller has no operating system. It runs one
program from power-on. That program is everything: driving the mic, computing features,
running the network.

**Why does the PC receive PCM?** Because that is the rawest honest form of the audio. Any
processing before transfer would mean the PC sees something different from what the device
sees — the mismatch we spent two experiments guarding against.

**Why do we process the audio?** A neural network cannot learn from 16,000 numbers per second.
MFCC compresses one second into 49×13 = 637 numbers that describe *shape* rather than
*pressure*.

**Why does the model need training examples?** It has no idea what "Sentinel" means. It only
learns what feature grids labelled `keyword` look like.

**Why negative and background classes?** Without them the model learns "someone spoke" and
fires on everything. Hard negatives force it to find the actual boundary.

**Why does the model sometimes predict wrongly?** Because it matches *shapes*, and other
sounds produce similar shapes. Measured: 49.7 % of non-keyword windows on the deployed model.

**What is inference?** One `Invoke()` call on the interpreter — 84.17 ms, every 200 ms.

**What is confidence?** How strongly this model matches the input to what it was trained on.
**Not** a probability of being right.

**What runs on the ESP32?** Everything needed to detect: I2S capture, MFCC, the int8 network,
the threshold.

**What runs on the PC?** Everything needed to *build*: recording, dataset assembly, training,
quantisation, evaluation, flashing. None of it at detection time.

**What does my friend need tomorrow?** The repo, Python 3.11, PlatformIO, the board and the
mic — and to change the COM port. They cannot retrain without a copy of `dataset/`, which is
not on GitHub.

**Can I work on it from my phone?** You can read, plan and edit code. You cannot flash,
record, or test on hardware. The USB cable does not reach the cloud.

---

*The system is modular — capture, features, model and decision logic are separate components
with measured interfaces between them — so individual parts can be changed without rebuilding
the rest. This report describes only what exists today.*
