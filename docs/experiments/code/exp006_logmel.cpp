// EXP-006 -- Phase 7 feature extraction: framing -> window -> FFT -> Mel -> log.
//
// EXP-005 showed a scalar energy detector cannot tell a keyword from a door
// slam: band-limiting improved the PEAK by 1.39x but left p90 and mean
// unchanged. One number per frame carries too little information. This builds
// the real front end, which emits a VECTOR describing spectral shape.
//
// Parameters follow the TFLite Micro micro_speech convention the plan cites:
//   16 kHz | 25 ms window (400) | 10 ms hop (160) | 512-pt FFT | 40 Mel bins
//   Mel range 125-7500 Hz. The 125 Hz lower edge also excludes the sub-audio
//   drift EXP-003 measured, so no extra high-pass is needed here.
//
// The FFT is a plain radix-2 implementation, NOT ESP-DSP. ESP-DSP is available
// (dsps_fft2r) and is the Phase 19 optimisation target -- but a measured
// baseline must exist first, or any later speedup claim is unfalsifiable.
//
// This experiment dumps BOTH the raw audio AND the computed features so the
// host can recompute from the same samples and verify parity. The plan section
// 4.2 requires feature extraction be identical on host and device; a silent
// mismatch there degrades the model in ways that are painful to debug later.

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "driver/i2s.h"
#include "esp_timer.h"

namespace {

constexpr int PIN_I2S_SCK = 6, PIN_I2S_WS = 5, PIN_I2S_SD = 4;
constexpr int SAMPLE_RATE = 16000;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

constexpr int FRAME_LEN = 400;   // 25 ms
constexpr int FRAME_HOP = 160;   // 10 ms
constexpr int FFT_N = 512;
constexpr int N_BINS = FFT_N / 2 + 1;  // 257
constexpr int N_MEL = 40;
constexpr float MEL_LO = 125.0f, MEL_HI = 7500.0f;

constexpr int CAPTURE_SECONDS = 3;
constexpr int CAPTURE_SAMPLES = SAMPLE_RATE * CAPTURE_SECONDS;   // 48,000
constexpr int N_FRAMES = (CAPTURE_SAMPLES - FRAME_LEN) / FRAME_HOP + 1;  // 298

int16_t *g_pcm = nullptr;
float *g_feat = nullptr;         // N_FRAMES x N_MEL
int32_t g_chunk[512];

float g_re[FFT_N], g_im[FFT_N];
float g_window[FRAME_LEN];
float g_melbank[N_MEL][N_BINS];  // triangular weights

// ---- radix-2 iterative FFT (decimation in time) -----------------------------
void fft(float *re, float *im, int n) {
  for (int i = 1, j = 0; i < n; ++i) {          // bit-reversal permutation
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const float ang = -2.0f * (float)M_PI / (float)len;
    const float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < len / 2; ++k) {
        const int a = i + k, b = i + k + len / 2;
        const float xr = re[b] * cr - im[b] * ci;
        const float xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr; im[b] = im[a] - xi;
        re[a] += xr;        im[a] += xi;
        const float nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
}

inline float hzToMel(float f) { return 2595.0f * log10f(1.0f + f / 700.0f); }
inline float melToHz(float m) { return 700.0f * (powf(10.0f, m / 2595.0f) - 1.0f); }

void buildTables() {
  // Periodic Hann window. A rectangular window would smear each tone across
  // the whole spectrum (spectral leakage); tapering the frame edges to zero
  // is what keeps the Mel energies meaningful.
  for (int i = 0; i < FRAME_LEN; ++i) {
    g_window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (float)FRAME_LEN);
  }
  // 40 triangular filters, evenly spaced on the Mel scale (not in Hz) so that
  // resolution follows human hearing: fine at low frequency, coarse at high.
  memset(g_melbank, 0, sizeof(g_melbank));
  const float ml = hzToMel(MEL_LO), mh = hzToMel(MEL_HI);
  float edge[N_MEL + 2];
  for (int i = 0; i < N_MEL + 2; ++i) {
    edge[i] = melToHz(ml + (mh - ml) * (float)i / (float)(N_MEL + 1));
  }
  const float bin_hz = (float)SAMPLE_RATE / (float)FFT_N;
  for (int m = 0; m < N_MEL; ++m) {
    const float lo = edge[m], mid = edge[m + 1], hi = edge[m + 2];
    for (int k = 0; k < N_BINS; ++k) {
      const float f = k * bin_hz;
      float w = 0.0f;
      if (f >= lo && f <= mid && mid > lo)      w = (f - lo) / (mid - lo);
      else if (f > mid && f <= hi && hi > mid)  w = (hi - f) / (hi - mid);
      g_melbank[m][k] = w;
    }
  }
}

// One frame -> N_MEL log-Mel values.
void computeFrame(const int16_t *src, float *out) {
  for (int i = 0; i < FRAME_LEN; ++i) {
    g_re[i] = (float)src[i] * g_window[i];
    g_im[i] = 0.0f;
  }
  for (int i = FRAME_LEN; i < FFT_N; ++i) { g_re[i] = 0.0f; g_im[i] = 0.0f; }

  fft(g_re, g_im, FFT_N);

  float power[N_BINS];
  for (int k = 0; k < N_BINS; ++k) {
    power[k] = g_re[k] * g_re[k] + g_im[k] * g_im[k];
  }
  for (int m = 0; m < N_MEL; ++m) {
    float acc = 0.0f;
    for (int k = 0; k < N_BINS; ++k) acc += g_melbank[m][k] * power[k];
    // log compresses a huge dynamic range into something a small network can
    // learn from, and matches how loudness is perceived.
    out[m] = logf(acc + 1e-6f);
  }
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
  Serial.println("================================================================");
  Serial.println(" EXP-006  LOG-MEL FEATURE EXTRACTION (host/device parity test)");
  Serial.printf("  %d Hz | win %d (25ms) | hop %d (10ms) | FFT %d | %d mel | %.0f-%.0f Hz\n",
                SAMPLE_RATE, FRAME_LEN, FRAME_HOP, FFT_N, N_MEL, MEL_LO, MEL_HI);
  Serial.printf("  frames = %d\n", N_FRAMES);
  Serial.println("================================================================");

  if (!i2sInit()) { Serial.println("I2S INIT FAILED"); while (true) delay(1000); }
  buildTables();

  g_pcm = (int16_t *)ps_malloc((size_t)CAPTURE_SAMPLES * sizeof(int16_t));
  g_feat = (float *)ps_malloc((size_t)N_FRAMES * N_MEL * sizeof(float));
  if (!g_pcm || !g_feat) { Serial.println("ps_malloc FAILED"); while (true) delay(1000); }

  {  // settle: discard the microphone startup transient
    const int64_t end = esp_timer_get_time() + 1500000;
    size_t br;
    while (esp_timer_get_time() < end)
      i2s_read(I2S_PORT, (void *)g_chunk, sizeof(g_chunk), &br, portMAX_DELAY);
  }

  Serial.printf("[ CAPTURE ] %d s -- SPEAK NOW\n", CAPTURE_SECONDS);
  long got = 0; size_t br;
  while (got < CAPTURE_SAMPLES) {
    const long want = min((long)512, (long)(CAPTURE_SAMPLES - got));
    i2s_read(I2S_PORT, (void *)g_chunk, (size_t)want * sizeof(int32_t), &br, portMAX_DELAY);
    const int n = (int)(br / sizeof(int32_t));
    for (int i = 0; i < n; ++i) g_pcm[got + i] = (int16_t)(g_chunk[i] >> 16);
    got += n;
  }
  Serial.printf("[ CAPTURE ] done, %ld samples\n", got);

  Serial.println("[ FEATURES ] computing...");
  const int64_t t0 = esp_timer_get_time();
  for (int f = 0; f < N_FRAMES; ++f) {
    computeFrame(&g_pcm[f * FRAME_HOP], &g_feat[f * N_MEL]);
  }
  const int64_t t1 = esp_timer_get_time();
  const double total_ms = (double)(t1 - t0) / 1000.0;
  Serial.printf("  %d frames in %.2f ms  =>  %.3f ms/frame\n", N_FRAMES, total_ms,
                total_ms / N_FRAMES);
  Serial.printf("  real-time budget is 10.000 ms/frame  =>  CPU load %.2f %%\n",
                100.0 * (total_ms / N_FRAMES) / 10.0);

  Serial.flush(); delay(200);
  Serial.printf("BEGIN_PCM %ld 2 %d\n", got, SAMPLE_RATE);
  Serial.flush();
  const uint8_t *p = (const uint8_t *)g_pcm;
  size_t rem = (size_t)got * sizeof(int16_t);
  while (rem) { size_t k = rem > 1024 ? 1024 : rem; Serial.write(p, k); p += k; rem -= k; }
  Serial.flush(); Serial.println(); Serial.println("END_PCM");

  delay(200);
  Serial.printf("BEGIN_FEAT %d %d 4\n", N_FRAMES, N_MEL);
  Serial.flush();
  p = (const uint8_t *)g_feat;
  rem = (size_t)N_FRAMES * N_MEL * sizeof(float);
  while (rem) { size_t k = rem > 1024 ? 1024 : rem; Serial.write(p, k); p += k; rem -= k; }
  Serial.flush(); Serial.println(); Serial.println("END_FEAT");
}

void loop() { delay(5000); Serial.println("idle - done"); }
