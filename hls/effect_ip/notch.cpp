#include "effect_ip.h"

// 60 Hz notch filter (2nd-order IIR biquad, Direct Form I)
//
// Difference equation:
//   y[n] = x[n] + b1*x[n-1] + x[n-2] - a1*y[n-1] - a2*y[n-2]
//
// Coefficients for 60 Hz notch at Fs = 48000 Hz:
//   omega0 = 2*pi*60/48000 = 0.007854 rad
//   cos(omega0) = 0.999969
//   r = 0.9997   →   notch bandwidth ≈ 4.6 Hz (-3 dB)
//   (r reduced from 0.9999 to 0.9997 to cut ringing τ from 208 ms to 69 ms;
//    pick-attack transients no longer excite a long 60 Hz ring that enters
//    distortion and causes crackling. A string (55 Hz) attenuation: ~0.82 dB,
//    inaudible under distortion. 60 Hz exact null is preserved regardless of r.)
//
//   b0 = 1           (no multiply)
//   b1 = -2*cos(w0)  = -1.999938
//   b2 = 1           (no multiply)
//   a1 = -2*r*cos(w0)= -1.999338
//   a2 = r^2         =  0.99940009
//
// Always applied (even in bypass): first-sample with zero state gives y[0]=x[0],
// so existing passthrough tests are unaffected.
//
// is_l=true  → L channel state (notch_x1_L, notch_x2_L, notch_y1_L, notch_y2_L)
// is_l=false → R channel state
// ap_fixed<18,2>: 2 integer bits + 16 fractional bits, resolution 2^-16 ≈ 15 µ
// Reduces multiply width vs ap_fixed<20,2>: 18×32→50-bit (2 DSPs) vs 52-bit (3 DSPs).
static const ap_fixed<18,2> NOTCH_B1 = -1.999938;
static const ap_fixed<18,2> NOTCH_A1 = -1.999338;
static const ap_fixed<18,2> NOTCH_A2 =  0.99940009;

sample_t apply_notch(sample_t in, bool is_l, state_t *state) {
    sample_t       x1 = is_l ? state->notch_x1_L : state->notch_x1_R;
    sample_t       x2 = is_l ? state->notch_x2_L : state->notch_x2_R;
    ap_fixed<32,2> y1 = is_l ? state->notch_y1_L : state->notch_y1_R;
    ap_fixed<32,2> y2 = is_l ? state->notch_y2_L : state->notch_y2_R;

    // y[n] = x[n] + b1*x[n-1] + x[n-2] - a1*y[n-1] - a2*y[n-2]
    ap_fixed<32,2> y = (ap_fixed<32,2>)in
                     + NOTCH_B1 * (ap_fixed<32,2>)x1
                     + (ap_fixed<32,2>)x2
                     - NOTCH_A1 * y1
                     - NOTCH_A2 * y2;

    if (is_l) {
        state->notch_x2_L = state->notch_x1_L;  state->notch_x1_L = in;
        state->notch_y2_L = state->notch_y1_L;  state->notch_y1_L = y;
    } else {
        state->notch_x2_R = state->notch_x1_R;  state->notch_x1_R = in;
        state->notch_y2_R = state->notch_y1_R;  state->notch_y1_R = y;
    }

    return (sample_t)y;
}
