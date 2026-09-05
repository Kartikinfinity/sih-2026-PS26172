// EXP-002 -- First INMP441 I2S capture: RAW data inspection.
//
// This program does NOT try to produce clean audio. Its only job is to answer
// three questions honestly:
//   1. Does the I2S peripheral initialise on the user's confirmed pins?
//   2. Do we actually receive the number of bytes we asked for?
//   3. Is the received data a real signal (varies, responds to sound), or is it
//      zeros / a stuck constant (which would mean the mic is not really talking)?
//
// It prints RAW 32-bit words in hex so bit alignment can be judged from evidence
// rather than assumed, plus per-block statistics.
//
// Wiring confirmed by user 2026-09-05 (see docs/experiments/EXP-001):
//   INMP441 SCK -> GPIO 6   (bit clock,  ESP32 -> mic)
//   INMP441 WS  -> GPIO 5   (word select,ESP32 -> mic)
//   INMP441 SD  -> GPIO 4   (data,       mic  -> ESP32)
//   INMP441 L/R -> GND      => LEFT channel  => I2S_CHANNEL_FMT_ONLY_LEFT

#include <Arduino.h>
#include <string.h>

#include "driver/i2s.h"

namespace {

constexpr int PIN_I2S_SCK = 6;
constexpr int PIN_I2S_WS = 5;
constexpr int PIN_I2S_SD = 4;

constexpr int SAMPLE_RATE = 16000;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

// One read = 512 frames. At 16 kHz that is 32 ms of audio per block.
constexpr int FRAMES = 512;
int32_t g_buf[FRAMES];

// INMP441 sends 24 bits of audio left-justified inside a 32-bit slot, so the
// sample occupies bits 31..8 and the low 8 bits are padding. An arithmetic
// right shift by 8 recovers a signed 24-bit value.
constexpr int32_t FULL_SCALE_24 = 8388607;  // 2^23 - 1

bool i2sInit() {
  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;  // L/R tied to GND
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;

  esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  Serial.printf("  i2s_driver_install  -> %s\n", esp_err_to_name(err));
  if (err != ESP_OK) return false;

  i2s_pin_config_t pins;
  memset(&pins, 0, sizeof(pins));
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_I2S_SCK;
  pins.ws_io_num = PIN_I2S_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;  // receive only
  pins.data_in_num = PIN_I2S_SD;

  err = i2s_set_pin(I2S_PORT, &pins);
  Serial.printf("  i2s_set_pin         -> %s\n", esp_err_to_name(err));
  if (err != ESP_OK) return false;

  i2s_zero_dma_buffer(I2S_PORT);
  return true;
}

struct BlockStats {
  size_t bytes_read;
  int32_t min24, max24;
  double mean24;   // DC offset
  double rms24;
  int zero_count;
  int same_as_first;
};

BlockStats analyse(size_t bytes_read) {
  BlockStats s;
  s.bytes_read = bytes_read;
  s.min24 = INT32_MAX;
  s.max24 = INT32_MIN;
  s.zero_count = 0;
  s.same_as_first = 0;

  const int n = (int)(bytes_read / sizeof(int32_t));
  if (n == 0) {
    s.min24 = s.max24 = 0;
    s.mean24 = s.rms24 = 0;
    return s;
  }

  const int32_t first = g_buf[0] >> 8;
  double sum = 0.0, sumsq = 0.0;

  for (int i = 0; i < n; ++i) {
    const int32_t v = g_buf[i] >> 8;  // arithmetic shift -> signed 24-bit
    if (v < s.min24) s.min24 = v;
    if (v > s.max24) s.max24 = v;
    if (v == 0) s.zero_count++;
    if (v == first) s.same_as_first++;
    sum += (double)v;
    sumsq += (double)v * (double)v;
  }

  s.mean24 = sum / n;
  s.rms24 = sqrt(sumsq / n);
  return s;
}

void printHeader() {
  Serial.println();
  Serial.println("================================================================");
  Serial.println(" EXP-002  INMP441 RAW I2S CAPTURE");
  Serial.printf("  pins: SCK=%d  WS=%d  SD=%d   |  %d Hz, 32-bit slot, LEFT only\n",
                PIN_I2S_SCK, PIN_I2S_WS, PIN_I2S_SD, SAMPLE_RATE);
  Serial.println("================================================================");
}

}  // namespace

void setup() {
  Serial.begin(921600);
  delay(600);

  printHeader();
  Serial.println("[ I2S INIT ]");

  if (!i2sInit()) {
    Serial.println("  >>> I2S INIT FAILED - stopping. Do not trust any later output.");
    while (true) delay(1000);
  }
  Serial.println("  I2S initialised OK");
  Serial.println();
  Serial.println("Legend:  raw = first 4 raw 32-bit words (hex, straight from DMA)");
  Serial.println("         min/max/mean/rms are signed 24-bit values (raw >> 8)");
  Serial.println("         full scale = +/-8388607");
  Serial.println();
}

void loop() {
  size_t bytes_read = 0;
  const esp_err_t err =
      i2s_read(I2S_PORT, (void *)g_buf, sizeof(g_buf), &bytes_read, portMAX_DELAY);

  if (err != ESP_OK) {
    Serial.printf("i2s_read ERROR: %s\n", esp_err_to_name(err));
    delay(250);
    return;
  }

  const BlockStats s = analyse(bytes_read);
  const int n = (int)(bytes_read / sizeof(int32_t));

  // Percentage of full scale, so loudness is readable without knowing 2^23.
  const double pct = 100.0 * s.rms24 / (double)FULL_SCALE_24;

  Serial.printf(
      "raw %08x %08x %08x %08x | n=%3d bytes=%4u | min=%9ld max=%9ld "
      "mean=%9.1f rms=%9.1f (%.4f%% FS) zeros=%3d flat=%3d\n",
      (unsigned)g_buf[0], (unsigned)g_buf[1], (unsigned)g_buf[2], (unsigned)g_buf[3],
      n, (unsigned)bytes_read, (long)s.min24, (long)s.max24, s.mean24, s.rms24, pct,
      s.zero_count, s.same_as_first);

  // ~4 lines per second keeps the log readable by a human.
  delay(250);
}
