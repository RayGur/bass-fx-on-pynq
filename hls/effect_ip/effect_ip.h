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

// Cross-sample state (Phase 3 + 14.2/14.7/14.9)
// lfo_phase    : LFO phase accumulator, wraps naturally at 2^32 (one full cycle)
// iir_prev_L/R : stage-1 IIR previous output, left/right channel
// iir_prev2_L/R: stage-2 IIR previous output, left/right channel (2nd-order cascade, 14.1)
// hpf_x_prev_L/R : HPF input history x[n-1], left/right (Fc≈28 Hz, 14.7)
// hpf_y_prev_L/R : HPF output history y[n-1], left/right
// notch_x1/x2_L/R: notch filter x[n-1], x[n-2] per channel (60 Hz biquad, r=0.9997, 14.7)
// notch_y1/y2_L/R: notch filter y[n-1], y[n-2] per channel
// dist_gate_open_L/R: noise gate hysteresis state (14.9); true=gate open, false=muted
typedef struct {
    // --- Wobble (Phase 3) ---
    ap_uint<32>    lfo_phase;
    ap_fixed<32,2> iir_prev_L;
    ap_fixed<32,2> iir_prev_R;
    ap_fixed<32,2> iir_prev2_L;
    ap_fixed<32,2> iir_prev2_R;
    // --- HPF (14.7, Fc≈28 Hz @ 48 kHz) ---
    sample_t       hpf_x_prev_L;
    sample_t       hpf_x_prev_R;
    ap_fixed<32,2> hpf_y_prev_L;
    ap_fixed<32,2> hpf_y_prev_R;
    // --- 60 Hz Notch (14.7, Direct Form I, r=0.9997, BW≈4.6 Hz) ---
    sample_t       notch_x1_L;
    sample_t       notch_x1_R;
    sample_t       notch_x2_L;
    sample_t       notch_x2_R;
    ap_fixed<32,2> notch_y1_L;
    ap_fixed<32,2> notch_y1_R;
    ap_fixed<32,2> notch_y2_L;
    ap_fixed<32,2> notch_y2_R;
    // --- Noise gate hysteresis (14.9) ---
    bool           dist_gate_open_L;
    bool           dist_gate_open_R;
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

// Distortion hard clipping with hysteresis noise gate (14.2/14.9)
// Gate opens when |in| > 0.001; stays open until |in| < 0.0003 (prevents chatter on decay)
// is_l selects L/R gate state branch
sample_t apply_distortion(sample_t in, param_t threshold, param_t gain,
                          state_t *state, bool is_l);

// DC-blocking HPF (Fc≈28 Hz, 14.7); is_l selects L/R state branch
// Applied only when dist_en || wobble_en — bypass stays true pass
sample_t apply_hpf(sample_t in, bool is_l, state_t *state);

// 60 Hz notch filter (IIR biquad, r=0.9997, BW≈4.6 Hz, 14.7); always-on
// First-sample with zero state: y[0]=x[0] (passthrough test unaffected)
sample_t apply_notch(sample_t in, bool is_l, state_t *state);

// Wobble IIR + LFO (Phase 3)
// is_l=true: advance LFO phase, use iir_prev_L
// is_l=false: hold LFO phase, use iir_prev_R
// lfo_floor: minimum B_LUT index (0–15); clamps sweep bottom for wah depth preset
sample_t apply_wobble(sample_t in, param_t lfo_rate, param_t lfo_depth,
                      param_t lfo_floor, state_t *state, bool is_l);

#endif // EFFECT_IP_H
