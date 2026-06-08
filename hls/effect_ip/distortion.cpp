#include "effect_ip.h"

// apply_distortion -- hard clipping distortion
//
// threshold : Q1.23 raw integer (PS writes int(clip_float * (1<<23)), e.g. 0.3 -> 0x2666666)
// gain      : plain integer multiplier 1-20
//
// Algorithm:
//   1. Amplify input by gain using ap_fixed<32,6> intermediate (Q6.26, range [-32,+32))
//   2. Decode threshold from Q1.23 raw int via ap_int<24> cast
//   3. Hard-clip amplified signal at +/-threshold
//   4. Cast back to sample_t (clip in step 3 guarantees no wrap)
sample_t apply_distortion(sample_t in, param_t threshold, param_t gain) {
    // 1. Amplify; use ap_fixed<6,6> for gain to keep result type manageable
    //    ap_fixed<24,1> * ap_fixed<6,6> -> ap_fixed<30,7>, fits in ap_fixed<32,6>
    ap_fixed<32, 6> amplified = (ap_fixed<32, 6>)in * (ap_fixed<6, 6>)gain;

    // 2. Decode threshold: raw bit reinterpret of Q1.23 int into ap_fixed<24,1>
    //    (ap_fixed<24,1>)(ap_int<24>)threshold performs VALUE conversion (wrong!)
    //    Correct: assign raw bits directly via .range()
    ap_fixed<24, 1> thresh_q;
    thresh_q.range(23, 0) = ap_int<24>(threshold);   // bit reinterpret, not value cast
    ap_fixed<32, 6> thresh = thresh_q;

    // 3. Hard clip at +/-threshold
    if (amplified >  thresh) amplified =  thresh;
    if (amplified < -thresh) amplified = -thresh;

    // 4. Return as sample_t; threshold <= 1.0 guaranteed by step 3, so no wrap
    return (sample_t)amplified;
}
