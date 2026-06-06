#include "effect_ip.h"

// HLS Top Function
// All control parameters are mapped to AXI-Lite bundle=ctrl.
// in_l / in_r / out_l / out_r use ap_none / ap_vld interfaces.
// state uses ap_memory so HLS manages cross-call state storage.
void process_sample(
    sample_t  in_l,        sample_t  in_r,
    sample_t *out_l,       sample_t *out_r,
    bool      dist_en,     bool      wobble_en,
    param_t   threshold,   param_t   gain,
    param_t   lfo_rate,    param_t   lfo_depth,
    state_t  *state
) {
#pragma HLS INTERFACE s_axilite   port=in_l       bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=in_r       bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=out_l      bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=out_r      bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=dist_en    bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=wobble_en  bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=threshold  bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=gain       bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=lfo_rate   bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=lfo_depth  bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=return     bundle=ctrl
#pragma HLS INTERFACE s_axilite   port=state      bundle=ctrl

    sample_t sig_l = in_l;
    sample_t sig_r = in_r;

    // Phase 1: passthrough, no effect calls.
    // Phase 2: uncomment the distortion block.
    // Phase 3: uncomment the wobble block.

    // --- distortion (uncomment in Phase 2) ---
    // if (dist_en) {
    //     sig_l = apply_distortion(sig_l, threshold, gain);
    //     sig_r = apply_distortion(sig_r, threshold, gain);
    // }

    // --- wobble (uncomment in Phase 3) ---
    // if (wobble_en) {
    //     sig_l = apply_wobble(sig_l, lfo_rate, lfo_depth, state);
    //     sig_r = apply_wobble(sig_r, lfo_rate, lfo_depth, state);
    // }

    *out_l = sig_l;
    *out_r = sig_r;
}
