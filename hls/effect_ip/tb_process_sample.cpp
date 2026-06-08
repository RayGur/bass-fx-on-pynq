#include <iostream>
#include <cstdlib>
#include <cmath>
#include "effect_ip.h"

// --- Helpers ---

static sample_t to_sample(float v) { return (sample_t)v; }

// Encode float threshold to Q1.23 int (same as PS would write)
static param_t to_threshold(float v) { return (param_t)(int)(v * (1 << 23)); }

static float epsilon() { return 1.0f / (1 << 20); }  // ~1e-6, tolerance for float comparison

// -----------------------------------------------------------------------
// test_passthrough: dist_en=false, expect out == in
// -----------------------------------------------------------------------
static int test_passthrough(float val_l, float val_r, const char *label) {
    sample_t in_l  = to_sample(val_l);
    sample_t in_r  = to_sample(val_r);
    sample_t out_l = 0, out_r = 0;
    state_t  state = {0};

    process_sample(in_l, in_r, &out_l, &out_r,
                   false, false, 0, 0, 0, 0, &state);

    bool pass = (out_l == in_l) && (out_r == in_r);
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] bypass:" << label
              << "  in_l=" << (float)in_l << " out_l=" << (float)out_l
              << std::endl;
    return pass ? 0 : 1;
}

// -----------------------------------------------------------------------
// test_distortion: dist_en=true
//   expected_l: expected output for left channel
//   tol       : tolerance (use 0 for exact, epsilon() for float rounding)
// -----------------------------------------------------------------------
static int test_distortion(float in_f, float threshold_f, int gain,
                            float expected, float tol, const char *label) {
    sample_t in_l  = to_sample(in_f);
    sample_t out_l = 0, out_r = 0;
    state_t  state = {0};
    param_t  thr   = to_threshold(threshold_f);

    process_sample(in_l, in_l, &out_l, &out_r,
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
// main
// -----------------------------------------------------------------------
int main() {
    int fail = 0;

    // ---- Phase 1 regression: bypass (dist_en=false) ----
    std::cout << "--- Bypass tests ---" << std::endl;
    fail += test_passthrough( 0.0f,   0.0f,  "zero");
    fail += test_passthrough( 0.5f,   0.5f,  "positive 0.5");
    fail += test_passthrough(-0.5f,  -0.5f,  "negative -0.5");
    fail += test_passthrough( 0.9999f, 0.9999f, "near +max");
    fail += test_passthrough(-1.0f,  -1.0f,  "min -1.0");
    fail += test_passthrough( 0.25f, -0.75f, "asymmetric L/R");

    // ---- Phase 2: distortion ----
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

    // Summary
    std::cout << std::endl;
    if (fail == 0)
        std::cout << "All tests PASSED. OK to proceed to synthesis." << std::endl;
    else
        std::cout << fail << " test(s) FAILED." << std::endl;

    return fail;
}
