// EXP-013 -- MFCC host/device parity re-verification.
//
// EXP-006 proved parity for the 98 x 40 log-Mel pipeline (max abs diff
// 0.000427). EXP-011/012 then changed the feature definition on BOTH sides --
// hop 10 -> 20 ms, an orthonormal DCT added, 40 log-Mel -> 13 MFCC -- and the
// parity test was NOT re-run, despite EXP-011 recording that it must be.
//
// The live test then showed 57 %% of inferences predicting "keyword" and
// 34.9 %% firing, against an offline false-fire of 10.7 %%. A feature mismatch
// produces exactly that: well-formed, confident, wrong.
//
// The feature code below is TRANSFORMED FROM the EXP-012 detector source, not
// retyped, so the code path under test is the same one the detector uses.
//
// Pipeline, running continuously:
//   INMP441 -> I2S DMA -> sliding 25 ms frame -> ONE new log-Mel frame per
//   10 ms hop -> feature ring (98 frames) -> normalise -> quantise to int8
//   -> TFLite Micro DS-CNN -> class probabilities
//
// Features are computed INCREMENTALLY: one 25 ms frame per 10 ms hop, pushed
// into a rolling buffer. Recomputing all 98 frames per inference would cost
// 98 x 1.107 ms = 108 ms at EXP-006's measured rate and could never keep up.
// Streaming costs 100 frames/s x 1.107 ms instead.
//
// Constants below come from model/model_meta.json and MUST match training:
//   log-Mel -> (x - mean) / std -> round(x / input_scale) + input_zero_point
// Applying either wrongly degrades accuracy silently while everything still
// runs -- the failure class EXP-006's parity test exists to prevent.
//
// Wiring (EXP-001): SCK=GPIO6, WS=GPIO5, SD=GPIO4, L/R=GND.

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "driver/i2s.h"
#include "esp_timer.h"


#include "kws_params.h"

namespace {

constexpr int PIN_I2S_SCK = 6, PIN_I2S_WS = 5, PIN_I2S_SD = 4;
constexpr int SAMPLE_RATE = 16000;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

// --- feature pipeline (must mirror tools/features.py exactly) ---------------
constexpr int FRAME_LEN = 400;      // 25 ms
constexpr int FRAME_HOP = 320;      // 20 ms (EXP-011: 98 -> 49 frames)
constexpr int FFT_N = 512;
constexpr int N_BINS = FFT_N / 2 + 1;
constexpr int N_MEL = 40;           // filterbank size (internal)
constexpr int N_MFCC = KWS_N_MFCC;  // model input width after the DCT
constexpr int N_FRAMES = KWS_N_FRAMES;  // 1.0 s context
constexpr float MEL_LO = 125.0f, MEL_HI = 7500.0f;

// --- model constants (model/model_meta.json) --------------------------------
// All from kws_params.h, generated from model_meta.json.
constexpr float IN_SCALE = KWS_IN_SCALE;
constexpr int IN_ZP = KWS_IN_ZP;
constexpr float OUT_SCALE = KWS_OUT_SCALE;
constexpr int OUT_ZP = KWS_OUT_ZP;

constexpr int INFER_EVERY_HOPS = 10;      // inference every 100 ms
constexpr float DETECT_THRESHOLD = 0.90f; // see EXP-009 miss/false-fire table

// The EXP-010 probe measured 132,012 bytes actually used.
constexpr int kArenaSize = 48 * 1024;

const char *kClassNames[3] = {"keyword", "unknown", "silence"};

int32_t g_chunk[FRAME_HOP];
int16_t g_audio[FRAME_LEN];         // sliding 25 ms analysis frame
int g_audio_fill = 0;

float g_re[FFT_N], g_im[FFT_N];
float g_window[FRAME_LEN];
float g_melbank[N_MEL][N_BINS];

float g_feat[N_FRAMES][N_MFCC];     // rolling MFCC buffer
float g_dct[N_MFCC][N_MEL];         // orthonormal DCT-II, N_MEL -> N_MFCC
int g_feat_head = 0;
long g_frames_seen = 0;

double g_feat_us = 0;
long g_feat_n = 0;

void fft(float *re, float *im, int n) {
  for (int i = 1, j = 0; i < n; ++i) {
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
  for (int i = 0; i < FRAME_LEN; ++i)
    g_window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (float)FRAME_LEN);
  memset(g_melbank, 0, sizeof(g_melbank));
  const float ml = hzToMel(MEL_LO), mh = hzToMel(MEL_HI);
  float edge[N_MEL + 2];
  for (int i = 0; i < N_MEL + 2; ++i)
    edge[i] = melToHz(ml + (mh - ml) * (float)i / (float)(N_MEL + 1));
  const float bin_hz = (float)SAMPLE_RATE / (float)FFT_N;
  for (int m = 0; m < N_MEL; ++m) {
    const float lo = edge[m], mid = edge[m + 1], hi = edge[m + 2];
    for (int k = 0; k < N_BINS; ++k) {
      const float f = k * bin_hz;
      float w = 0.0f;
      if (f >= lo && f <= mid && mid > lo)     w = (f - lo) / (mid - lo);
      else if (f > mid && f <= hi && hi > mid) w = (hi - f) / (hi - mid);
      g_melbank[m][k] = w;
    }
  }
  // Orthonormal DCT-II, matching tools/features.py::_build_dct exactly.
  for (int k = 0; k < N_MFCC; ++k) {
    const float sc = (k == 0) ? sqrtf(1.0f / N_MEL) : sqrtf(2.0f / N_MEL);
    for (int n = 0; n < N_MEL; ++n)
      g_dct[k][n] = sc * cosf((float)M_PI * k * (2 * n + 1) / (2.0f * N_MEL));
  }
}

void computeFrameInto(float *out) {
  for (int i = 0; i < FRAME_LEN; ++i) {
    g_re[i] = (float)g_audio[i] * g_window[i];
    g_im[i] = 0.0f;
  }
  for (int i = FRAME_LEN; i < FFT_N; ++i) { g_re[i] = 0.0f; g_im[i] = 0.0f; }
  fft(g_re, g_im, FFT_N);
  float power[N_BINS];
  for (int k = 0; k < N_BINS; ++k) power[k] = g_re[k] * g_re[k] + g_im[k] * g_im[k];
  float logmel[N_MEL];
  for (int m = 0; m < N_MEL; ++m) {
    float acc = 0.0f;
    for (int k = 0; k < N_BINS; ++k) acc += g_melbank[m][k] * power[k];
    logmel[m] = logf(acc + 1e-6f);
  }
  for (int c = 0; c < N_MFCC; ++c) {
    float acc = 0.0f;
    for (int m = 0; m < N_MEL; ++m) acc += g_dct[c][m] * logmel[m];
    out[c] = acc;
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
  cfg.dma_buf_count = 16;
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


constexpr int CAPTURE_SECONDS = 3;
constexpr int CAPTURE_SAMPLES = SAMPLE_RATE * CAPTURE_SECONDS;
constexpr int DUMP_FRAMES = (CAPTURE_SAMPLES - FRAME_LEN) / FRAME_HOP + 1;
int16_t *g_pcm = nullptr;
float *g_dump = nullptr;

}  // namespace

void setup() {
  Serial.begin(921600);
  delay(800);
  Serial.println();
  Serial.println("EXP-013 MFCC HOST/DEVICE PARITY");
  Serial.printf("  hop %d | frames %d | mfcc %d\n", FRAME_HOP, DUMP_FRAMES, N_MFCC);

  if (!i2sInit()) { Serial.println("I2S INIT FAILED"); while (true) delay(1000); }
  buildTables();

  g_pcm = (int16_t *)ps_malloc((size_t)CAPTURE_SAMPLES * sizeof(int16_t));
  g_dump = (float *)ps_malloc((size_t)DUMP_FRAMES * N_MFCC * sizeof(float));
  if (!g_pcm || !g_dump) { Serial.println("ps_malloc FAILED"); while (true) delay(1000); }

  { const int64_t end = esp_timer_get_time() + 1500000; size_t br;
    while (esp_timer_get_time() < end)
      i2s_read(I2S_PORT, (void *)g_chunk, sizeof(g_chunk), &br, portMAX_DELAY); }

  Serial.println("[ CAPTURE ] 3 s -- content does not matter");
  long got = 0; size_t br;
  while (got < CAPTURE_SAMPLES) {
    const long want = min((long)FRAME_HOP, (long)(CAPTURE_SAMPLES - got));
    i2s_read(I2S_PORT, (void *)g_chunk, (size_t)want * sizeof(int32_t), &br, portMAX_DELAY);
    const int n = (int)(br / sizeof(int32_t));
    for (int i = 0; i < n; ++i) g_pcm[got + i] = (int16_t)(g_chunk[i] >> 16);
    got += n;
  }

  // Same computeFrameInto() the detector calls, fed via the same g_audio buffer.
  for (int f = 0; f < DUMP_FRAMES; ++f) {
    memcpy(g_audio, &g_pcm[f * FRAME_HOP], FRAME_LEN * sizeof(int16_t));
    computeFrameInto(&g_dump[f * N_MFCC]);
  }
  Serial.printf("[ DONE ] %ld samples, %d frames\n", got, DUMP_FRAMES);

  Serial.flush(); delay(200);
  Serial.printf("BEGIN_PCM %ld 2 %d\n", got, SAMPLE_RATE);
  Serial.flush();
  const uint8_t *p = (const uint8_t *)g_pcm;
  size_t rem = (size_t)got * sizeof(int16_t);
  while (rem) { size_t k = rem > 1024 ? 1024 : rem; Serial.write(p, k); p += k; rem -= k; }
  Serial.flush(); Serial.println(); Serial.println("END_PCM");

  delay(200);
  Serial.printf("BEGIN_FEAT %d %d 4\n", DUMP_FRAMES, N_MFCC);
  Serial.flush();
  p = (const uint8_t *)g_dump;
  rem = (size_t)DUMP_FRAMES * N_MFCC * sizeof(float);
  while (rem) { size_t k = rem > 1024 ? 1024 : rem; Serial.write(p, k); p += k; rem -= k; }
  Serial.flush(); Serial.println(); Serial.println("END_FEAT");
}

void loop() { delay(5000); Serial.println("idle - parity dump complete"); }
