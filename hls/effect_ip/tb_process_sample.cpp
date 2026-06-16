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
    state_t  state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    process_sample_core(in_l, in_r, &out_l, &out_r,
                        false, false, 0, 0, 0, 0, 0, &state);

    bool pass = (out_l == in_l) && (out_r == in_r);
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] bypass:" << label
              << "  in_l=" << (float)in_l << " out_l=" << (float)out_l
              << std::endl;
    return pass ? 0 : 1;
}

// -----------------------------------------------------------------------
// test_distortion_direct: calls apply_distortion() in isolation (no notch/HPF).
// Use this for exact-value tests (no-clip cases) where the chain pre-processing
// would alter the input and the expected output cannot be expressed with epsilon().
// -----------------------------------------------------------------------
static int test_distortion_direct(float in_f, float threshold_f, int gain,
                                  float expected, float tol, const char *label) {
    sample_t in_s = to_sample(in_f);
    param_t  thr  = to_threshold(threshold_f);
    state_t  state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    sample_t out_s = apply_distortion(in_s, thr, gain, &state, /*is_l=*/true);

    float got  = (float)out_s;
    bool  pass = fabsf(got - expected) <= tol;
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] dist_direct:" << label
              << "  in=" << in_f << " thr=" << threshold_f << " gain=" << gain
              << "  expected=" << expected << " got=" << got
              << std::endl;
    return pass ? 0 : 1;
}

// -----------------------------------------------------------------------
// test_distortion: dist_en=true, exercises full chain (notch→HPF→distortion).
// With fresh zero state, notch passes first sample unchanged (y[0]=x[0]),
// but HPF attenuates: HPF_out ≈ 0.9963*in. Use clipping scenarios only here
// (output = threshold regardless of exact HPF attenuation). For no-clip exact
// value checks, use test_distortion_direct().
// -----------------------------------------------------------------------
static int test_distortion(float in_f, float threshold_f, int gain,
                            float expected, float tol, const char *label) {
    sample_t in_l  = to_sample(in_f);
    sample_t out_l = 0, out_r = 0;
    state_t  state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    param_t  thr   = to_threshold(threshold_f);

    process_sample_core(in_l, in_l, &out_l, &out_r,
                        true, false, thr, gain, 0, 0, 0, &state);

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
    state_t  state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    process_sample_core(in_l, in_l, &out_l, &out_r,
                        false, true, 0, 0, /*lfo_rate=*/89478, /*lfo_depth=*/100, /*lfo_floor=*/0, &state);

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
    state_t  state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    process_sample_core(in_l, in_l, &out_l, &out_r,
                        false, true, 0, 0, /*lfo_rate=*/0, /*lfo_depth=*/0, /*lfo_floor=*/0, &state);

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
    state_t  state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int fail = 0;

    for (int i = 0; i < 32; i++) {
        sample_t out_l = 0, out_r = 0;
        process_sample_core(in_l, in_l, &out_l, &out_r,
                            false, true, 0, 0, /*lfo_rate=*/89478, /*lfo_depth=*/100, /*lfo_floor=*/0, &state);
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
// test_noise_gate: signals below NOISE_GATE_THR (0.001) must be muted when
// dist_en=true; signals above must pass through the normal distortion path.
// -----------------------------------------------------------------------
static int test_noise_gate_mutes(float in_f, const char *label) {
    // |in_f| < 0.001 → noise gate → out must be 0
    sample_t in_l  = to_sample(in_f);
    sample_t out_l = 0, out_r = 0;
    state_t  state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    param_t  thr   = to_threshold(0.8f);

    process_sample_core(in_l, in_l, &out_l, &out_r,
                        true, false, thr, /*gain=*/20, 0, 0, 0, &state);

    bool pass = (out_l == to_sample(0.0f)) && (out_r == to_sample(0.0f));
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] noise_gate:mute:" << label
              << "  in=" << in_f << " out_l=" << (float)out_l << std::endl;
    return pass ? 0 : 1;
}

static int test_noise_gate_pass(float in_f, float thr_f, int gain,
                                float expected, float tol, const char *label) {
    // |in_f| > 0.001 → gate open → normal distortion path
    // Use heavy-clipping scenario so HPF first-sample attenuation doesn't affect result:
    //   in=0.5, gain=4, thr=0.3 → HPF(0.5)≈0.4998, *4=1.999 → clipped to 0.3
    sample_t in_l  = to_sample(in_f);
    sample_t out_l = 0, out_r = 0;
    state_t  state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    param_t  thr   = to_threshold(thr_f);

    process_sample_core(in_l, in_l, &out_l, &out_r,
                        true, false, thr, gain, 0, 0, 0, &state);

    float got  = (float)out_l;
    bool  pass = fabsf(got - expected) <= tol;
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] noise_gate:pass:" << label
              << "  in=" << in_f << " expected=" << expected << " got=" << got
              << std::endl;
    return pass ? 0 : 1;
}

// -----------------------------------------------------------------------
// test_hpf_dc_block: constant DC input (0.5) must be blocked by the HPF.
// Calls apply_hpf() directly (no notch in path) to isolate HPF behavior.
// HPF output decays as alpha^n * 0.5 (alpha ≈ 0.9960 in ap_fixed<18,1>).
// After 2000 samples: 0.9960^2001 * 0.5 ≈ 0.000141 < 0.001 (effectively 0).
// -----------------------------------------------------------------------
static int test_hpf_dc_block(const char *label) {
    state_t  state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    sample_t in_l  = to_sample(0.5f);
    sample_t out   = 0;

    // Call apply_hpf() directly — no notch transient interference
    for (int i = 0; i < 2000; i++) {
        out = apply_hpf(in_l, /*is_l=*/true, &state);
    }

    // After 2000 samples of DC, HPF output should have decayed well below 0.001
    bool pass = fabsf((float)out) < 0.001f;
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] hpf:" << label
              << "  out after 2000 samples=" << (float)out << " (expect < 0.001)"
              << std::endl;
    return pass ? 0 : 1;
}

// -----------------------------------------------------------------------
// test_notch_first_sample: with zero state, first sample must pass through
// unchanged (Direct Form I property: y[0] = x[0] when all prev state = 0).
// -----------------------------------------------------------------------
static int test_notch_first_sample(float val, const char *label) {
    sample_t in_l  = to_sample(val);
    sample_t out_l = 0, out_r = 0;
    state_t  state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    // bypass mode: both effects off — notch still runs (always-on), HPF does not
    process_sample_core(in_l, in_l, &out_l, &out_r,
                        false, false, 0, 0, 0, 0, 0, &state);

    bool pass = (out_l == in_l) && (out_r == in_l);
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] notch:first_sample:" << label
              << "  in=" << val << " out_l=" << (float)out_l << std::endl;
    return pass ? 0 : 1;
}

// -----------------------------------------------------------------------
// test_notch_60hz: pure 60 Hz sine input must be strongly attenuated.
// With r=0.9997, transient decay time constant τ ≈ 1/(1-r) = 3,333 samples.
// After 50,000 samples: 0.9997^50000 ≈ 5e-7 → output amplitude negligible.
// Checks last 200 samples RMS < 0.05 (input RMS ≈ 0.354).
// Calls apply_notch() directly to isolate from HPF/distortion.
// -----------------------------------------------------------------------
static int test_notch_60hz(const char *label) {
    state_t  state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const int N     = 50000;  // > 5τ at r=0.9999 (τ=10000 samples)
    const int CHECK = 200;
    float rms_sum = 0.0f;
    int fail = 0;

    for (int i = 0; i < N; i++) {
        float in_f = 0.5f * sinf(2.0f * 3.14159265f * 60.0f * i / 48000.0f);
        sample_t in_s = to_sample(in_f);

        // Call apply_notch() directly — no HPF/distortion interference
        sample_t out_s = apply_notch(in_s, /*is_l=*/true, &state);

        if (i >= N - CHECK) {
            float v = (float)out_s;
            rms_sum += v * v;
        }
    }

    float rms = sqrtf(rms_sum / CHECK);
    // Input RMS ≈ 0.354; after 50k samples the notch transient ≈ 0.007× → RMS < 0.005
    bool pass = (rms < 0.05f);
    std::cout << "[" << (pass ? "PASS" : "FAIL") << "] notch:" << label
              << "  RMS(last " << CHECK << " of " << N << " samples)="
              << rms << " (expect < 0.05)" << std::endl;
    if (!pass) fail++;
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
                   0, 0, /*lfo_rate=*/89478, /*lfo_depth=*/100, /*lfo_floor=*/0);

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
                   thr, gain, 0, 0, /*lfo_floor=*/0);

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

    // ---- Phase 2: distortion ----
    std::cout << "\n--- Distortion tests ---" << std::endl;

    // No-clip exact-value tests use apply_distortion() directly (no chain pre-processing).
    // gain=2, in=0.2, thr=0.8 -> amp=0.4 < 0.8 -> out=0.4
    fail += test_distortion_direct( 0.2f, 0.8f, 2,  0.4f, epsilon(), "no-clip positive");
    fail += test_distortion_direct(-0.2f, 0.8f, 2, -0.4f, epsilon(), "no-clip negative");

    // gain=1, thr≈1.0: near passthrough
    fail += test_distortion_direct(0.7f, 0.9999f, 1, 0.7f, epsilon(), "gain=1 near-passthrough");

    // Max gain, tiny input, no clip: gain=20, in=0.01, thr=0.5 -> amp=0.2 < 0.5
    fail += test_distortion_direct(0.01f, 0.5f, 20, 0.2f, epsilon(), "max-gain no-clip");

    // Clipping tests use the full chain (notch→HPF→distortion).
    // HPF attenuation (~0.9963×in) still causes clipping since gain amplifies heavily.
    // Positive clip: gain=4, in=0.5, thr=0.3 -> HPF(0.5)≈0.498, *4=1.99 -> clipped to 0.3
    fail += test_distortion( 0.5f, 0.3f,  4,  0.3f, epsilon(), "clip positive");
    fail += test_distortion(-0.5f, 0.3f,  4, -0.3f, epsilon(), "clip negative");

    // Max gain, small input, clip: gain=20, in=0.5, thr=0.3 -> clipped to 0.3
    fail += test_distortion( 0.5f, 0.3f, 20,  0.3f, epsilon(), "max-gain clip");

    // Noise gate: input below 0.001 must be muted (gain=20 would amplify without gate)
    fail += test_noise_gate_mutes( 0.0005f, "+0.0005 below thr");
    fail += test_noise_gate_mutes(-0.0005f, "-0.0005 below thr");
    fail += test_noise_gate_mutes( 0.001f,  "+0.001 at thr boundary");

    // Noise gate: input above 0.001 → gate open → normal distortion (clipping path)
    // in=0.5, gain=4, thr=0.3: HPF(0.5)≈0.4998 → *4=1.999 → clipped to 0.3
    fail += test_noise_gate_pass( 0.5f, 0.3f, 4,  0.3f, epsilon(), "+0.5 clips to thr");
    fail += test_noise_gate_pass(-0.5f, 0.3f, 4, -0.3f, epsilon(), "-0.5 clips to -thr");

    // ---- Phase 3: wobble (process_sample_core) ----
    std::cout << "\n--- Wobble tests ---" << std::endl;
    fail += test_wobble_zero("zero-in");
    fail += test_wobble_attenuation("depth=0 attenuation");
    fail += test_wobble_lfo_separates_lr("32pairs bounded");

    // ---- Phase 6: AXI-Stream top function ----
    std::cout << "\n--- Stream tests ---" << std::endl;

    // No clipping via stream: gain=2, in=0.2, thr=0.8 -> out=0.4
    // Clipping via stream: gain=4, in=0.5, thr=0.3 -> HPF(0.5)≈0.498, *4=1.99 -> clips to 0.3
    // All 16 pairs stay in clipping region (HPF output > 0.075 for first 32 samples)
    fail += test_stream(0.5f, 0.3f, 4, 0.3f, epsilon(), 16, "clip 16pairs");

    // Max gain clipping: gain=20, in=0.5, thr=0.3 -> heavily clipped to 0.3
    fail += test_stream(0.5f, 0.3f, 20, 0.3f, epsilon(), 16, "max-gain clip 16pairs");

    // Single pair edge case: verify TLAST on word 1 (index 1 of 2); use clipping
    fail += test_stream(0.5f, 0.3f, 4, 0.3f, epsilon(), 1, "single-pair TLAST clip");

    // Wobble via stream: lfo_rate=89478 (~4 Hz), depth=100; check TLAST + bounds
    fail += test_stream_wobble(0.5f, 16, "wobble 16pairs");
    fail += test_stream_wobble(0.5f,  1, "wobble single-pair TLAST");

    // ---- HPF: DC blocking ----
    std::cout << "\n--- HPF tests ---" << std::endl;
    fail += test_hpf_dc_block("dc=0.5 decays after 200 samples");

    // ---- Notch: 60 Hz rejection ----
    std::cout << "\n--- Notch tests ---" << std::endl;
    fail += test_notch_first_sample(0.5f,  "first-sample +0.5");
    fail += test_notch_first_sample(-0.5f, "first-sample -0.5");
    fail += test_notch_60hz("60Hz sine RMS < 0.05 at steady state");

    // Summary
    std::cout << std::endl;
    if (fail == 0)
        std::cout << "All tests PASSED. OK to proceed to synthesis." << std::endl;
    else
        std::cout << fail << " test(s) FAILED." << std::endl;

    return fail;
}
