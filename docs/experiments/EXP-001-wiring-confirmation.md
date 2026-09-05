# EXP-001 — Physical Wiring Confirmation

| Field | Value |
|---|---|
| Experiment ID | EXP-001 |
| Date | 2026-09-05 |
| Phase | 1 (hardware/wiring verification) |
| Status | **PASS** (with one item unanswered, non-blocking) |

## Objective

Establish the user's ACTUAL physical wiring as hardware source of truth, and cross-check it
against ESP32-S3-WROOM-1-N16R8 pin constraints and INMP441 requirements — before any I2S
code is written against assumed pins.

## User-reported wiring (source of truth)

| INMP441 pin | Connected to | Meaning |
|---|---|---|
| SCK | GPIO 6 | I2S bit clock (BCLK), ESP32 → mic |
| WS | GPIO 5 | I2S word select (LRCLK), ESP32 → mic |
| SD | GPIO 4 | I2S serial data, mic → ESP32 |
| VDD | 3V3 | supply |
| GND | GND | ground |
| L/R | GND | **selects LEFT channel** |
| CHIPEN | not reported | assumed tied high on breakout (common) |

## Cross-check against ESP32-S3 reserved pins

Verified by reading the installed SoC headers, not from memory:

| Reserved range | GPIOs | Source | Conflict with 4/5/6? |
|---|---|---|---|
| Flash/PSRAM main SPI | 27–32 | `soc/esp32s3/spi_pins.h` | No |
| Octal PSRAM extension | 33–37 | `soc/esp32s3/spi_pins.h` (`*_OCT`) | No |
| USB D-/D+ | 19, 20 | `USB_DM/DP_GPIO_NUM` | No |
| UART0 | 43, 44 | `variants/esp32s3/pins_arduino.h` | No |
| Strapping | 0, 3, 45, 46 | ESP32-S3 documentation | No |

GPIO 4/5/6 are defined in the Arduino variant as `A3/A4/A5` (ADC1) and `T4/T5/T6` (touch).
These are **alternate functions, not reservations** — the pins are free for digital I2S use.

## Analysis

- **VDD → 3V3 is correct.** The INMP441 is a 1.8–3.3 V part. 5 V would risk damage.
- **L/R → GND forces LEFT-channel output.** This is the single most important consequence
  for software: the I2S driver must read the LEFT slot
  (`I2S_CHANNEL_FMT_ONLY_LEFT`). If we read the right slot instead, the mic is
  electrically fine but returns a stream of zeros — a failure that mimics dead hardware.
- **CHIPEN unreported.** Most INMP441 breakouts tie it high internally. If EXP-002 returns
  all zeros, this becomes a suspect. Non-blocking for now.

## Verdict

**PASS.** Wiring is electrically sound, free of every reserved GPIO range, and matches the
roadmap's recommended pin set (SD=4, WS=5, SCK=6). No changes requested of the user.

## Decision

**PROCEED** to EXP-002 (first I2S capture). Note that this experiment proves only that the
wiring is *plausible and safe* — it proves nothing about whether the microphone actually
produces data.
