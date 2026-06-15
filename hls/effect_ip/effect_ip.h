#ifndef EFFECT_IP_H
#define EFFECT_IP_H

#include "ap_fixed.h"
#include "ap_int.h"
#include "ap_axi_sdata.h"
#include "hls_stream.h"

// --- Type definitions ---
// Audio sample: Q1.23, 24-bit, range [-1, +1)
// Matches ADAU1761 24-bit codec output
typedef ap_fixed<24, 1> sample_t;

// AXI-Lite parameter: PS writes an integer, IP interprets as fixed-point
typedef int param_t;

// Cross-sample state (Phase 3)
// lfo_phase   : LFO phase accumulator, wraps naturally at 2^32 (one full cycle)
// iir_prev_L  : stage-1 IIR previous output, left channel  (unclamped ap_fixed<32,2>)
// iir_prev_R  : stage-1 IIR previous output, right channel
// iir_prev2_L : stage-2 IIR previous output, left channel  (2nd-order cascade, 14.1)
// iir_prev2_R : stage-2 IIR previous output, right channel
typedef struct {
    ap_uint<32>    lfo_phase;
    ap_fixed<32,2> iir_prev_L;
    ap_fixed<32,2> iir_prev_R;
    ap_fixed<32,2> iir_prev2_L;
    ap_fixed<32,2> iir_prev2_R;
} state_t;

// AXI-Stream packet: 32-bit data + TLAST
typedef ap_axis<32, 0, 0, 0> audio_pkt_t;

// --- Function declarations ---

// HLS top function (Phase 6): AXI-Stream interface.
// Processes n_samples stereo pairs (L/R interleaved) per invocation.
// TLAST is asserted on the last word of the transfer.
// Cross-buffer state (IIR/LFO) is held as static inside this function.
void process_sample(
    hls::stream<audio_pkt_t> &s_in,
    hls::stream<audio_pkt_t> &s_out,
    int     n_samples,
    bool    dist_en,    bool    wobble_en,
    param_t threshold,  param_t gain,
    param_t lfo_rate,   param_t lfo_depth,
    param_t lfo_floor
);

// Processing core — one stereo pair per call.
// Decoupled from the stream wrapper so the testbench can drive it directly.
void process_sample_core(
    sample_t  in_l,        sample_t  in_r,
    sample_t *out_l,       sample_t *out_r,
    bool      dist_en,     bool      wobble_en,
    param_t   threshold,   param_t   gain,
    param_t   lfo_rate,    param_t   lfo_depth,
    param_t   lfo_floor,
    state_t  *state
);

// Distortion hard clipping
sample_t apply_distortion(sample_t in, param_t threshold, param_t gain);

// Wobble IIR + LFO (Phase 3)
// is_l=true: advance LFO phase, use iir_prev_L
// is_l=false: hold LFO phase, use iir_prev_R
// lfo_floor: minimum B_LUT index (0–15); clamps sweep bottom for wah depth preset
sample_t apply_wobble(sample_t in, param_t lfo_rate, param_t lfo_depth,
                      param_t lfo_floor, state_t *state, bool is_l);

#endif // EFFECT_IP_H
