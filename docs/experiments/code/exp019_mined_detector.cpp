// SENTINEL -- on-device keyword spotting: streaming MFCC + int8 DS-CNN.
//
// Pipeline, running continuously:
//   INMP441 -> I2S DMA -> sliding 25 ms frame -> ONE new log-Mel frame per
//   20 ms hop -> feature ring (49 frames) -> normalise -> quantise to int8
//   -> TFLite Micro DS-CNN -> class probabilities
//
// Features are computed INCREMENTALLY: one 25 ms frame per 20 ms hop, pushed
// into a rolling buffer. Recomputing all 49 frames on every inference would
// cost 49 x 1.11 ms = 54 ms at EXP-006's measured rate, on top of the 84 ms
// inference. Streaming costs 50 frames/s x 1.11 ms instead.
//
// Constants come from model_mined/model_meta.json via tools/emit_params.py
// (src/kws_params.h, generated) and MUST match training:
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

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "kws_model.h"
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

// --- model constants -------------------------------------------------------
// All generated into kws_params.h from model_mined/model_meta.json.
constexpr float IN_SCALE = KWS_IN_SCALE;
constexpr int IN_ZP = KWS_IN_ZP;
constexpr float OUT_SCALE = KWS_OUT_SCALE;
constexpr int OUT_ZP = KWS_OUT_ZP;

constexpr int INFER_EVERY_HOPS = 10;      // inference every 10 x 20 ms = 200 ms

// Operating point swept on the EXP-019 live captures, after hard-negative
// mining. Measured Pareto frontier -- a single point dominates everything:
//
//   thr 0.90  3-of-3  ->  11/11 detected (100 %%), 0.00 false/min
//
// This works now and did not before because the model became SUSTAINED on real
// keywords (39 of 186 windows above 0.90 during spoken keywords) while its
// false responses stayed brief spikes (9 windows above 0.90 in 40 s of
// non-keyword speech, never three consecutively). Requiring three consecutive
// high-confidence windows separates them; against the earlier models, whose
// false responses were equally sustained, the same rule bought nothing.
//
// CAVEAT: swept on the same 80 s used to validate it. Re-measured on fresh
// audio in EXP-019; see that record for the honest figure.
constexpr float DETECT_THRESHOLD = 0.90f;
constexpr int DETECT_M = 3;
constexpr int DETECT_N = 3;
constexpr int REFRACTORY_WINDOWS = 6;     // 6 x 200 ms = 1.2 s lockout

// EXP-012 measured 15,460 bytes actually used by this model. 48 KB leaves
// generous headroom without wasting internal SRAM.
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

uint8_t *g_arena = nullptr;
tflite::MicroInterpreter *g_interp = nullptr;
TfLiteTensor *g_input = nullptr;
TfLiteTensor *g_output = nullptr;

double g_feat_us = 0;
long g_feat_n = 0;

bool g_recent[DETECT_N] = {false};   // sliding history of threshold crossings
int g_recent_head = 0;
int g_lockout = 0;                   // windows remaining in the refractory period
long g_detections = 0;

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

void runInference() {
  int8_t *in = g_input->data.int8;
  int idx = 0;
  // Unroll the ring oldest-first so frame order matches training.
  for (int f = 0; f < N_FRAMES; ++f) {
    const float *row = g_feat[(g_feat_head + f) % N_FRAMES];
    for (int m = 0; m < N_MFCC; ++m) {
      const float norm = (row[m] - KWS_NORM_MEAN[m]) / KWS_NORM_STD[m];
      int q = (int)lroundf(norm / IN_SCALE) + IN_ZP;
      if (q > 127) q = 127;
      if (q < -128) q = -128;
      in[idx++] = (int8_t)q;
    }
  }

  const int64_t t0 = esp_timer_get_time();
  const TfLiteStatus st = g_interp->Invoke();
  const int64_t t1 = esp_timer_get_time();
  if (st != kTfLiteOk) { Serial.println("Invoke FAILED"); return; }

  float logits[3], mx = -1e30f;
  for (int i = 0; i < 3; ++i) {
    logits[i] = ((int)g_output->data.int8[i] - OUT_ZP) * OUT_SCALE;
    if (logits[i] > mx) mx = logits[i];
  }
  float sum = 0.0f, prob[3];
  for (int i = 0; i < 3; ++i) { prob[i] = expf(logits[i] - mx); sum += prob[i]; }
  for (int i = 0; i < 3; ++i) prob[i] /= sum;

  int best = 0;
  for (int i = 1; i < 3; ++i) if (prob[i] > prob[best]) best = i;

  // --- temporal smoothing (M-of-N vote + refractory) ---------------------
  // Fire when DETECT_M of the last DETECT_N windows cross the threshold, then
  // lock out for REFRACTORY_WINDOWS so one utterance cannot report twice. This
  // is the exact rule tools/eval_streaming.py measures, so device behaviour
  // matches the published numbers.
  //
  // It works because the two error types differ in time structure: a real
  // utterance spans several consecutive windows, while false windows are
  // largely isolated. EXP-015 measured N=1 -> N=2 removing every false
  // activation (4.00/min -> 0.00) at a cost of 18.4 points of detection.
  //
  // Deliberately NOT applied before EXP-014. Against the previous model's
  // ~50 % per-window false rate, consecutive false windows were common, so
  // smoothing would have improved the reported number while concealing the
  // defect rather than removing it.
  g_recent[g_recent_head] = (prob[0] >= DETECT_THRESHOLD);
  g_recent_head = (g_recent_head + 1) % DETECT_N;
  int votes = 0;
  for (int i = 0; i < DETECT_N; ++i) if (g_recent[i]) votes++;

  bool fired = false;
  if (g_lockout > 0) {
    g_lockout--;
  } else if (votes >= DETECT_M) {
    fired = true;
    g_detections++;
    g_lockout = REFRACTORY_WINDOWS;
    for (int i = 0; i < DETECT_N; ++i) g_recent[i] = false;  // start clean
  }

  const double feat_ms = g_feat_us / 1000.0 / (double)(g_feat_n > 0 ? g_feat_n : 1);
  const double inf_ms = (double)(t1 - t0) / 1000.0;
  // Hop is FRAME_HOP samples, not a hardcoded 10 ms. This arithmetic was left
  // stale when the hop changed 10 -> 20 ms in EXP-011 and briefly reported
  // 95.9 %% CPU when the true figure was ~48 %%.
  constexpr double kHopMs = 1000.0 * FRAME_HOP / SAMPLE_RATE;
  const double cpu = 100.0 * (feat_ms / kHopMs + inf_ms / (kHopMs * INFER_EVERY_HOPS));

  Serial.printf("kw %.3f  unk %.3f  sil %.3f  -> %-7s | vote %d | feat %.2f ms  infer %.2f ms  cpu %.1f%%%s\n",
                prob[0], prob[1], prob[2], kClassNames[best], votes, feat_ms,
                inf_ms, cpu,
                fired ? "   *** SENTINEL ***" : "");
}

}  // namespace

void setup() {
  // Onboard RGB LED: hold it OFF.
  //
  // The DevKitC-1 carries a single WS2812 addressable LED on GPIO48 (the
  // esp32s3 variant's PIN_NEOPIXEL; RGB_BUILTIN is that pin tagged with
  // SOC_GPIO_PIN_COUNT so the core routes it to the RMT driver). This firmware
  // never drives it, which is exactly the problem: an unwritten WS2812 keeps
  // whatever its shift register latched, so a glitch on the floating data line
  // during boot or flashing leaves it lit at an arbitrary colour.
  //
  // One zero frame clears it deterministically. neopixelWrite() then leaves
  // GPIO48 bound to the RMT peripheral idling low, so no stray edge can latch
  // a new colour afterwards -- and this is the firmware's only write to it.
  // From the Arduino core (esp32-hal-rgb-led.h, via Arduino.h); no new library.
#ifdef RGB_BUILTIN
  neopixelWrite(RGB_BUILTIN, 0, 0, 0);
#endif

  Serial.begin(921600);
  delay(800);
  Serial.println();
  Serial.println("================================================================");
  Serial.println(" SENTINEL  on-device keyword spotting (MFCC + int8 DS-CNN)");
  Serial.printf("  %d frames x %d MFCC | hop %d ms | inference every %d ms\n",
                N_FRAMES, N_MFCC, FRAME_HOP * 1000 / SAMPLE_RATE,
                INFER_EVERY_HOPS * FRAME_HOP * 1000 / SAMPLE_RATE);
  Serial.printf("  threshold %.2f | %d-of-%d vote | %d-window refractory\n",
                DETECT_THRESHOLD, DETECT_M, DETECT_N, REFRACTORY_WINDOWS);
  Serial.println("================================================================");

  if (!i2sInit()) { Serial.println("I2S INIT FAILED"); while (true) delay(1000); }
  buildTables();

  g_arena = (uint8_t *)heap_caps_malloc(kArenaSize, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (!g_arena) { Serial.println("arena alloc FAILED"); while (true) delay(1000); }

  const tflite::Model *model = tflite::GetModel(g_kws_model);
  static tflite::MicroMutableOpResolver<5> resolver;
  resolver.AddConv2D();
  resolver.AddDepthwiseConv2D();
  resolver.AddFullyConnected();
  resolver.AddMean();
  resolver.AddReshape();
  static tflite::MicroInterpreter interp(model, resolver, g_arena, kArenaSize);
  if (interp.AllocateTensors() != kTfLiteOk) {
    Serial.println("AllocateTensors FAILED");
    while (true) delay(1000);
  }
  g_interp = &interp;
  g_input = interp.input(0);
  g_output = interp.output(0);
  Serial.printf("[ MODEL ] %u bytes | arena used %u of %d | heap free %u\n",
                g_kws_model_len, (unsigned)interp.arena_used_bytes(), kArenaSize,
                (unsigned)ESP.getFreeHeap());

  // Discard the microphone startup transient before listening. EXP-003 measured
  // it at up to 30 % of full scale against a settled floor 45x lower.
  {
    const int64_t end = esp_timer_get_time() + 1500000;
    size_t br;
    while (esp_timer_get_time() < end)
      i2s_read(I2S_PORT, (void *)g_chunk, sizeof(g_chunk), &br, portMAX_DELAY);
  }

  Serial.println("[ LISTENING ] say the keyword");
}

void loop() {
  size_t br;
  i2s_read(I2S_PORT, (void *)g_chunk, sizeof(g_chunk), &br, portMAX_DELAY);
  const int n = (int)(br / sizeof(int32_t));

  // Slide the 25 ms analysis frame forward by one 10 ms hop.
  memmove(g_audio, g_audio + n, (FRAME_LEN - n) * sizeof(int16_t));
  for (int i = 0; i < n; ++i) g_audio[FRAME_LEN - n + i] = (int16_t)(g_chunk[i] >> 16);
  if (g_audio_fill < FRAME_LEN) { g_audio_fill += n; return; }

  const int64_t t0 = esp_timer_get_time();
  computeFrameInto(g_feat[g_feat_head]);
  g_feat_us += (double)(esp_timer_get_time() - t0);
  g_feat_n++;
  g_feat_head = (g_feat_head + 1) % N_FRAMES;
  g_frames_seen++;

  if (g_frames_seen >= N_FRAMES && (g_frames_seen % INFER_EVERY_HOPS) == 0) {
    runInference();
  }
}
