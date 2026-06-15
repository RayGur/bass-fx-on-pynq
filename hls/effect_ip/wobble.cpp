#include "effect_ip.h"

// B_LUT: 16-entry table of IIR coefficient b in Q15 format (b_raw / 32768 = b ∈ (0,1))
// Maps LFO sweep position to low-pass cutoff; higher index → higher b → faster response
// Computed via b = 1 - exp(-2π·fc/fs), fs=48000, log-spaced 10–2000 Hz (14.1 fix)
// fc(Hz): 10, 14, 20, 29, 41, 58, 83, 119, 169, 240, 342, 487, 694, 988, 1408, 2000
static const ap_uint<16> B_LUT[16] = {
      43,   61,   87,  124,  176,  250,  355,  505,
     717, 1015, 1436, 2027, 2847, 3982, 5528, 7569
};

// apply_wobble -- one-pole IIR low-pass with LFO-swept coefficient
//
// lfo_rate  : phase increment per stereo pair (PS writes integer; larger = faster sweep)
// lfo_depth : 0–100, maps LFO amplitude to B_LUT index range [0,15]
// state     : shared persistent state; L/R have separate iir_prev fields
// is_l      : true for L sample → advance LFO phase; false for R → hold phase
//
// Algorithm (Claire's Phase 3 design):
//   1. Advance lfo_phase only on L sample
//   2. Triangle LFO: direction bit selects up/down ramp; frac = phase[30:23]
//   3. depth_scaled = (wave * lfo_depth) / 100 → maps to LUT index (>> 4)
//   4. b = B_LUT[idx] / 32768  (Q15 → float in (0,1))
//   5. y = b * in + (1-b) * iir_prev  (IIR low-pass)
//   6. Clamp output to [-1.0, +0.9999]; store unclamped y back to state
sample_t apply_wobble(sample_t in, param_t lfo_rate, param_t lfo_depth,
                      state_t *state, bool is_l) {
#pragma HLS INLINE

    // --- LFO phase update (L sample only) ---
    if (is_l) {
        state->lfo_phase += (ap_uint<32>)lfo_rate;
    }

    // --- Triangle LFO → B_LUT index ---
    ap_uint<1>  direction    = state->lfo_phase[31];
    ap_uint<8>  frac         = (ap_uint<8>)((state->lfo_phase >> 23) & 0xFF);
    ap_uint<8>  wave         = direction ? (ap_uint<8>)(255 - frac) : frac;
    ap_uint<16> depth_prod   = (ap_uint<16>)(wave * (ap_uint<8>)lfo_depth);
    ap_uint<8>  depth_scaled = (ap_uint<8>)(depth_prod / 100);
    ap_uint<4>  lut_idx      = depth_scaled >> 4;

    ap_uint<16> b_raw = B_LUT[lut_idx];

    // --- Convert Q15 integer to fixed-point coefficient b ∈ (0,1) ---
    // Division by power-of-2 (32768 = 2^15); HLS optimises to right-shift
    ap_fixed<32, 2> b = ap_fixed<32, 17>(b_raw) / ap_fixed<32, 17>(32768);

    // --- Stage 1 IIR: y = b*in + (1-b)*prev ---
    ap_fixed<32, 2> prev;
    if (is_l) prev = state->iir_prev_L;
    else       prev = state->iir_prev_R;

    ap_fixed<32, 2> y = b * (ap_fixed<32, 2>)in
                      + (ap_fixed<32, 2>(1.0) - b) * prev;

    if (is_l) state->iir_prev_L = y;
    else       state->iir_prev_R = y;

    // --- Stage 2 IIR: y2 = b*y + (1-b)*prev2 (12 dB/oct cascade, 14.1) ---
    ap_fixed<32, 2> prev2;
    if (is_l) prev2 = state->iir_prev2_L;
    else       prev2 = state->iir_prev2_R;

    ap_fixed<32, 2> y2 = b * y + (ap_fixed<32, 2>(1.0) - b) * prev2;

    if (is_l) state->iir_prev2_L = y2;
    else       state->iir_prev2_R = y2;

    // --- Clamp final output ---
    sample_t out;
    if      (y2 >  ap_fixed<32, 2>(0.9999)) out = sample_t(0.9999);
    else if (y2 < ap_fixed<32, 2>(-1.0))    out = sample_t(-1.0);
    else                                     out = (sample_t)y2;

    return out;
}
