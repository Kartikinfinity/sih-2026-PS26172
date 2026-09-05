# PROJECT STATUS — Low-Latency Voice Activator (ESP32-S3 + INMP441)

Last updated: 2026-09-05

## Current stage

**PHASE 0 — environment verification: COMPLETE**
**PHASE 1 — wiring verification: COMPLETE (EXP-001, PASS)**
**PHASE 2-3 — I2S capture: PASS (EXP-002B speech confirmed)**
**PHASE 4-5 — PCM transfer + signal analysis: PASS (EXP-003)**
**PHASE 6 — preprocessing: COMPLETE (EXP-005). Band-limited detector passes on every word.**

**MILESTONE 01 reached: the audio capture path is fully validated** (see
`experiments/MILESTONE-01-audio-capture-validated.md`) — sample rate measured, bit
alignment proven on every sample, zero clipping, and the recording confirmed by ear to
sound natural in pitch and speed. Phases 2-5 complete.

No ML, no features, no keyword detection exists yet.

## Verified facts (experimentally observed, not assumed)

Measured on-device and read from installed packages — see `experiments/EXP-000`.

| Fact | Value | How it was verified |
|---|---|---|
| Chip | ESP32-S3 (QFN56), rev v0.2 | esptool chip detect + on-device `esp_chip_info()` |
| Cores / clock | 2 × LX7 @ 240 MHz | on-device runtime report |
| Flash | 16 MB (16,777,216 B) | esptool SPI read + on-device `esp_flash_get_size()` |
| Flash interface | quad (QIO), 3.3 V | eFuse readout via esptool |
| PSRAM | 8 MB (8,386,231 B usable) | on-device `ESP.getPsramSize()`, `psramFound()` true |
| Internal heap | 394,924 B total / 370,680 B free | on-device runtime report |
| Module identification | ESP32-S3-WROOM-1-N16R8 | eFuse says quad flash @3.3 V, so NOT WROOM-2 N16R8V |
| Serial port | COM5, VID:PID 303A:1001 | native USB-Serial/JTAG, survives reset |
| **Sample rate** | **16,001.60 Hz (+0.010%)** | EXP-003, frames vs esp_timer |
| 24-in-32 bit alignment | confirmed, all 80,000 samples | EXP-003 |
| Sub-audio drift | 64.6% of energy below 100 Hz | EXP-003 spectrum |
| UART0 pins | TX=43, RX=44 | Arduino esp32s3 variant `pins_arduino.h` |

## Toolchain (already installed — do not reinstall)

| Component | Version |
|---|---|
| PlatformIO Core | 6.1.19 |
| Platform | espressif32 @ 7.1.1 |
| Framework | Arduino (framework-arduinoespressif32 3.20017 = core 2.0.17) |
| **Underlying ESP-IDF** | **v4.4.7** |
| Toolchain | xtensa-esp32s3 GCC 8.4.0 |
| ESP-DSP | present (`libespressif__esp-dsp.a`) |
| I2S API available | **`driver/i2s.h` (legacy) ONLY — no `driver/i2s_std.h`** |

## Open questions / known risks

1. **The roadmap specifies ESP-IDF v5.x + `driver/i2s_std.h`; we have IDF 4.4.7 + legacy
   `driver/i2s.h`.** Verified by listing headers in the installed SDK. The legacy driver is
   fully sufficient for Phases 1-7 (capture → PCM → WAV → analysis → features), so no
   toolchain change is justified yet. **Decision deferred to Phase 14** (TFLite Micro /
   esp-tflite-micro deployment), where the IDF 5.x component ecosystem actually matters.
   Do not churn the verified toolchain before then.
2. Actual physical wiring is **unconfirmed**. The roadmap's suggested pins and the user's
   reported pins must be treated as claims until the user confirms the physical board.
3. Whether the INMP441 produces a valid signal is **entirely unproven**. Nothing about
   audio capture has been demonstrated.

## Next experiment

**EXP-006 — Phase 7 feature extraction: framing, windowing, FFT, Mel filterbank.**
EXP-005 showed band-limited energy improves the PEAK by only 1.39x over wideband and
leaves p90/mean unchanged — a scalar detector cannot tell a keyword from a door slam.
Feature *vectors* describing spectral shape are required.

## Explicitly NOT done yet

Nothing related to: I2S init, DMA, PCM capture, WAV, DSP, feature extraction, TFLite
Micro, KWS models, training, streaming, or Wi-Fi.
