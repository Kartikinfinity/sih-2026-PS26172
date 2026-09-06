// Auto-generated from model_mfcc/model_meta.json by tools/emit_params.py
// DO NOT EDIT. Hand-copying 26 floats is exactly how a silent
// train/inference mismatch gets introduced.
#pragma once

constexpr int KWS_N_FRAMES = 49;
constexpr int KWS_N_MFCC   = 13;
constexpr float KWS_IN_SCALE = 0.0454486459f;
constexpr int   KWS_IN_ZP    = 9;
constexpr float KWS_OUT_SCALE = 0.0830315799f;
constexpr int   KWS_OUT_ZP    = -16;

// Per-coefficient normalisation. A single global mean/std collapsed
// c2..c12 to std 0.04-0.08 and training degenerated to the majority
// class (test accuracy 0.3846, exactly the silence fraction).
constexpr float KWS_NORM_MEAN[13] = {
    100.02132416f, 11.01793480f, -0.65456498f, 3.62467003f, 0.43711516f, 0.85500222f, -0.70805985f, 0.48502991f, 0.46310714f, -0.06397094f, 0.63987660f, -0.54931015f, 0.56621581f
};
constexpr float KWS_NORM_STD[13] = {
    8.85087490f, 3.55068016f, 2.10035396f, 2.53812122f, 1.50011671f, 1.76147842f, 1.79658437f, 1.30191684f, 1.15091944f, 1.32954788f, 1.18973994f, 1.04542971f, 1.07859147f
};
