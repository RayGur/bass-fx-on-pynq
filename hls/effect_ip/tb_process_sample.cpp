#include <iostream>
#include <cstdlib>
#include <cmath>
#include <cassert>
#include "effect_ip.h"

// --- Helpers ---

static sample_t to_sample(float v) { return (sample_t)v; }

// Encode float threshold to Q1.23 int (same as PS would write)
static param_t to_threshold(float v) { return (param_t)(int)(v * (1 << 23)); }

static float epsilon() { return 1.0f / (1 << 20); }  // ~1e-6, tolerance for float comparison

// -----------------------------------------------------------------------
// test_passthrough: dist_en=false, expect out == in
// (calls process_sample_core directly)
// -----------------------------------------------------------------------
static int test_passthrough(float val_l, float val_r, const char *label) {
    sample_t in_l  = to_sample(val_l);
    sample_t in_r  = to_sample(val_r);
    sample_t out_l = 0, out_r = 0;
    state_t  state = {0, 0, 0, 0, 0};

    process_sample_core(in_l, in_r, &out_l, &out_r,
                        false, false, 0, 0, 0, 0, &state);

    bool pass = (out_l == in_l) && (out_r == in_r);
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] bypass:" << label
              << "  in_l=" << (float)in_l << " out_l=" << (float)out_l
              << std::endl;
    return pass ? 0 : 1;
}

// -----------------------------------------------------------------------
// test_distortion: dist_en=true
// (calls process_sample_core directly)
// -----------------------------------------------------------------------
static int test_distortion(float in_f, float threshold_f, int gain,
                            float expected, float tol, const char *label) {
    sample_t in_l  = to_sample(in_f);
    sample_t out_l = 0, out_r = 0;
    state_t  state = {0, 0, 0, 0, 0};
    param_t  thr   = to_threshold(threshold_f);

    process_sample_core(in_l, in_l, &out_l, &out_r,
                        true, false, thr, gain, 0, 0, &state);

    float got  = (float)out_l;
    bool  pass = fabsf(got - expected) <= tol;
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] dist:" << label
              << "  in=" << in_f << " thr=" << threshold_f << " gain=" << gain
              << "  expected=" << expected << " got=" << got
              << std::endl;
    return pass ? 0 : 1;
}

// -----------------------------------------------------------------------
// test_wobble_zero: zero input must produce zero output regardless of LFO state
// -----------------------------------------------------------------------
static int test_wobble_zero(const char *label) {
    sample_t in_l  = to_sample(0.0f);
    sample_t out_l = 0, out_r = 0;
    state_t  state = {0, 0, 0, 0, 0};

    process_sample_core(in_l, in_l, &out_l, &out_r,
                        false, true, 0, 0, /*lfo_rate=*/89478, /*lfo_depth=*/100, &state);

    bool pass = (out_l == to_sample(0.0f)) && (out_r == to_sample(0.0f));
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] wobble:" << label
              << "  out_l=" << (float)out_l << " out_r=" << (float)out_r
              << std::endl;
    return pass ? 0 : 1;
}

// -----------------------------------------------------------------------
// test_wobble_attenuation: with lfo_rate=0 and lfo_depth=0, lut_idx=0 always,
// giving b = B_LUT[0]/32768 ≈ 0.00131 (fc≈10 Hz). With 2nd-order cascade,
// first L output ≈ b²*in ≈ 8.6e-7 (for in=0.5), which satisfies 0 < out_l < in_l.
// -----------------------------------------------------------------------
static int test_wobble_attenuation(const char *label) {
    sample_t in_l  = to_sample(0.5f);
    sample_t out_l = 0, out_r = 0;
    state_t  state = {0, 0, 0, 0, 0};

    process_sample_core(in_l, in_l, &out_l, &out_r,
                        false, true, 0, 0, /*lfo_rate=*/0, /*lfo_depth=*/0, &state);

    // b≈0.00131: 2nd-order output ≈ b²*in ≈ 8.6e-7 (in=0.5), strictly between 0 and in
    bool pass = ((float)out_l > 0.0f) && ((float)out_l < (float)in_l)
             && ((float)out_r > 0.0f) && ((float)out_r < (float)in_l);
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] wobble:" << label
              << "  in=0.5 out_l=" << (float)out_l << " out_r=" << (float)out_r
              << std::endl;
    return pass ? 0 : 1;
}

// -----------------------------------------------------------------------
// test_wobble_lfo_separates_lr: after multiple calls, lfo_phase advances on
// L samples only. Verify that L/R iir_prev diverge as expected (different state).
// -----------------------------------------------------------------------
static int test_wobble_lfo_separates_lr(const char *label) {
    // Run several stereo pairs; after steady-state the L/R outputs will differ
    // because R sees the already-advanced lfo_phase but holds its own iir_prev.
    // Simple sanity: output stays bounded in [-1, 0.9999].
    sample_t in_l = to_sample(0.8f);
    state_t  state = {0, 0, 0, 0, 0};
    int fail = 0;

    for (int i = 0; i < 32; i++) {
        sample_t out_l = 0, out_r = 0;
        process_sample_core(in_l, in_l, &out_l, &out_r,
                            false, true, 0, 0, /*lfo_rate=*/89478, /*lfo_depth=*/100, &state);
        if ((float)out_l < -1.0f || (float)out_l > 0.9999f ||
            (float)out_r < -1.0f || (float)out_r > 0.9999f) {
            std::cout << "[FAIL] wobble:" << label << " iter=" << i
                      << " out_l=" << (float)out_l << " out_r=" << (float)out_r << std::endl;
            fail++;
        }
    }
    if (fail == 0)
        std::cout << "[PASS] wobble:" << label << std::endl;
    return fail;
}

// -----------------------------------------------------------------------
// test_stream_wobble: AXI-Stream top function with wobble_en=1.
// Verifies TLAST placement and bounded output; no exact value check
// since IIR state evolves per sample.
// -----------------------------------------------------------------------
static int test_stream_wobble(float in_f, int n_pairs, const char *label) {
    hls::stream<audio_pkt_t> s_in, s_out;
    int total = n_pairs * 2;

    for (int i = 0; i < total; i++) {
        audio_pkt_t p;
        p.data = (ap_int<32>)(int)(in_f * (1 << 23));
        p.last = (i == total - 1) ? 1 : 0;
        s_in.write(p);
    }

    process_sample(s_in, s_out, n_pairs,
                   /*dist_en=*/0, /*wobble_en=*/1,
                   0, 0, /*lfo_rate=*/89478, /*lfo_depth=*/100);

    int fail = 0;
    for (int i = 0; i < total; i++) {
        audio_pkt_t p = s_out.read();

        bool expect_last = (i == total - 1);
        if ((int)p.last != (int)expect_last) {
            std::cout << "[FAIL] stream_wobble:" << label
                      << "  word " << i << " last=" << (int)p.last
                      << " expected=" << (int)expect_last << std::endl;
            fail++;
        }

        sample_t out_s;
        out_s.range(23, 0) = p.data.range(23, 0);
        float got = (float)out_s;
        if (got < -1.0f || got > 0.9999f) {
            std::cout << "[FAIL] stream_wobble:" << label
                      << "  word " << i << " out-of-bounds got=" << got << std::endl;
            fail++;
        }
    }
    if (fail == 0)
        std::cout << "[PASS] stream_wobble:" << label << std::endl;
    return fail;
}

// -----------------------------------------------------------------------
// test_stream: exercises the AXI-Stream top function.
// Sends N stereo pairs, verifies output values and TLAST on last word.
// -----------------------------------------------------------------------
static int test_stream(float in_f, float threshold_f, int gain,
                       float expected, float tol, int n_pairs,
                       const char *label) {
    hls::stream<audio_pkt_t> s_in, s_out;
    param_t thr = to_threshold(threshold_f);

    // Fill stream: L then R, interleaved; TLAST on very last word
    int total = n_pairs * 2;
    for (int i = 0; i < total; i++) {
        audio_pkt_t p;
        p.data = (ap_int<32>)(int)(in_f * (1 << 23));
        p.last = (i == total - 1) ? 1 : 0;
        s_in.write(p);
    }

    process_sample(s_in, s_out, n_pairs,
                   /*dist_en=*/1, /*wobble_en=*/0,
                   thr, gain, 0, 0);

    int fail = 0;
    for (int i = 0; i < total; i++) {
        audio_pkt_t p = s_out.read();

        // verify TLAST: only the very last word should have last=1
        bool expect_last = (i == total - 1);
        if ((int)p.last != (int)expect_last) {
            std::cout << "[FAIL] stream:" << label
                      << "  word " << i << " last=" << (int)p.last
                      << " expected=" << (int)expect_last << std::endl;
            fail++;
        }

        // verify output value (L and R should be identical for symmetric input)
        sample_t out_s;
        out_s.range(23, 0) = p.data.range(23, 0);
        float got = (float)out_s;
        if (fabsf(got - expected) > tol) {
            std::cout << "[FAIL] stream:" << label
                      << "  word " << i << " got=" << got
                      << " expected=" << expected << std::endl;
            fail++;
        }
    }

    if (fail == 0)
        std::cout << "[PASS] stream:" << label << std::endl;

    return fail;
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------
int main() {
    int fail = 0;

    // ---- Phase 1 regression: bypass (process_sample_core) ----
    std::cout << "--- Bypass tests ---" << std::endl;
    fail += test_passthrough( 0.0f,   0.0f,  "zero");
    fail += test_passthrough( 0.5f,   0.5f,  "positive 0.5");
    fail += test_passthrough(-0.5f,  -0.5f,  "negative -0.5");
    fail += test_passthrough( 0.9999f, 0.9999f, "near +max");
    fail += test_passthrough(-1.0f,  -1.0f,  "min -1.0");
    fail += test_passthrough( 0.25f, -0.75f, "asymmetric L/R");

    // ---- Phase 2: distortion (process_sample_core) ----
    std::cout << "\n--- Distortion tests ---" << std::endl;

    // No clipping: gain=2, in=0.2, thr=0.8 -> amp=0.4 < 0.8 -> out=0.4
    fail += test_distortion(0.2f, 0.8f, 2, 0.4f, epsilon(), "no-clip positive");

    // No clipping negative: gain=2, in=-0.2, thr=0.8 -> amp=-0.4 -> out=-0.4
    fail += test_distortion(-0.2f, 0.8f, 2, -0.4f, epsilon(), "no-clip negative");

    // Positive clip: gain=4, in=0.5, thr=0.3 -> amp=2.0 -> clipped to 0.3
    fail += test_distortion(0.5f, 0.3f, 4, 0.3f, epsilon(), "clip positive");

    // Negative clip: gain=4, in=-0.5, thr=0.3 -> amp=-2.0 -> clipped to -0.3
    fail += test_distortion(-0.5f, 0.3f, 4, -0.3f, epsilon(), "clip negative");

    // gain=1, thr≈1.0: near passthrough (threshold just under 1.0)
    fail += test_distortion(0.7f, 0.9999f, 1, 0.7f, epsilon(), "gain=1 near-passthrough");

    // Max gain, small input, clip: gain=20, in=0.5, thr=0.3 -> amp=10.0 -> clipped to 0.3
    fail += test_distortion(0.5f, 0.3f, 20, 0.3f, epsilon(), "max-gain clip");

    // Max gain, tiny input, no clip: gain=20, in=0.01, thr=0.5 -> amp=0.2 < 0.5
    fail += test_distortion(0.01f, 0.5f, 20, 0.2f, epsilon(), "max-gain no-clip");

    // ---- Phase 3: wobble (process_sample_core) ----
    std::cout << "\n--- Wobble tests ---" << std::endl;
    fail += test_wobble_zero("zero-in");
    fail += test_wobble_attenuation("depth=0 attenuation");
    fail += test_wobble_lfo_separates_lr("32pairs bounded");

    // ---- Phase 6: AXI-Stream top function ----
    std::cout << "\n--- Stream tests ---" << std::endl;

    // No clipping via stream: gain=2, in=0.2, thr=0.8 -> out=0.4
    fail += test_stream(0.2f, 0.8f, 2, 0.4f, epsilon(), 16, "no-clip 16pairs");

    // Clipping via stream: gain=4, in=0.5, thr=0.3 -> clipped to 0.3
    fail += test_stream(0.5f, 0.3f, 4, 0.3f, epsilon(), 16, "clip 16pairs");

    // Single pair edge case: verify TLAST on word 1 (index 1 of 2)
    fail += test_stream(0.2f, 0.8f, 2, 0.4f, epsilon(), 1, "single-pair TLAST");

    // Wobble via stream: lfo_rate=89478 (~4 Hz), depth=100; check TLAST + bounds
    fail += test_stream_wobble(0.5f, 16, "wobble 16pairs");
    fail += test_stream_wobble(0.5f,  1, "wobble single-pair TLAST");

    // Summary
    std::cout << std::endl;
    if (fail == 0)
        std::cout << "All tests PASSED. OK to proceed to synthesis." << std::endl;
    else
        std::cout << fail << " test(s) FAILED." << std::endl;

    return fail;
}
