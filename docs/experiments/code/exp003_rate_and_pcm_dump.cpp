// EXP-003 -- Sample-rate verification + raw PCM dump for offline analysis.
//
// Two objectives, neither of which needs any action from the user:
//
//   1. MEASURE the true sample rate. 16 kHz is what we configured; it has never
//      been verified. Everything downstream (filters, Mel spectrogram, the KWS
//      model itself) is silently wrong if this number is wrong.
//
//   2. CAPTURE 5 s of raw samples into PSRAM and stream them to the PC, so the
//      waveform and spectrum can be examined offline. EXP-002B showed the noise
//      floor may be dominated by low-frequency energy rather than true noise;
//      only a spectrum can settle that.
//
// Wiring (EXP-001): SCK=GPIO6, WS=GPIO5, SD=GPIO4, L/R=GND (left channel).

#include <Arduino.h>
#include <string.h>

#include "driver/i2s.h"
#include "esp_timer.h"

namespace {

constexpr int PIN_I2S_SCK = 6;
constexpr int PIN_I2S_WS = 5;
constexpr int PIN_I2S_SD = 4;

constexpr int SAMPLE_RATE = 16000;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

constexpr int CHUNK_FRAMES = 512;
int32_t g_chunk[CHUNK_FRAMES];

constexpr int CAPTURE_SECONDS = 5;
constexpr int CAPTURE_FRAMES = SAMPLE_RATE * CAPTURE_SECONDS;  // 80,000

int32_t *g_pcm = nullptr;  // lives in PSRAM

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

// Read and discard, to let the microphone settle past its startup transient.
void settle(int ms) {
  const int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
  size_t br;
  while (esp_timer_get_time() < end) {
    i2s_read(I2S_PORT, (void *)g_chunk, sizeof(g_chunk), &br, portMAX_DELAY);
  }
}

// The measurement that matters: how many frames does the DMA actually deliver
// per second of wall-clock time? This is the hardware's real rate, independent
// of what we asked for.
double measureSampleRate(int seconds) {
  size_t br;
  long frames = 0;
  const long target = (long)SAMPLE_RATE * seconds;

  const int64_t t0 = esp_timer_get_time();
  while (frames < target) {
    i2s_read(I2S_PORT, (void *)g_chunk, sizeof(g_chunk), &br, portMAX_DELAY);
    frames += (long)(br / sizeof(int32_t));
  }
  const int64_t t1 = esp_timer_get_time();

  const double elapsed_s = (double)(t1 - t0) / 1e6;
  Serial.printf("  frames=%ld  elapsed=%.4f s\n", frames, elapsed_s);
  return (double)frames / elapsed_s;
}

}  // namespace

void setup() {
  Serial.begin(921600);
  delay(800);

  Serial.println();
  Serial.println("================================================================");
  Serial.println(" EXP-003  SAMPLE RATE VERIFICATION + RAW PCM DUMP");
  Serial.println("================================================================");

  if (!i2sInit()) {
    Serial.println("I2S INIT FAILED");
    while (true) delay(1000);
  }
  Serial.println("[ I2S ] initialised");

  Serial.println("[ SETTLE ] discarding 1500 ms");
  settle(1500);

  Serial.println("[ SAMPLE RATE ] measuring over 5 s of real capture...");
  const double measured = measureSampleRate(5);
  const double err_pct = 100.0 * (measured - SAMPLE_RATE) / SAMPLE_RATE;
  Serial.printf("  configured = %d Hz\n", SAMPLE_RATE);
  Serial.printf("  MEASURED   = %.2f Hz   (error %+.3f %%)\n", measured, err_pct);

  g_pcm = (int32_t *)ps_malloc((size_t)CAPTURE_FRAMES * sizeof(int32_t));
  if (g_pcm == nullptr) {
    Serial.println("  ps_malloc FAILED - cannot capture");
    while (true) delay(1000);
  }
  Serial.printf("[ PSRAM ] allocated %u bytes for %d frames\n",
                (unsigned)(CAPTURE_FRAMES * sizeof(int32_t)), CAPTURE_FRAMES);

  Serial.printf("[ CAPTURE ] %d s ...\n", CAPTURE_SECONDS);
  long got = 0;
  size_t br;
  while (got < CAPTURE_FRAMES) {
    const long want = min((long)CHUNK_FRAMES, (long)(CAPTURE_FRAMES - got));
    i2s_read(I2S_PORT, (void *)g_chunk, (size_t)want * sizeof(int32_t), &br,
             portMAX_DELAY);
    const long n = (long)(br / sizeof(int32_t));
    memcpy(&g_pcm[got], g_chunk, (size_t)n * sizeof(int32_t));
    got += n;
  }
  Serial.printf("[ CAPTURE ] done, %ld frames\n", got);

  // Binary handshake. Python reads the header line, then exactly this many bytes.
  Serial.flush();
  delay(200);
  Serial.printf("BEGIN_PCM %ld 4 %.2f\n", got, measured);
  Serial.flush();

  const uint8_t *p = (const uint8_t *)g_pcm;
  size_t remaining = (size_t)got * sizeof(int32_t);
  while (remaining > 0) {
    const size_t n = remaining > 1024 ? 1024 : remaining;
    Serial.write(p, n);
    p += n;
    remaining -= n;
  }
  Serial.flush();
  Serial.println();
  Serial.println("END_PCM");
}

void loop() {
  delay(5000);
  Serial.println("idle - capture complete");
}
