#include "effect_ip.h"

// Processing core: one stereo pair per call.
// No HLS interface pragmas — called from stream top function and testbench.
void process_sample_core(
    sample_t  in_l,        sample_t  in_r,
    sample_t *out_l,       sample_t *out_r,
    bool      dist_en,     bool      wobble_en,
    param_t   threshold,   param_t   gain,
    param_t   lfo_rate,    param_t   lfo_depth,
    state_t  *state
) {
    sample_t sig_l = in_l;
    sample_t sig_r = in_r;

    // --- distortion (Phase 2) ---
    if (dist_en) {
        sig_l = apply_distortion(sig_l, threshold, gain);
        sig_r = apply_distortion(sig_r, threshold, gain);
    }

    // --- wobble (uncomment in Phase 3) ---
    // if (wobble_en) {
    //     sig_l = apply_wobble(sig_l, lfo_rate, lfo_depth, state);
    //     sig_r = apply_wobble(sig_r, lfo_rate, lfo_depth, state);
    // }

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
    param_t lfo_rate,   param_t lfo_depth
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
#pragma HLS INTERFACE s_axilite port=return     bundle=ctrl

    static state_t state = {0, sample_t(0)};

    for (int i = 0; i < n_samples; i++) {
#pragma HLS PIPELINE II=1

        audio_pkt_t pkt_l = s_in.read();
        audio_pkt_t pkt_r = s_in.read();

        sample_t in_l, in_r, out_l, out_r;
        in_l.range(23, 0) = pkt_l.data.range(23, 0);
        in_r.range(23, 0) = pkt_r.data.range(23, 0);

        process_sample_core(in_l, in_r, &out_l, &out_r,
                            dist_en, wobble_en,
                            threshold, gain,
                            lfo_rate, lfo_depth,
                            &state);

        bool is_last = (i == n_samples - 1);

        audio_pkt_t out_l_pkt, out_r_pkt;
        out_l_pkt.data = 0;
        out_l_pkt.data.range(23, 0) = out_l.range(23, 0);  // bit-copy, not numeric cast
        out_l_pkt.keep = ~0;   // all byte enables valid; TKEEP=0 → WSTRB=0 → DMA no-write
        out_l_pkt.last = 0;
        out_r_pkt.data = 0;
        out_r_pkt.data.range(23, 0) = out_r.range(23, 0);
        out_r_pkt.keep = ~0;   // all byte enables valid
        out_r_pkt.last = is_last ? 1 : 0;

        s_out.write(out_l_pkt);
        s_out.write(out_r_pkt);
    }
}
