"""
wobble_test.py -- Phase 3 wobble AXI-Lite sanity check (no codec / no audio)
PYNQ-Z2, PYNQ 2.5 (Glasgow)

Usage (Jupyter Notebook):
    %run wobble_test.py

No bass / codec needed. Pure AXI-Lite register access only.
"""

from pynq import Overlay, MMIO

# ------------------------------------------------------------------ #
#  Overlay                                                            #
# ------------------------------------------------------------------ #

BIT_PATH = "/home/xilinx/jupyter_notebooks/bass_fx/bass_fx.bit"

print("Loading overlay...")
ol = Overlay(BIT_PATH, ignore_version=True)
print("Overlay loaded.")

# ------------------------------------------------------------------ #
#  Address map                                                        #
# ------------------------------------------------------------------ #

EFFECT_BASE  = 0x40020000
EFFECT_RANGE = 0x10000

ADDR_AP_CTRL   = 0x00
ADDR_IN_L      = 0x10
ADDR_IN_R      = 0x18
ADDR_OUT_L     = 0x20
ADDR_OUT_R     = 0x30
ADDR_DIST_EN   = 0x40
ADDR_WOBBLE_EN = 0x48
ADDR_LFO_RATE  = 0x60
ADDR_LFO_DEPTH = 0x68

effect = MMIO(EFFECT_BASE, EFFECT_RANGE)
effect.write(ADDR_DIST_EN,   0)
effect.write(ADDR_WOBBLE_EN, 0)

# ------------------------------------------------------------------ #
#  Helpers                                                            #
# ------------------------------------------------------------------ #

def float_to_q123(f):
    return int(f * (1 << 23)) & 0xFFFFFF

def q123_to_float(raw):
    raw24 = raw & 0xFFFFFF
    if raw24 & 0x800000:
        raw24 -= 0x1000000
    return raw24 / (1 << 23)

def run_effect(in_l, in_r):
    effect.write(ADDR_IN_L, float_to_q123(in_l))
    effect.write(ADDR_IN_R, float_to_q123(in_r))
    effect.write(ADDR_AP_CTRL, 0x01)
    for _ in range(1000):
        if effect.read(ADDR_AP_CTRL) & 0x02:
            break
    return q123_to_float(effect.read(ADDR_OUT_L)), \
           q123_to_float(effect.read(ADDR_OUT_R))

def set_wobble(rate_hz=1.0, depth_pct=70):
    lfo_step = int(rate_hz * (1 << 32) / 48000)
    effect.write(ADDR_LFO_RATE,  lfo_step)
    effect.write(ADDR_LFO_DEPTH, depth_pct)

def enable_wobble(on=True):
    effect.write(ADDR_WOBBLE_EN, 1 if on else 0)

def reset_iir(n=8):
    """Feed zero samples to drain IIR state."""
    for _ in range(n):
        run_effect(0.0, 0.0)

# ------------------------------------------------------------------ #
#  Test runner                                                        #
# ------------------------------------------------------------------ #

_passed = 0
_failed = 0

def check(label, ok, detail=""):
    global _passed, _failed
    status = "PASS" if ok else "FAIL"
    print("  [{}] {}{}".format(status, label,
                                "  — " + detail if detail else ""))
    if ok:
        _passed += 1
    else:
        _failed += 1

TOL = 0.002   # ~2 LSB in Q1.23

# ------------------------------------------------------------------ #
#  1. Passthrough: both effects off                                   #
# ------------------------------------------------------------------ #
print()
print("[1] Passthrough (dist_en=0, wobble_en=0)")
effect.write(ADDR_DIST_EN,   0)
effect.write(ADDR_WOBBLE_EN, 0)
for val in [0.5, -0.5, 0.25, -0.25, 0.0]:
    out_l, out_r = run_effect(val, val)
    check("in={:+.2f}".format(val),
          abs(out_l - val) < TOL,
          "out_l={:.4f}".format(out_l))

# ------------------------------------------------------------------ #
#  2. Wobble bypass: wobble_en=0 ignores params                      #
# ------------------------------------------------------------------ #
print()
print("[2] Wobble bypass (wobble_en=0, params written)")
set_wobble(rate_hz=2.0, depth_pct=80)
enable_wobble(False)
for val in [0.5, -0.3]:
    out_l, _ = run_effect(val, val)
    check("bypass {:+.2f}".format(val),
          abs(out_l - val) < TOL,
          "out_l={:.4f}".format(out_l))

# ------------------------------------------------------------------ #
#  3. Wobble on: IIR attenuates signal                               #
# ------------------------------------------------------------------ #
print()
print("[3] Wobble on — IIR should attenuate signal")
reset_iir()
enable_wobble(True)
set_wobble(rate_hz=1.0, depth_pct=70)
out_l, _ = run_effect(0.5, 0.5)
check("attenuates vs input",
      abs(out_l - 0.5) > TOL,
      "in=0.5000 out_l={:.4f}".format(out_l))
check("no overflow",
      abs(out_l) <= 1.0,
      "out_l={:.4f}".format(out_l))

# ------------------------------------------------------------------ #
#  4. LFO advancing: output changes over consecutive samples         #
# ------------------------------------------------------------------ #
print()
print("[4] LFO advancing — output changes over time")
enable_wobble(True)
set_wobble(rate_hz=4.0, depth_pct=100)
samples = [run_effect(0.5, 0.5)[0] for _ in range(20)]
unique = len(set(round(s, 4) for s in samples))
check("lfo_phase advancing",
      unique > 1,
      "{} unique values over 20 samples".format(unique))

# ------------------------------------------------------------------ #
#  5. Rate affects output                                             #
# ------------------------------------------------------------------ #
print()
print("[5] Rate affects output (slow vs fast)")
reset_iir()
enable_wobble(True)
set_wobble(rate_hz=0.5, depth_pct=70)
for _ in range(10):
    run_effect(0.5, 0.5)
out_slow, _ = run_effect(0.5, 0.5)

reset_iir()
set_wobble(rate_hz=4.0, depth_pct=70)
for _ in range(10):
    run_effect(0.5, 0.5)
out_fast, _ = run_effect(0.5, 0.5)

check("slow != fast",
      abs(out_slow - out_fast) > TOL,
      "slow={:.4f} fast={:.4f}".format(out_slow, out_fast))

# ------------------------------------------------------------------ #
#  6. Depth affects attenuation                                      #
# ------------------------------------------------------------------ #
print()
print("[6] Depth affects output (depth=10 vs depth=100)")
reset_iir()
set_wobble(rate_hz=1.0, depth_pct=10)
for _ in range(10):
    run_effect(0.5, 0.5)
out_shallow, _ = run_effect(0.5, 0.5)

reset_iir()
set_wobble(rate_hz=1.0, depth_pct=100)
for _ in range(10):
    run_effect(0.5, 0.5)
out_deep, _ = run_effect(0.5, 0.5)

check("shallow != deep",
      abs(out_shallow - out_deep) > TOL,
      "depth10={:.4f} depth100={:.4f}".format(out_shallow, out_deep))

# ------------------------------------------------------------------ #
#  7. No overflow for edge inputs                                    #
# ------------------------------------------------------------------ #
print()
print("[7] No overflow (|out| <= 1.0) for edge inputs")
enable_wobble(True)
for depth in [0, 50, 100]:
    set_wobble(rate_hz=1.0, depth_pct=depth)
    for val in [0.9, -0.9]:
        out_l, out_r = run_effect(val, val)
        check("depth={} in={:.1f}".format(depth, val),
              abs(out_l) <= 1.0 and abs(out_r) <= 1.0,
              "out_l={:.4f} out_r={:.4f}".format(out_l, out_r))

# ------------------------------------------------------------------ #
#  Summary                                                            #
# ------------------------------------------------------------------ #

enable_wobble(False)

print()
print("=" * 50)
total = _passed + _failed
print("  Result: {}/{} passed".format(_passed, total))
if _failed == 0:
    print("  ALL PASS — wobble IP AXI-Lite OK")
    print("  (codec + audio test requires bass guitar)")
else:
    print("  {} FAILED — check wobble.cpp / process_sample.cpp".format(_failed))
print("=" * 50)
