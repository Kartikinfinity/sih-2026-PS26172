// EXP-005 -- Band-limited (300-3400 Hz) energy detector.
//
// EXP-004 proved broadband RMS is the wrong instrument: it integrates over
// bands where the signal is not. Measured there:
//
//     band            quiet     speech
//     0-100 Hz        41.4%      2.3%
//     300-3400 Hz     22.3%     43.8%
//
// ...yet broadband peak-to-floor stayed stuck at 9.6x and failed its 10x bar.
// So this experiment measures energy INSIDE the speech band instead.
//
// Prediction to falsify: band-limited speech-to-floor >= 10x.
// From EXP-004's numbers the expected value is ~13x -- better, but NOT a
// dramatic leap. If that is all band-limiting buys, it is evidence that
// reliable keyword detection needs the full feature pipeline plus temporal
// smoothing, not merely a better level meter.
//
// Two independent filter chains run per sample:
//   A. 100 Hz high-pass  -> buffered for the WAV (wideband, listenable)
//   B. 300-3400 bandpass -> the detector metric
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

constexpr int CHUNK = 512;  // 32 ms
int32_t g_chunk[CHUNK];

constexpr int CAPTURE_SECONDS = 20;
constexpr long CAPTURE_FRAMES = (long)SAMPLE_RATE * CAPTURE_SECONDS;
int16_t *g_wav = nullptr;

// ---- one-pole filters -------------------------------------------------------
// High-pass: y = a*(y_prev + x - x_prev),      a = RC/(RC+dt)
// Low-pass:  y = y_prev + b*(x - y_prev),      b = dt/(RC+dt)
struct HP1 {
  float a, xp, yp;
  void init(float fc, float fs) {
    const float rc = 1.0f / (2.0f * (float)M_PI * fc);
    const float dt = 1.0f / fs;
    a = rc / (rc + dt);
    xp = yp = 0.0f;
  }
  inline float step(float x) {
    const float y = a * (yp + x - xp);
    xp = x;
    yp = y;
    return y;
  }
};

struct LP1 {
  float b, yp;
  void init(float fc, float fs) {
    const float rc = 1.0f / (2.0f * (float)M_PI * fc);
    const float dt = 1.0f / fs;
    b = dt / (rc + dt);
    yp = 0.0f;
  }
  inline float step(float x) {
    yp += b * (x - yp);
    return yp;
  }
};

// Chain A: wideband, for listening.
HP1 g_a1, g_a2;
// Chain B: speech band, for detection. 12 dB/octave on each edge.
HP1 g_b1, g_b2;
LP1 g_b3, g_b4;

constexpr float WIDE_FC = 100.0f;
constexpr float BAND_LO = 300.0f;
constexpr float BAND_HI = 3400.0f;

inline float chainA(float x) { return g_a2.step(g_a1.step(x)); }
inline float chainB(float x) {
  return g_b4.step(g_b3.step(g_b2.step(g_b1.step(x))));
}

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
  // Prime both chains so their startup transients are not recorded as signal.
  for (int i = 0; i < CHUNK; ++i) {
    const float v = (float)(g_chunk[i] >> 8);
    chainA(v);
    chainB(v);
  }
}

}  // namespace

void setup() {
  Serial.begin(921600);
  delay(800);

  Serial.println();
  Serial.println("================================================================");
  Serial.println(" EXP-005  BAND-LIMITED (300-3400 Hz) ENERGY DETECTOR");
  Serial.printf("  chain A: %.0f Hz high-pass      -> WAV\n", WIDE_FC);
  Serial.printf("  chain B: %.0f-%.0f Hz bandpass -> detector\n", BAND_LO, BAND_HI);
  Serial.println("  prediction: band speech/floor >= 10x");
  Serial.println("================================================================");

  if (!i2sInit()) {
    Serial.println("I2S INIT FAILED");
    while (true) delay(1000);
  }

  g_a1.init(WIDE_FC, SAMPLE_RATE);
  g_a2.init(WIDE_FC, SAMPLE_RATE);
  g_b1.init(BAND_LO, SAMPLE_RATE);
  g_b2.init(BAND_LO, SAMPLE_RATE);
  g_b3.init(BAND_HI, SAMPLE_RATE);
  g_b4.init(BAND_HI, SAMPLE_RATE);

  g_wav = (int16_t *)ps_malloc((size_t)CAPTURE_FRAMES * sizeof(int16_t));
  if (g_wav == nullptr) {
    Serial.println("ps_malloc FAILED");
    while (true) delay(1000);
  }

  Serial.println("[ SETTLE ] 1500 ms (both chains primed)");
  settle(1500);

  Serial.printf("[ CAPTURE ] %d s -- START YOUR SEQUENCE NOW\n", CAPTURE_SECONDS);
  Serial.println("t(s)    broadband     wide100       band     band/wide");

  long got = 0;
  int block = 0;
  size_t br;
  const int64_t t0 = esp_timer_get_time();

  while (got < CAPTURE_FRAMES) {
    const long want = min((long)CHUNK, CAPTURE_FRAMES - got);
    i2s_read(I2S_PORT, (void *)g_chunk, (size_t)want * sizeof(int32_t), &br,
             portMAX_DELAY);
    const int n = (int)(br / sizeof(int32_t));

    double sum = 0.0, sumsq = 0.0, asq = 0.0, bsq = 0.0;
    for (int i = 0; i < n; ++i) {
      const float v = (float)(g_chunk[i] >> 8);
      sum += v;
      sumsq += (double)v * v;

      const float a = chainA(v);
      asq += (double)a * a;

      const float b = chainB(v);
      bsq += (double)b * b;

      int32_t s16 = (int32_t)(a / 256.0f);
      if (s16 > 32767) s16 = 32767;
      if (s16 < -32768) s16 = -32768;
      g_wav[got + i] = (int16_t)s16;
    }

    const double mean = sum / n;
    const double broad = sqrt(fmax(sumsq / n - mean * mean, 0.0));
    const double wide = sqrt(asq / n);
    const double band = sqrt(bsq / n);

    got += n;
    if ((block++ % 8) == 0) {
      Serial.printf("%5.1f %12.0f %11.0f %10.0f %11.3f\n",
                    (double)(esp_timer_get_time() - t0) / 1e6, broad, wide, band,
                    wide > 1.0 ? band / wide : 0.0);
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
