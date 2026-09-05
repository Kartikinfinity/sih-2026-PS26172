# SIH 2026 — PS26172 · Low-Latency Efficient Voice Activator

Custom keyword spotting (KWS) and low-latency audio streaming on an
**ESP32-S3-WROOM-1-N16R8** with an **INMP441** MEMS I2S microphone.

Target architecture:

```
INMP441 → I2S DMA → PCM ring buffer → preprocessing → feature extraction
        → tiny int8 KWS model → temporal smoothing → keyword detection
        → low-latency audio streaming → remote ASR
```

---

## Current status

**MILESTONE 01 reached — the audio capture path is fully validated.**
Phases 2–5 complete. See [`docs/PROJECT_STATUS.md`](docs/PROJECT_STATUS.md).

**No machine learning exists yet.** No feature extraction, no model, no keyword
detection. No claims are made about latency, CPU load, or accuracy, because none
of those have been measured.

---

## Hardware (locked)

| Component | Detail |
|---|---|
| MCU module | ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM) |
| Microphone | INMP441 digital I2S MEMS |
| Board | ESP32-S3-DevKitC-1-style, breadboard wiring |
| Connection | native USB-Serial/JTAG (`303A:1001`), COM5 @ 921600 |

### Wiring (verified — see [EXP-001](docs/experiments/EXP-001-wiring-confirmation.md))

| INMP441 | ESP32-S3 | Purpose |
|---|---|---|
| SCK | GPIO 6 | I2S bit clock (BCLK) |
| WS | GPIO 5 | I2S word select (LRCLK) |
| SD | GPIO 4 | I2S serial data (mic → MCU) |
| VDD | 3V3 | supply (1.8–3.3 V part; **never 5 V**) |
| GND | GND | ground |
| L/R | GND | selects **LEFT** channel |

GPIO 4/5/6 were checked against every reserved range on this SoC (flash/PSRAM
bus 27–37, USB 19/20, UART0 43/44, strapping 0/3/45/46) and are clear.

---

## Experimentally verified facts

Every row is a measurement, not an assumption or a datasheet quote.

| Fact | Value | Established by |
|---|---|---|
| Chip | ESP32-S3 (QFN56) rev v0.2, 2 × LX7 @ 240 MHz | EXP-000 |
| Flash | 16 MB, quad-SPI @ 3.3 V (eFuse) | EXP-000 |
| PSRAM | 8 MB octal, `psramFound()` true | EXP-000 |
| Module identity | **WROOM-1-N16R8**, not WROOM-2 N16R8V | EXP-000 (eFuse: quad @3.3 V) |
| **Sample rate** | **16,001.60 Hz (+0.010 % error)** | EXP-003 |
| Sample format | 24-bit left-justified in 32-bit slot | EXP-003 (all 80,000 samples) |
| Noise character | 64.6 % of energy below 100 Hz (sub-audio drift @ 0.2–2.4 Hz) | EXP-003 |
| Clipping | 0 clipped samples across all captures | EXP-002…004 |
| Speech-band response | 300–3400 Hz occupancy: **1.4 % ambient → 43.8 % speaking** | EXP-004 |
| Audible correctness | recording confirmed natural in pitch and speed | EXP-004 listening test |

---

## Toolchain

| Component | Version |
|---|---|
| PlatformIO Core | 6.1.19 |
| Platform | `espressif32 @ 7.1.1` |
| Framework | Arduino core 2.0.17 (**ESP-IDF 4.4.7**) |
| Toolchain | xtensa-esp32s3 GCC 8.4.0 |
| I2S driver | legacy `driver/i2s.h` |

> **Known deviation from the implementation plan.** The plan specifies ESP-IDF v5.x
> and `driver/i2s_std.h`. That header does not exist in ESP-IDF 4.4, which Arduino
> core 2.0.17 pins us to. The legacy driver is fully sufficient for Phases 1–7, so
> the verified toolchain has deliberately not been churned. The decision is deferred
> to Phase 14 (TFLite Micro / `esp-tflite-micro`), where the IDF 5.x component
> ecosystem actually matters. Tracked in `docs/PROJECT_STATUS.md`.

### Build and flash

```bash
pio run                 # build
pio run -t upload       # flash to COM5
pio device monitor -b 921600
```

`platformio.ini` overrides the stock `esp32-s3-devkitc-1` board definition, which is
the **N8 / no-PSRAM** variant. Without `board_build.arduino.memory_type = qio_opi`,
`-DBOARD_HAS_PSRAM` and the 16 MB partition table, the 8 MB PSRAM is absent at runtime.
`-DARDUINO_USB_CDC_ON_BOOT=1` is required or `Serial` binds to UART0 (GPIO 43/44) and
nothing reaches the native USB port.

---

## Experiment log

Full records in [`docs/experiments/`](docs/experiments/), each with objective,
hypothesis, pre-declared pass/fail criteria, measurements, and an explicit statement
of what was **not** proven.

| ID | Subject | Status |
|---|---|---|
| [EXP-000](docs/experiments/EXP-000-environment-verification.md) | Environment & hardware verification | PASS |
| [EXP-001](docs/experiments/EXP-001-wiring-confirmation.md) | Physical wiring cross-check | PASS |
| [EXP-002](docs/experiments/EXP-002-first-i2s-capture.md) | First raw I2S capture | INCONCLUSIVE (uncontrolled stimulus) |
| [EXP-002B](docs/experiments/EXP-002B-controlled-stimulus.md) | Controlled-stimulus repeat | PARTIAL PASS |
| [EXP-003](docs/experiments/EXP-003-sample-rate-and-spectrum.md) | Sample-rate verification + spectral diagnosis | PASS |
| [EXP-004](docs/experiments/EXP-004-highpass-speech-test.md) | On-device high-pass + AC detector + first WAV | PARTIAL PASS (**prediction failed**) |
| [MILESTONE-01](docs/experiments/MILESTONE-01-audio-capture-validated.md) | Audio capture path validated | ✅ |

---

## Methodology

Every stage follows **LEARN → HYPOTHESIS → EXPERIMENT → OBSERVE → ANALYZE →
VALIDATE → DOCUMENT → PROCEED**, and nothing is called working because it compiled,
flashed, or returned bytes.

Two entries in the log exist specifically because that discipline was applied:

- **EXP-002 was downgraded to INCONCLUSIVE** after the fact. Its 16.3× "response to
  sound" was recorded during a capture in which the user performed no actions, so
  the stimulus was uncontrolled and the result unattributable.
- **EXP-004's pre-declared prediction failed.** Speech reached 9.6× the noise floor
  against a stated bar of ≥10×. An alternative floor estimator would have yielded
  11.4× and "passed"; it was rejected, because choosing the estimator after seeing
  the data is how a result gets faked. The failure was informative: it proved
  broadband RMS is the wrong detector and motivated band-limited measurement.

---

## Repository layout

```
platformio.ini                  build config (documented overrides)
src/main.cpp                    current experiment firmware
docs/PROJECT_STATUS.md          live status: verified facts, open questions, next step
docs/experiments/               one record per experiment
docs/experiments/code/          archived firmware for each experiment
docs/experiments/*.wav|pcm|png  raw evidence and analysis figures
*.pdf                           vendor datasheets and the implementation plan
```

---

## Open questions

1. ESP-IDF 4.4 vs 5.x — deferred to Phase 14 (see toolchain note above).
2. Residual sub-100 Hz energy is still 41 % in quiet after the 12 dB/octave
   high-pass. A steeper filter or higher corner has not been evaluated.
3. Serial PCM transfer truncated at 96.3 % in EXP-004 (receive deadline too short);
   the artifact was identified and excluded, but the tooling needs fixing.

## Next

**EXP-005** — band-limited (300–3400 Hz) energy detector, the on-ramp to the
Phase 7 Mel/MFCC feature pipeline.

---

## AI assistance disclosure

Firmware, analysis tooling, and experiment documentation in this repository were
developed with Claude Code (Anthropic) acting as implementation and analysis
assistant. All hardware wiring, physical test execution, and acceptance decisions
were performed by the author. All measurements reported here come from the actual
hardware.
