#include "effect_ip.h"

// apply_distortion -- hard clipping distortion
//
// Phase 1: passthrough stub, returns input unchanged.
// Phase 2 (Ray): fill in the following steps:
//   1. Amplify 'in' by 'gain'
//   2. Hard-clip the result at 'threshold' (clamp to [-1, +1))
//   3. Return the clipped value
//
// Interpretation of threshold/gain as fixed-point values to be decided in Phase 2.
sample_t apply_distortion(sample_t in, param_t threshold, param_t gain) {
    return in;  // Phase 2: replace with hard clipping logic
}
