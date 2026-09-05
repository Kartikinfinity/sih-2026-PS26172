# EXP-000 — Environment & Hardware Verification

| Field | Value |
|---|---|
| Experiment ID | EXP-000 |
| Date | 2026-09-05 |
| Phase | 0 (existing environment verification) |
| Status | **PASS** |

## Objective

Prove that the ESP32-S3 board is the claimed N16R8 hardware, that the toolchain builds and
flashes, and that a serial link exists — before any microphone work begins.

## Hardware configuration

ESP32-S3 DevKitC-1-style board, powered and connected over the S3's **native USB**
(USB-Serial/JTAG, `303A:1001`), enumerating as COM5. INMP441 physically attached by the
user but **not wired into any code path** in this experiment.

## Software configuration

PlatformIO Core 6.1.19 · espressif32 7.1.1 · Arduino core 2.0.17 (ESP-IDF 4.4.7) ·
xtensa-esp32s3 GCC 8.4.0. Board ID `esp32-s3-devkitc-1` with memory overrides
(16 MB flash, `default_16MB.csv`, `qio_opi`, `BOARD_HAS_PSRAM`).

## Procedure

1. Read the board definition JSON from the installed platform (not from memory).
2. Discover it is the **N8, no-PSRAM** variant; override flash size, partition table and
   PSRAM memory type in `platformio.ini`.
3. Build, upload, and read the device's own runtime report over serial.

## Expected vs actual

| Measurement | Expected (N16R8) | Actual | Verdict |
|---|---|---|---|
| Chip model | ESP32-S3 | ESP32-S3 (QFN56) rev v0.2 | PASS |
| Flash size | 16 MB | 16,777,216 B (configured **and** SPI-detected) | PASS |
| PSRAM | 8 MB | 8,386,231 B total, `psramFound()` = true | PASS |
| Flash interface | quad | eFuse: quad (4 data lines), 3.3 V | PASS |
| Stability | no resets | 5 s heartbeats, monotonic uptime, constant free heap | PASS |

## Key findings

1. The stock `esp32-s3-devkitc-1` board definition is the **N8 / no-PSRAM** variant. It
   must be overridden. Without `-DBOARD_HAS_PSRAM` + `memory_type = qio_opi`, the 8 MB
   PSRAM is simply absent at runtime.
2. **`qio_opi` is correct, `opi_opi` would be wrong.** eFuse reports quad-SPI flash at
   3.3 V. A WROOM-2 N16R8V would report octal flash at 1.8 V. This settles the module
   identification: it is a **WROOM-1-N16R8**.
3. On native USB, Arduino `Serial` binds to UART0 (GPIO 43/44) unless
   `-DARDUINO_USB_CDC_ON_BOOT=1` is set. Without it the upload succeeds and the board runs
   correctly but **emits nothing** — a silent failure that looks like a dead board.

## Interpretation

Hardware and toolchain are proven. Point 3 is a useful lesson for later phases: a
successful flash plus silence is not evidence of failure, and a successful flash plus
output is not evidence of correct behaviour. Both require independent confirmation.

## Decision

**PROCEED** to Phase 1 (wiring confirmation). Nothing here says anything about whether the
microphone works — that remains completely unproven.
