// EXP-004 -- On-device high-pass + AC-coupled level detector + WAV capture.
//
// EXP-003 proved 64.6% of the signal's energy sits below 100 Hz as sub-audio
// drift (peaks at 0.2-2.4 Hz), and that removing it offline dropped the noise
// floor 2.5x. This experiment moves that filter ONTO the device and re-runs the
// controlled speech test with a detector that measures sound instead of drift.
//
// Prediction to falsify: speech >= 10x the filtered noise floor.
//
// It also buffers the filtered audio so a WAV can be written on the PC -- the
// one check no statistic replaces: actually listening.
//
// Wiring (EXP-001): SCK=GPIO6, WS=GPIO5, SD=GPIO4, L/R=GND (left channel).

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "driver/i2s.h"
#include "esp_timer.h"

namespace {

constexpr int PIN_I2S_SCK = 6;
constexpr int PIN_I2S_WS = 5;
constexpr int PIN_I2S_SD = 4;

constexpr int SAMPLE_RATE = 16000;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

constexpr int CHUNK = 512;              // 32 ms per block
int32_t g_chunk[CHUNK];

constexpr int CAPTURE_SECONDS = 25;
constexpr long CAPTURE_FRAMES = (long)SAMPLE_RATE * CAPTURE_SECONDS;
int16_t *g_wav = nullptr;               // filtered audio, 16-bit, in PSRAM

// ---- First-order high-pass, cascaded twice (12 dB/octave) --------------------
// y[n] = a * (y[n-1] + x[n] - x[n-1]),  a = RC / (RC + dt),  RC = 1/(2*pi*fc)
// EXP-003 showed a single 6 dB/octave section leaves too much of the 50-100 Hz
// band (30.5% of energy), so two sections are used.
constexpr float HP_FC = 100.0f;

struct HighPass1 {
  float a, x_prev, y_prev;
  void init(float fc, float fs) {
    const float rc = 1.0f / (2.0f * (float)M_PI * fc);
    const float dt = 1.0f / fs;
    a = rc / (rc + dt);
    x_prev = 0.0f;
    y_prev = 0.0f;
  }
  inline float step(float x) {
    const float y = a * (y_prev + x - x_prev);
    x_prev = x;
    y_prev = y;
    return y;
  }
};

HighPass1 g_hp1, g_hp2;

bool i2sInit() {
  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk = 0;
  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) return false;

  i2s_pin_config_t pins;
  memset(&pins, 0, sizeof(pins));
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_I2S_SCK;
  pins.ws_io_num = PIN_I2S_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = PIN_I2S_SD;
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) return false;
  i2s_zero_dma_buffer(I2S_PORT);
  return true;
}

void settle(int ms) {
  const int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
  size_t br;
  while (esp_timer_get_time() < end) {
    i2s_read(I2S_PORT, (void *)g_chunk, sizeof(g_chunk), &br, portMAX_DELAY);
  }
  // Prime the filter so its startup transient is not recorded as signal.
  for (int i = 0; i < CHUNK; ++i) {
    g_hp2.step(g_hp1.step((float)(g_chunk[i] >> 8)));
  }
}

}  // namespace

void setup() {
  Serial.begin(921600);
  delay(800);

  Serial.println();
  Serial.println("================================================================");
  Serial.println(" EXP-004  HIGH-PASS + AC DETECTOR + WAV CAPTURE");
  Serial.printf("  high-pass: 2 x 1st-order @ %.0f Hz (12 dB/octave)\n", HP_FC);
  Serial.println("================================================================");

  if (!i2sInit()) {
    Serial.println("I2S INIT FAILED");
    while (true) delay(1000);
  }

  g_hp1.init(HP_FC, (float)SAMPLE_RATE);
  g_hp2.init(HP_FC, (float)SAMPLE_RATE);

  g_wav = (int16_t *)ps_malloc((size_t)CAPTURE_FRAMES * sizeof(int16_t));
  if (g_wav == nullptr) {
    Serial.println("ps_malloc FAILED");
    while (true) delay(1000);
  }
  Serial.printf("[ PSRAM ] %ld frames buffered (%ld bytes)\n", CAPTURE_FRAMES,
                CAPTURE_FRAMES * (long)sizeof(int16_t));

  Serial.println("[ SETTLE ] 1500 ms (filter primed)");
  settle(1500);

  Serial.printf("[ CAPTURE ] %d s -- START YOUR SEQUENCE NOW\n", CAPTURE_SECONDS);
  Serial.println("t(s)   raw_ac      hp_ac    ratio");

  long got = 0;
  int block = 0;
  size_t br;
  const int64_t t0 = esp_timer_get_time();

  while (got < CAPTURE_FRAMES) {
    const long want = min((long)CHUNK, CAPTURE_FRAMES - got);
    i2s_read(I2S_PORT, (void *)g_chunk, (size_t)want * sizeof(int32_t), &br,
             portMAX_DELAY);
    const int n = (int)(br / sizeof(int32_t));

    // Pass 1: raw statistics (for the before/after comparison).
    double sum = 0.0, sumsq = 0.0;
    for (int i = 0; i < n; ++i) {
      const double v = (double)(g_chunk[i] >> 8);
      sum += v;
      sumsq += v * v;
    }
    const double mean = sum / n;
    const double raw_ac = sqrt(fmax(sumsq / n - mean * mean, 0.0));

    // Pass 2: high-pass, then AC level of the FILTERED signal.
    double hsq = 0.0;
    for (int i = 0; i < n; ++i) {
      const float y = g_hp2.step(g_hp1.step((float)(g_chunk[i] >> 8)));
      hsq += (double)y * (double)y;
      // 24-bit -> 16-bit for the WAV. No gain applied: an honest level.
      int32_t s16 = (int32_t)(y / 256.0f);
      if (s16 > 32767) s16 = 32767;
      if (s16 < -32768) s16 = -32768;
      g_wav[got + i] = (int16_t)s16;
    }
    const double hp_ac = sqrt(hsq / n);  // already zero-mean after high-pass

    got += n;
    if ((block++ % 8) == 0) {
      Serial.printf("%5.1f %10.0f %10.0f %7.2f\n",
                    (double)(esp_timer_get_time() - t0) / 1e6, raw_ac, hp_ac,
                    hp_ac > 1.0 ? raw_ac / hp_ac : 0.0);
    }
  }

  Serial.printf("[ CAPTURE ] done, %ld frames\n", got);
  Serial.flush();
  delay(200);
  Serial.printf("BEGIN_PCM %ld 2 %d\n", got, SAMPLE_RATE);
  Serial.flush();

  const uint8_t *p = (const uint8_t *)g_wav;
  size_t remaining = (size_t)got * sizeof(int16_t);
  while (remaining > 0) {
    const size_t k = remaining > 1024 ? 1024 : remaining;
    Serial.write(p, k);
    p += k;
    remaining -= k;
  }
  Serial.flush();
  Serial.println();
  Serial.println("END_PCM");
}

void loop() {
  delay(5000);
  Serial.println("idle - capture complete");
}
