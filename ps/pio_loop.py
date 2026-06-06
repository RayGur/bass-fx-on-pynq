"""
pio_loop.py -- Phase 1 PS-side PIO audio loop
PYNQ-Z2, PYNQ 2.5 (Glasgow), Ubuntu 18.04

Usage (Jupyter Notebook or on-board Python):
    %run pio_loop.py          # loads overlay + runs sanity check
    audio = init_audio(ol)    # configure codec (call once)
    process_audio_block(audio, seconds=3.0)  # record→Effect IP→play

Audio architecture:
    - Codec I2C config : PS hardware IIC1 (/dev/i2c-1) via libaudio.so
    - Audio data path  : audio_codec_ctrl (I2S) ↔ libaudio.so DMA ↔ numpy buffer
    - Effect processing: per-sample via AXI-Lite MMIO to process_sample IP

Phase 1 is batch mode (not real-time):
    record N seconds → process each sample through Effect IP → play back.
    Real-time per-sample DMA path is Phase 6 (A→B upgrade).

If init_audio() fails with "Cannot find UIO device audio-codec-ctrl":
    The UIO device is created from the device tree when the overlay loads.
    If it is missing, load the base overlay first (which registers the UIO),
    then reload bass_fx.bit. The codec retains its I2C config across overlay swaps.
"""

from pynq import Overlay, MMIO
from pynq.lib.audio import AudioADAU1761
import numpy as np
import time

# ------------------------------------------------------------------ #
#  Address map (from xprocess_sample_hw.h + bass_fx_bd.hwh)          #
# ------------------------------------------------------------------ #

# Effect IP (process_sample_0)
EFFECT_BASE   = 0x40020000
EFFECT_RANGE  = 0x10000

ADDR_AP_CTRL     = 0x00   # bit0=ap_start, bit1=ap_done, bit2=ap_idle
ADDR_IN_L        = 0x10   # write: 24-bit Q1.23 left input
ADDR_IN_R        = 0x18   # write: 24-bit Q1.23 right input
ADDR_OUT_L       = 0x20   # read:  24-bit Q1.23 left output
ADDR_OUT_L_VLD   = 0x24   # read:  out_l valid flag (clear on read)
ADDR_OUT_R       = 0x30   # read:  24-bit Q1.23 right output
ADDR_OUT_R_VLD   = 0x34   # read:  out_r valid flag (clear on read)
ADDR_DIST_EN     = 0x40   # write: bit0 = distortion enable
ADDR_WOBBLE_EN   = 0x48   # write: bit0 = wobble enable
ADDR_THRESHOLD   = 0x50   # write: distortion threshold (int)
ADDR_GAIN        = 0x58   # write: distortion gain (int)
ADDR_LFO_RATE    = 0x60   # write: wobble LFO rate (int)
ADDR_LFO_DEPTH   = 0x68   # write: wobble LFO depth (int)

# AXI GPIO
GPIO0_BASE  = 0x40000000  # sw[1:0] on DATA (0x000), btn[3:0] on DATA2 (0x008)
GPIO1_BASE  = 0x40010000  # led[3:0] on DATA (0x000)
GPIO_RANGE  = 0x10000
GPIO_DATA   = 0x000
GPIO_DATA2  = 0x008
GPIO_TRI    = 0x004       # tristate: 1=input, 0=output
GPIO_TRI2   = 0x00C

# ------------------------------------------------------------------ #
#  Q1.23 helpers                                                      #
# ------------------------------------------------------------------ #

def float_to_q123(f):
    """Convert float in [-1, +1) to 24-bit Q1.23 integer stored in 32-bit word."""
    v = int(f * (1 << 23))
    return v & 0xFFFFFF  # keep lower 24 bits (two's complement)

def q123_to_float(raw):
    """Convert 24-bit Q1.23 register value (in lower 24 bits) back to float."""
    raw24 = raw & 0xFFFFFF
    if raw24 & 0x800000:       # sign-extend if negative
        raw24 -= 0x1000000
    return raw24 / (1 << 23)

# ------------------------------------------------------------------ #
#  Overlay and MMIO init                                              #
# ------------------------------------------------------------------ #

print("Loading overlay...")
ol = Overlay("/home/xilinx/jupyter_notebooks/bass_fx/bass_fx.bit")
print("Overlay loaded.")

effect = MMIO(EFFECT_BASE, EFFECT_RANGE)
gpio0  = MMIO(GPIO0_BASE,  GPIO_RANGE)
gpio1  = MMIO(GPIO1_BASE,  GPIO_RANGE)

# Configure GPIO tristate: sw and btn are inputs (TRI=1), led is output (TRI=0)
gpio0.write(GPIO_TRI,  0xFF)   # sw: all input
gpio0.write(GPIO_TRI2, 0xFF)   # btn: all input
gpio1.write(GPIO_TRI,  0x00)   # led: all output

# Phase 1: both effects off (passthrough)
effect.write(ADDR_DIST_EN,   0)
effect.write(ADDR_WOBBLE_EN, 0)

# ------------------------------------------------------------------ #
#  Effect IP helpers                                                  #
# ------------------------------------------------------------------ #

def run_effect(sample_l, sample_r):
    """
    Write one stereo sample to the Effect IP, trigger it, and read back output.
    sample_l / sample_r: float in [-1, +1)
    Returns (out_l, out_r) as floats.
    """
    effect.write(ADDR_IN_L, float_to_q123(sample_l))
    effect.write(ADDR_IN_R, float_to_q123(sample_r))
    effect.write(ADDR_AP_CTRL, 0x01)               # ap_start

    # Poll ap_done (bit 1)
    for _ in range(1000):
        if effect.read(ADDR_AP_CTRL) & 0x02:
            break

    out_l = q123_to_float(effect.read(ADDR_OUT_L))
    out_r = q123_to_float(effect.read(ADDR_OUT_R))
    return out_l, out_r

def read_sw():
    """Return sw[1:0] as integer (bit0=sw0, bit1=sw1)."""
    return gpio0.read(GPIO_DATA) & 0x3

def read_btn():
    """Return btn[3:0] as integer."""
    return gpio0.read(GPIO_DATA2) & 0xF

def write_led(val):
    """Write led[3:0] (bit0=led0 ... bit3=led3)."""
    gpio1.write(GPIO_DATA, val & 0xF)

# ------------------------------------------------------------------ #
#  Codec init                                                         #
# ------------------------------------------------------------------ #

def init_audio(ol, iic_index=1, uio_name="audio-codec-ctrl"):
    """
    Configure the ADAU1761 codec and select line-in input.

    Parameters
    ----------
    ol         : loaded Overlay object
    iic_index  : PS IIC bus index (1 = /dev/i2c-1 on PYNQ-Z2)
    uio_name   : UIO device name registered in device tree for audio IP

    Returns
    -------
    AudioADAU1761 object ready for record() / play()

    Notes
    -----
    configure() internally calls config_audio_pll + config_audio_codec
    via libaudio.so, which talk to the codec over PS hardware I2C.
    It also looks up uio_name in /dev/uio* for DMA interrupt handling.

    If ValueError "Cannot find UIO device" is raised, load the base
    overlay first so its device tree entry creates /dev/uio*, then
    reload bass_fx.bit (codec I2C state is preserved in hardware).
    """
    audio = ol.audio_codec_ctrl_0
    audio.configure(sample_rate=48000, iic_index=iic_index, uio_name=uio_name)
    audio.select_line_in()
    print("Codec configured: 48 kHz stereo, line-in selected.")
    return audio


# ------------------------------------------------------------------ #
#  Batch audio processing (Phase 1 Exit Criteria verification)        #
# ------------------------------------------------------------------ #

def process_audio_block(audio, seconds=3.0):
    """
    Record audio, route each sample through the Effect IP, then play back.

    This is the Phase 1 passthrough verification:
      audio in  →  Effect IP (passthrough mode)  →  audio out
    should sound identical to audio.bypass(seconds).

    Buffer layout (from pynq.lib.audio):
      int32 array, interleaved stereo: [L0, R0, L1, R1, ...]
      Each value is 24-bit signed sample in lower 24 bits (Q1.23).

    Parameters
    ----------
    audio   : AudioADAU1761 object from init_audio()
    seconds : recording duration (also determines playback duration)
    """
    print(f"Recording {seconds:.1f}s...")
    audio.record(seconds)

    buf = audio.buffer          # int32 numpy array, interleaved L/R
    n_frames = audio.sample_len
    out_buf = np.zeros_like(buf)

    print(f"Processing {n_frames} stereo frames through Effect IP...")
    t0 = time.time()
    for i in range(n_frames):
        in_l = buf[2 * i]     / (1 << 23)   # Q1.23 → float
        in_r = buf[2 * i + 1] / (1 << 23)
        fl, fr = run_effect(in_l, in_r)
        out_buf[2 * i]     = int(fl * (1 << 23))
        out_buf[2 * i + 1] = int(fr * (1 << 23))
    elapsed = time.time() - t0

    print(f"Done: {elapsed:.2f}s elapsed  ({n_frames / elapsed:.0f} frames/s)")
    print("Playing back...")
    audio.buffer[:] = out_buf
    audio.play()
    print("Playback complete.")

# ------------------------------------------------------------------ #
#  Main                                                               #
# ------------------------------------------------------------------ #

if __name__ == "__main__":
    print("Addresses confirmed:")
    print(f"  Effect IP  : {hex(EFFECT_BASE)}")
    print(f"  AXI GPIO 0 : {hex(GPIO0_BASE)}  (sw / btn)")
    print(f"  AXI GPIO 1 : {hex(GPIO1_BASE)}  (led)")
    print()

    # --- Effect IP sanity check ---
    print("Effect IP sanity check (0.5, -0.5):")
    out_l, out_r = run_effect(0.5, -0.5)
    ok_l = "OK" if abs(out_l - 0.5)  < 0.001 else "MISMATCH"
    ok_r = "OK" if abs(out_r + 0.5)  < 0.001 else "MISMATCH"
    print(f"  in_l= 0.5000  out_l={out_l:.4f}  {ok_l}")
    print(f"  in_r=-0.5000  out_r={out_r:.4f}  {ok_r}")
    print()

    # --- Codec init ---
    print("Initialising codec...")
    try:
        audio = init_audio(ol)
    except (ValueError, AttributeError) as e:
        print(f"  Codec init failed: {e}")
        print("  Workaround: load base overlay first, then reload bass_fx.bit.")
        audio = None

    # --- Phase 1 Exit Criteria test ---
    if audio is not None:
        print()
        print("Starting Phase 1 audio passthrough test (3 seconds).")
        print("Play audio into line-in now...")
        time.sleep(1)
        process_audio_block(audio, seconds=3.0)
        print()
        print("Phase 1 Exit Criteria: compare playback to audio.bypass(3.0).")
        print("They should sound identical.")
    else:
        print("Skipping audio test. Fix codec init first.")
