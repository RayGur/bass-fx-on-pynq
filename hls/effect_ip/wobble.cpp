#include "effect_ip.h"

// apply_wobble -- first-order IIR lowpass with LFO sweep (wobble / wah)
//
// Phase 1: passthrough stub, returns input unchanged.
// Phase 3 (Claire): fill in the following steps:
//   1. Advance state->lfo_phase by lfo_rate (phase accumulator)
//   2. Look up the cutoff-frequency coefficient for the current LFO phase from a LUT
//   3. Apply first-order IIR: y[n] = a * x[n] + (1 - a) * state->iir_prev
//   4. Update state->iir_prev, clamp to [-1, +1), and return
//
// NOTE FOR CLAIRE (before starting Phase 3):
//   - Agree with Ray on state_t fields (LFO phase type, IIR history type)
//   - Update the state_t definition in effect_ip.h
//   - Ask Ray to uncomment the wobble block in process_sample.cpp
//   - Pre-compute LFO coefficients into a LUT to avoid quantisation error
//     from recomputing them on every sample during the sweep
sample_t apply_wobble(sample_t in, param_t lfo_rate, param_t lfo_depth, state_t *state) {
    return in;  // Phase 3: replace with IIR + LFO logic
}
