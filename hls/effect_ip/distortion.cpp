#include "effect_ip.h"

// apply_distortion -- hard clipping distortion with hysteresis noise gate
//
// threshold : Q1.23 raw integer (PS writes int(clip_float * (1<<23)), e.g. 0.3 -> 0x2666666)
// gain      : plain integer multiplier 1-20
//
// Algorithm:
//   0. Noise gate with hysteresis (14.2/14.9):
//        open  threshold = 0.001  (gate opens when |in| exceeds this)
//        close threshold = 0.0003 (gate stays open until |in| falls below this)
//      Hysteresis prevents gate chatter during note decay: without it, the
//      decaying sine oscillates around 0.001, getting chopped at every zero
//      crossing, producing a broken/crackling sound. With hysteresis the note
//      decays smoothly to 0.0003 before the gate closes.
//   1. Amplify input by gain using ap_fixed<32,6> intermediate (Q6.26, range [-32,+32))
//   2. Decode threshold from Q1.23 raw int via ap_int<24> cast
//   3. Hard-clip amplified signal at +/-threshold
//   4. Cast back to sample_t (clip in step 3 guarantees no wrap)
sample_t apply_distortion(sample_t in, param_t threshold, param_t gain,
                          state_t *state, bool is_l) {
    static const sample_t GATE_OPEN_THR  = 0.001;   // -60 dBFS: open gate when exceeded
    static const sample_t GATE_CLOSE_THR = 0.0003;  // -70 dBFS: close gate when fallen below

    bool gate_open = is_l ? state->dist_gate_open_L : state->dist_gate_open_R;

    if (gate_open) {
        // Gate is currently open: close it only when signal falls to close threshold
        if (in >= -GATE_CLOSE_THR && in <= GATE_CLOSE_THR)
            gate_open = false;
    } else {
        // Gate is currently closed: open it when signal exceeds open threshold
        if (in < -GATE_OPEN_THR || in > GATE_OPEN_THR)
            gate_open = true;
    }

    if (is_l) state->dist_gate_open_L = gate_open;
    else       state->dist_gate_open_R = gate_open;

    if (!gate_open) return (sample_t)0;

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
