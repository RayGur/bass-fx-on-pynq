#ifndef EFFECT_IP_H
#define EFFECT_IP_H

#include "ap_fixed.h"

// --- Type definitions ---
// Audio sample: Q1.23, 24-bit, range [-1, +1)
// Matches ADAU1761 24-bit codec output
typedef ap_fixed<24, 1> sample_t;

// AXI-Lite parameter: PS writes an integer, IP interprets as fixed-point
typedef int param_t;

// Cross-sample state (placeholder for Phase 1)
// Phase 3: Claire adds LFO phase accumulator and IIR history here
typedef struct {
    int placeholder;  // do not modify until Phase 3
} state_t;

// --- Function declarations ---

// HLS top function (AXI-Lite interface, see process_sample.cpp)
void process_sample(
    sample_t  in_l,        sample_t  in_r,
    sample_t *out_l,       sample_t *out_r,
    bool      dist_en,     bool      wobble_en,
    param_t   threshold,   param_t   gain,
    param_t   lfo_rate,    param_t   lfo_depth,
    state_t  *state
);

// Distortion hard clipping (Phase 1: passthrough stub)
sample_t apply_distortion(sample_t in, param_t threshold, param_t gain);

// Wobble IIR + LFO (Phase 1: passthrough stub, Phase 3 filled by Claire)
sample_t apply_wobble(sample_t in, param_t lfo_rate, param_t lfo_depth, state_t *state);

#endif // EFFECT_IP_H
