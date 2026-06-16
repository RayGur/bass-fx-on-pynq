#include "effect_ip.h"

// Processing core: one stereo pair per call.
// No HLS interface pragmas — called from stream top function and testbench.
void process_sample_core(
    sample_t  in_l,        sample_t  in_r,
    sample_t *out_l,       sample_t *out_r,
    bool      dist_en,     bool      wobble_en,
    param_t   threshold,   param_t   gain,
    param_t   lfo_rate,    param_t   lfo_depth,
    param_t   lfo_floor,
    state_t  *state
) {
    sample_t sig_l = in_l;
    sample_t sig_r = in_r;

    // --- notch (14.7): always applied; 60 Hz notch, BW≈1.5 Hz ---
    sig_l = apply_notch(sig_l, /*is_l=*/true,  state);
    sig_r = apply_notch(sig_r, /*is_l=*/false, state);

    // --- HPF (14.7): DC offset removal, Fc≈28 Hz; effect-conditional ---
    // bypass (both off) stays true pass (HPF first-sample ≠ input, breaks exact check)
    if (dist_en || wobble_en) {
        sig_l = apply_hpf(sig_l, /*is_l=*/true,  state);
        sig_r = apply_hpf(sig_r, /*is_l=*/false, state);
    }

    // --- wobble (Phase 3) ---
    // Wobble before distortion (14.8 fix): wah tone-shapes → dist clips shaped signal.
    // Harmonics from clipping are NOT subsequently LP-filtered, preserving distortion
    // character at all LFO phases (prior dist→wobble: wobble LP removed clipping harmonics
    // during the trough, making attack sound clean with no distortion texture).
    if (wobble_en) {
        sig_l = apply_wobble(sig_l, lfo_rate, lfo_depth, lfo_floor, state, /*is_l=*/true);
        sig_r = apply_wobble(sig_r, lfo_rate, lfo_depth, lfo_floor, state, /*is_l=*/false);
    }

    // --- distortion (Phase 2, hysteresis noise gate inside apply_distortion) ---
    if (dist_en) {
        sig_l = apply_distortion(sig_l, threshold, gain, state, /*is_l=*/true);
        sig_r = apply_distortion(sig_r, threshold, gain, state, /*is_l=*/false);
    }

    *out_l = sig_l;
    *out_r = sig_r;
}

// HLS top function (Phase 6): AXI-Stream wrapper.
// Reads n_samples stereo pairs (L then R, interleaved) from s_in,
// processes each pair via process_sample_core(), writes results to s_out.
// TLAST is set on the last word. Cross-buffer state is static.
void process_sample(
    hls::stream<audio_pkt_t> &s_in,
    hls::stream<audio_pkt_t> &s_out,
    int     n_samples,
    bool    dist_en,    bool    wobble_en,
    param_t threshold,  param_t gain,
    param_t lfo_rate,   param_t lfo_depth,
    param_t lfo_floor
) {
#pragma HLS INTERFACE axis      port=s_in       depth=512
#pragma HLS INTERFACE axis      port=s_out      depth=512
#pragma HLS INTERFACE s_axilite port=n_samples  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=dist_en    bundle=ctrl
#pragma HLS INTERFACE s_axilite port=wobble_en  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=threshold  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=gain       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=lfo_rate   bundle=ctrl
#pragma HLS INTERFACE s_axilite port=lfo_depth  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=lfo_floor  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return     bundle=ctrl

    static state_t state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    // Loop over individual 32-bit words (n_samples*2 total: L0,R0,L1,R1,...).
    // One read + one write per iteration → achieves II=1.
    // Previous design (2 reads + 2 writes per iter) caused II=2, which made
    // the DMA S2MM skip every other word (R samples never written to DDR).
    // n_samples semantics unchanged: stereo pairs. PS still writes 256.
    for (int i = 0; i < n_samples * 2; i++) {
#pragma HLS PIPELINE II=1

        audio_pkt_t pkt = s_in.read();

        sample_t in_s, out_s;
        in_s.range(23, 0) = pkt.data.range(23, 0);

        bool is_l = (i % 2 == 0);

        // --- notch (14.7): 60 Hz biquad, always applied ---
        in_s = apply_notch(in_s, is_l, &state);

        // --- HPF (14.7): Fc≈28 Hz, only when an effect is enabled ---
        if (dist_en || wobble_en) {
            in_s = apply_hpf(in_s, is_l, &state);
        }

        // --- wobble (Phase 3) ---
        // Wobble before distortion (14.8 fix): wah LP tone-shapes first, then dist
        // clips the shaped signal and adds harmonics that are NOT subsequently filtered.
        // Prior order (dist→wobble) had wobble LP removing all clipping harmonics,
        // making attack sound clean when LFO was near its trough (fc≈83 Hz).
        if (wobble_en) {
            out_s = apply_wobble(in_s, lfo_rate, lfo_depth, lfo_floor, &state, is_l);
        } else {
            out_s = in_s;
        }

        // --- distortion (Phase 2, hysteresis noise gate inside apply_distortion) ---
        if (dist_en) {
            out_s = apply_distortion(out_s, threshold, gain, &state, is_l);
        }

        audio_pkt_t out_pkt;
        out_pkt.data = 0;
        out_pkt.data.range(23, 0) = out_s.range(23, 0);
        out_pkt.keep = ~0;
        out_pkt.strb = ~0;
        out_pkt.last = (i == n_samples * 2 - 1) ? 1 : 0;
        s_out.write(out_pkt);
    }
}
