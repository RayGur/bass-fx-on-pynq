#ifndef EFFECT_IP_H
#define EFFECT_IP_H

#include "ap_fixed.h"

// --- Type definitions ---
// Audio sample: Q1.23, 24-bit, range [-1, +1)
// Matches ADAU1761 24-bit codec output
typedef ap_fixed<24, 1> sample_t;

// AXI-Lite parameter: PS writes an integer, IP interprets as fixed-point
typedef int param_t;

// Cross-sample state (Phase 3)
// lfo_phase : LFO phase accumulator, wraps naturally at 2^32 (one full cycle)
// iir_prev  : IIR filter previous output y[n-1], same format as sample_t
typedef struct {
    ap_uint<32>  lfo_phase;
    sample_t     iir_prev;
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
