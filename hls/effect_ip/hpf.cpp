#include "effect_ip.h"

// DC-blocking high-pass filter (1st-order IIR)
//
// Difference equation: y[n] = alpha * (y[n-1] + x[n] - x[n-1])
// alpha = 1 - 2*pi*Fc/Fs = 1 - 2*pi*28/48000 ≈ 0.9963  →  Fc ≈ 28 Hz
//
// Applied only when dist_en || wobble_en (caller's responsibility).
// True bypass path stays y = x.  State persists across DMA bursts via state_t.
//
// is_l=true  → operates on hpf_x_prev_L / hpf_y_prev_L
// is_l=false → operates on hpf_x_prev_R / hpf_y_prev_R
static const ap_fixed<18,1> HPF_ALPHA = 0.9963;

sample_t apply_hpf(sample_t in, bool is_l, state_t *state) {
    sample_t       x_prev = is_l ? state->hpf_x_prev_L : state->hpf_x_prev_R;
    ap_fixed<32,2> y_prev = is_l ? state->hpf_y_prev_L : state->hpf_y_prev_R;

    ap_fixed<32,2> y = HPF_ALPHA * (y_prev + (ap_fixed<32,2>)in - (ap_fixed<32,2>)x_prev);

    if (is_l) { state->hpf_x_prev_L = in; state->hpf_y_prev_L = y; }
    else       { state->hpf_x_prev_R = in; state->hpf_y_prev_R = y; }

    return (sample_t)y;
}
