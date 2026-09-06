// EXP-007 -- Continuous PCM streamer for dataset collection.
//
// Phase 9 needs hundreds of recorded utterances. Rather than build a
// prompt-and-capture protocol into the firmware, this streams raw audio
// continuously and lets the host do all the work: prompting, segmenting,
// labelling, saving. That keeps the dataset workflow iterable without
// reflashing the board every time the collection strategy changes.
//
// Throughput check: 16 kHz x 2 bytes = 32,000 B/s. The USB-Serial/JTAG link
// runs far above that, so a continuous stream is comfortable.
//
// Audio is streamed RAW and UNFILTERED, converted 24-bit -> 16-bit by >> 8
// (>> 16 from the 32-bit I2S slot), exactly as EXP-006 fed its feature
// extractor. This matters: training data must be identical in character to
// what the device will feed the model at inference time. Any filtering applied
// here but not on-device would silently create a train/inference mismatch --
// the same class of bug EXP-006's parity test was built to rule out.
//
// Wiring (EXP-001): SCK=GPIO6, WS=GPIO5, SD=GPIO4, L/R=GND (left channel).

#include <Arduino.h>
#include <string.h>

#include "driver/i2s.h"
#include "esp_timer.h"

namespace {

constexpr int PIN_I2S_SCK = 6, PIN_I2S_WS = 5, PIN_I2S_SD = 4;
constexpr int SAMPLE_RATE = 16000;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

constexpr int CHUNK = 512;
int32_t g_chunk[CHUNK];
int16_t g_out[CHUNK];

bool i2sInit() {
  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 16;   // deeper queue: a host stall must not drop samples
  cfg.dma_buf_len = 256;
  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) return false;
  i2s_pin_config_t p;
  memset(&p, 0, sizeof(p));
  p.mck_io_num = I2S_PIN_NO_CHANGE;
  p.bck_io_num = PIN_I2S_SCK;
  p.ws_io_num = PIN_I2S_WS;
  p.data_out_num = I2S_PIN_NO_CHANGE;
  p.data_in_num = PIN_I2S_SD;
  if (i2s_set_pin(I2S_PORT, &p) != ESP_OK) return false;
  i2s_zero_dma_buffer(I2S_PORT);
  return true;
}

}  // namespace

void setup() {
  Serial.begin(921600);
  delay(800);
  Serial.println();
  Serial.println("EXP-007 CONTINUOUS PCM STREAM");

  if (!i2sInit()) {
    Serial.println("I2S INIT FAILED");
    while (true) delay(1000);
  }

  // Discard the microphone startup transient before the stream begins.
  {
    const int64_t end = esp_timer_get_time() + 1500000;
    size_t br;
    while (esp_timer_get_time() < end)
      i2s_read(I2S_PORT, (void *)g_chunk, sizeof(g_chunk), &br, portMAX_DELAY);
  }

  // Everything after this line is raw little-endian int16 samples.
  Serial.printf("BEGIN_STREAM %d 2\n", SAMPLE_RATE);
  Serial.flush();
}

void loop() {
  size_t br;
  i2s_read(I2S_PORT, (void *)g_chunk, sizeof(g_chunk), &br, portMAX_DELAY);
  const int n = (int)(br / sizeof(int32_t));
  for (int i = 0; i < n; ++i) g_out[i] = (int16_t)(g_chunk[i] >> 16);
  Serial.write((const uint8_t *)g_out, (size_t)n * sizeof(int16_t));
}
