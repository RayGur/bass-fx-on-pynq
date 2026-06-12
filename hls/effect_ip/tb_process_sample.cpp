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
    state_t  state = {0, sample_t(0)};

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
    state_t  state = {0, sample_t(0)};
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

    // ---- Phase 6: AXI-Stream top function ----
    std::cout << "\n--- Stream tests ---" << std::endl;

    // No clipping via stream: gain=2, in=0.2, thr=0.8 -> out=0.4
    fail += test_stream(0.2f, 0.8f, 2, 0.4f, epsilon(), 16, "no-clip 16pairs");

    // Clipping via stream: gain=4, in=0.5, thr=0.3 -> clipped to 0.3
    fail += test_stream(0.5f, 0.3f, 4, 0.3f, epsilon(), 16, "clip 16pairs");

    // Single pair edge case: verify TLAST on word 1 (index 1 of 2)
    fail += test_stream(0.2f, 0.8f, 2, 0.4f, epsilon(), 1, "single-pair TLAST");

    // Summary
    std::cout << std::endl;
    if (fail == 0)
        std::cout << "All tests PASSED. OK to proceed to synthesis." << std::endl;
    else
        std::cout << fail << " test(s) FAILED." << std::endl;

    return fail;
}
