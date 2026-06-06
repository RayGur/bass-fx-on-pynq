#include <iostream>
#include <cstdlib>
#include "effect_ip.h"

// --- Helpers ---

// Cast float to sample_t for convenient test vector creation
static sample_t to_sample(float v) {
    return (sample_t)v;
}

// Single passthrough test: assert out_l == in_l and out_r == in_r.
// Returns 1 on failure, 0 on pass.
static int test_passthrough(float val_l, float val_r, const char *label) {
    sample_t in_l  = to_sample(val_l);
    sample_t in_r  = to_sample(val_r);
    sample_t out_l = 0, out_r = 0;
    state_t  state = {0};

    process_sample(
        in_l, in_r,
        &out_l, &out_r,
        false, false,   // dist_en=false, wobble_en=false (both off in Phase 1)
        0, 0,           // threshold, gain
        0, 0,           // lfo_rate, lfo_depth
        &state
    );

    bool pass = (out_l == in_l) && (out_r == in_r);
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] " << label
              << "  in_l=" << (float)in_l << " out_l=" << (float)out_l
              << "  in_r=" << (float)in_r << " out_r=" << (float)out_r
              << std::endl;
    return pass ? 0 : 1;
}

// --- main ---

int main() {
    int fail = 0;

    // Basic values
    fail += test_passthrough( 0.0f,  0.0f, "zero");
    fail += test_passthrough( 0.5f,  0.5f, "positive 0.5");
    fail += test_passthrough(-0.5f, -0.5f, "negative -0.5");

    // Near boundary values (ap_fixed<24,1> max ~+0.9999999, min -1.0)
    fail += test_passthrough( 0.9999f,  0.9999f, "near +max");
    fail += test_passthrough(-1.0f,    -1.0f,    "min -1.0");

    // Asymmetric L/R (verify channels are independent)
    fail += test_passthrough( 0.25f, -0.75f, "asymmetric L/R");

    // Summary
    if (fail == 0) {
        std::cout << "\nAll tests PASSED. OK to proceed to synthesis." << std::endl;
    } else {
        std::cout << "\n" << fail << " test(s) FAILED." << std::endl;
    }

    return fail;  // 0 = pass; non-zero causes Vitis HLS C Sim to report failure
}
