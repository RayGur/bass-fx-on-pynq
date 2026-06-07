"""
pio_loop.py -- Phase 1 PS-side PIO audio loop
PYNQ-Z2, PYNQ 2.5 (Glasgow), Ubuntu 18.04

Usage (Jupyter Notebook or on-board Python):
    %run pio_loop.py          # loads overlay + runs Effect IP sanity check
    init_codec_via_axiic(ol)  # configure ADAU1761 via AXI IIC (call once)
    audio = init_audio(ol)    # returns AudioADAU1761 ready for record/play
    process_audio_block(audio, seconds=3.0)

Audio architecture (Phase 1):
    - Codec I2C config  : ol.axi_iic_0 (AxiIIC, direct AXI register access)
    - Audio data path   : audio_codec_ctrl (I2S) <-> libaudio.so UIO <-> numpy buffer
    - Effect processing : per-sample via AXI-Lite MMIO to process_sample IP

Why monkey-patch AudioADAU1761.configure():
    configure() calls libaudio.config_audio_pll(iic_index) which polls PLL lock
    over /dev/i2c-X.  ADAU1761's I2C is on PL pins U9/T9, reachable only via
    AXI IIC in PL -- the Linux i2c-xiic kernel driver is NOT loaded (PYNQ 2.5
    does not auto-create a DT entry for axi_iic).  We skip the libaudio I2C
    calls and do the same work ourselves via AxiIIC.

    libaudio.record() does not write I2C during recording (only opens the fd).
    libaudio.play() writes mixer enable/disable via I2C -- those writes fail
    silently (wrong bus), but we pre-enable the mixers via AxiIIC so audio
    comes out regardless.
"""

import math
import time
import numpy as np
from pynq import Overlay, MMIO
from pynq.lib.audio import AudioADAU1761
from pynq.uio import get_uio_index

# ------------------------------------------------------------------ #
#  Monkey-patch AudioADAU1761.configure() BEFORE Overlay() is called  #
#  (Overlay.__init__ triggers AudioADAU1761.__init__ → configure())   #
# ------------------------------------------------------------------ #

def _configure_skip_iic(self, sample_rate=48000,
                         iic_index=0, uio_name="audio-codec-ctrl"):
    """
    Replacement for AudioADAU1761.configure().
    Sets up iic_index / uio_index but skips libaudio I2C calls.
    Codec I2C init is done separately via init_codec_via_axiic().

    iic_index=0 → /dev/i2c-0 (PS Cadence IIC, physically cannot reach
    ADAU1761 on PL pins U9/T9).  libaudio will open and fail silently;
    UIO-based audio data path is unaffected.
    """
    self.sample_rate = sample_rate
    self.iic_index = iic_index
    self.uio_index = get_uio_index(uio_name)
    if self.uio_index is None:
        raise ValueError("Cannot find UIO device {}".format(uio_name))

AudioADAU1761.configure = _configure_skip_iic

# ------------------------------------------------------------------ #
#  Address map (from xprocess_sample_hw.h + bass_fx_bd.hwh)          #
# ------------------------------------------------------------------ #

EFFECT_BASE   = 0x40020000
EFFECT_RANGE  = 0x10000

ADDR_AP_CTRL     = 0x00
ADDR_IN_L        = 0x10
ADDR_IN_R        = 0x18
ADDR_OUT_L       = 0x20
ADDR_OUT_L_VLD   = 0x24
ADDR_OUT_R       = 0x30
ADDR_OUT_R_VLD   = 0x34
ADDR_DIST_EN     = 0x40
ADDR_WOBBLE_EN   = 0x48
ADDR_THRESHOLD   = 0x50
ADDR_GAIN        = 0x58
ADDR_LFO_RATE    = 0x60
ADDR_LFO_DEPTH   = 0x68

GPIO0_BASE  = 0x40000000
GPIO1_BASE  = 0x40010000
GPIO_RANGE  = 0x10000
GPIO_DATA   = 0x000
GPIO_DATA2  = 0x008
GPIO_TRI    = 0x004
GPIO_TRI2   = 0x00C

# ------------------------------------------------------------------ #
#  Overlay load                                                        #
# ------------------------------------------------------------------ #

print("Loading overlay...")
ol = Overlay("/home/xilinx/jupyter_notebooks/bass_fx/bass_fx.bit",
             ignore_version=True)
print("Overlay loaded.")

effect = MMIO(EFFECT_BASE, EFFECT_RANGE)
gpio0  = MMIO(GPIO0_BASE,  GPIO_RANGE)
gpio1  = MMIO(GPIO1_BASE,  GPIO_RANGE)

gpio0.write(GPIO_TRI,  0xFF)
gpio0.write(GPIO_TRI2, 0xFF)
gpio1.write(GPIO_TRI,  0x00)

effect.write(ADDR_DIST_EN,   0)
effect.write(ADDR_WOBBLE_EN, 0)

# ------------------------------------------------------------------ #
#  Q1.23 helpers                                                      #
# ------------------------------------------------------------------ #

def float_to_q123(f):
    v = int(f * (1 << 23))
    return v & 0xFFFFFF

def q123_to_float(raw):
    raw24 = raw & 0xFFFFFF
    if raw24 & 0x800000:
        raw24 -= 0x1000000
    return raw24 / (1 << 23)

# ------------------------------------------------------------------ #
#  Effect IP helpers                                                  #
# ------------------------------------------------------------------ #

def run_effect(sample_l, sample_r):
    effect.write(ADDR_IN_L, float_to_q123(sample_l))
    effect.write(ADDR_IN_R, float_to_q123(sample_r))
    effect.write(ADDR_AP_CTRL, 0x01)
    for _ in range(1000):
        if effect.read(ADDR_AP_CTRL) & 0x02:
            break
    out_l = q123_to_float(effect.read(ADDR_OUT_L))
    out_r = q123_to_float(effect.read(ADDR_OUT_R))
    return out_l, out_r

def read_sw():
    return gpio0.read(GPIO_DATA) & 0x3

def read_btn():
    return gpio0.read(GPIO_DATA2) & 0xF

def write_led(val):
    gpio1.write(GPIO_DATA, val & 0xF)

# ------------------------------------------------------------------ #
#  ADAU1761 codec init via AXI IIC                                    #
# ------------------------------------------------------------------ #

_CODEC_ADDR = 0x3B  # ADAU1761 I2C slave address

def _iic_write(iic, reg_addr, value):
    """Write one ADAU1761 register: sends [0x40, reg_addr, value]."""
    ffi = iic._ffi
    tx = ffi.new("unsigned char[3]", [0x40, reg_addr, value])
    iic.send(_CODEC_ADDR, tx, 3)


def init_codec_via_axiic(ol):
    """
    Configure ADAU1761 using ol.axi_iic_0 (AxiIIC, direct AXI access).

    Replicates the full libaudio.so sequence:
      config_audio_pll  -- 10 MHz MCLK → 49.152 MHz PLL → 48 kHz
      config_audio_codec -- register init
      select_line_in
    Then pre-enables HP output mixers so libaudio.play() works even
    when its I2C writes to /dev/i2c-0 fail silently.
    """
    iic = ol.axi_iic_0
    ffi = iic._ffi

    # ---------- config_audio_pll ----------
    # Disable core clock
    _iic_write(iic, 0x00, 0x0E)

    # PLL config: M=625(0x0271), N=572(0x023C), R=4, X=1
    pll = ffi.new("unsigned char[8]",
                  [0x40, 0x02, 0x02, 0x71, 0x02, 0x3C, 0x21, 0x03])
    iic.send(_CODEC_ADDR, pll, 8)

    # Poll PLL lock (bit 1 of 6th read byte)
    ptr   = ffi.new("unsigned char[2]", [0x40, 0x02])
    rxbuf = ffi.new("unsigned char[6]")
    locked = False
    for _ in range(200):
        iic.send(_CODEC_ADDR, ptr, 2, 1)   # REPEAT_START=1
        iic.receive(_CODEC_ADDR, rxbuf, 6)
        if rxbuf[5] & 0x02:
            locked = True
            break
        time.sleep(0.01)
    if not locked:
        raise RuntimeError("ADAU1761 PLL did not lock — check MCLK (U5) and I2C (U9/T9)")

    # Enable core clock with PLL source
    _iic_write(iic, 0x00, 0x0F)
    print("  PLL locked.")

    # ---------- config_audio_codec ----------
    _iic_write(iic, 0x0A, 0x00)  # R4:  mute record mixer left
    _iic_write(iic, 0x0C, 0x00)  # R6:  mute record mixer right
    _iic_write(iic, 0x0E, 0xB3)  # R8:  LDVOL=21dB, left diff enable
    _iic_write(iic, 0x0F, 0xB3)  # R9:  RDVOL=21dB, right diff enable
    _iic_write(iic, 0x10, 0x01)  # R10: MIC bias enable
    _iic_write(iic, 0x14, 0x20)  # R14: ALC control
    _iic_write(iic, 0x15, 0x01)  # R15: serial port master mode
    _iic_write(iic, 0x19, 0x33)  # R19: ADC enable both channels
    _iic_write(iic, 0x1C, 0x00)  # R22: mute playback mixer left
    _iic_write(iic, 0x1D, 0x00)  # R23: mute left input to mixer3
    _iic_write(iic, 0x1E, 0x00)  # R24: mute playback mixer right
    _iic_write(iic, 0x1F, 0x00)  # R25: mute right input to mixer4
    _iic_write(iic, 0x23, 0xE5)  # R29: HP left (muted)
    _iic_write(iic, 0x24, 0xE5)  # R30: HP right (muted)
    _iic_write(iic, 0x29, 0x03)  # R35: playback power L+R
    _iic_write(iic, 0x2A, 0x03)  # R36: DAC enable L+R
    _iic_write(iic, 0xF2, 0x01)  # R58: SDATA_In → DAC
    _iic_write(iic, 0xF3, 0x01)  # R59: SDATA_Out → ADC
    _iic_write(iic, 0xF5, 0x01)  # R61: DSP enable
    _iic_write(iic, 0xF6, 0x01)  # R62: DSP run
    _iic_write(iic, 0xF9, 0x7F)  # R65: clock enable 0
    _iic_write(iic, 0xFA, 0x03)  # R66: clock enable 1

    # ---------- select_line_in ----------
    _iic_write(iic, 0x0A, 0x01)  # R4:  enable mixer1 left
    _iic_write(iic, 0x0B, 0x07)  # R5:  LAUX gain 0 dB
    _iic_write(iic, 0x0C, 0x01)  # R6:  enable mixer2 right
    _iic_write(iic, 0x0D, 0x07)  # R7:  RAUX gain 0 dB

    # ---------- pre-enable HP output ----------
    # libaudio.play() tries these writes but /dev/i2c-0 can't reach ADAU1761;
    # write_audio_reg() prints error and continues -- audio data still flows
    # via UIO.  We pre-enable here so output is active regardless.
    _iic_write(iic, 0x1C, 0x21)  # R22: mixer3 ← DAC left
    _iic_write(iic, 0x1E, 0x41)  # R24: mixer4 ← DAC right
    _iic_write(iic, 0x23, 0xE7)  # R29: HP left unmuted
    _iic_write(iic, 0x24, 0xE7)  # R30: HP right unmuted

    print("  Codec configured: 48 kHz, line-in, HP out enabled.")


# ------------------------------------------------------------------ #
#  Codec init (public)                                                #
# ------------------------------------------------------------------ #

def init_audio(ol, uio_name="audio-codec-ctrl"):
    """
    Configure ADAU1761 and return a ready AudioADAU1761 object.

    Steps:
      1. init_codec_via_axiic(ol)  -- I2C config via AXI IIC
      2. ol.audio_codec_ctrl_0     -- already has patched configure()
         with iic_index=0 and correct uio_index

    iic_index=0 is /dev/i2c-0 (PS Cadence IIC).  libaudio opens it
    successfully but I2C writes to ADAU1761 fail silently because the
    physical bus (U9/T9) is routed through AXI IIC, not PS IIC.
    Audio data flows via UIO regardless.
    """
    print("Initialising codec...")
    init_codec_via_axiic(ol)
    audio = ol.audio_codec_ctrl_0
    # Sanity-check UIO
    if audio.uio_index is None:
        raise ValueError("audio-codec-ctrl UIO not found after overlay load")
    print("Codec ready. uio_index={}, iic_index={}".format(
        audio.uio_index, audio.iic_index))
    return audio

# ------------------------------------------------------------------ #
#  Direct I2S MMIO helpers (bypass libaudio record/play)             #
# ------------------------------------------------------------------ #
# audio_codec_ctrl register offsets (from audio_adau1761.h)
# MMIO.array is uint32[], so index = byte_offset // 4
_RX_L = 0   # 0x00: I2S_DATA_RX_L_REG
_RX_R = 1   # 0x04: I2S_DATA_RX_R_REG
_TX_L = 2   # 0x08: I2S_DATA_TX_L_REG
_TX_R = 3   # 0x0C: I2S_DATA_TX_R_REG
_STAT = 4   # 0x10: I2S_STATUS_REG  (non-zero = new frame ready; write 1 to clear)

_I2S_TIMEOUT = 200000  # poll iterations before giving up (~4× a 48kHz period in Python)


def _wait_frame(arr):
    """Wait for next I2S frame, clear status. Raises on timeout."""
    for _ in range(_I2S_TIMEOUT):
        if arr[_STAT] != 0:
            arr[_STAT] = 1
            return
    raise RuntimeError("I2S timeout: no audio clock from codec (check PLL/MCLK)")


def py_record(audio_ip, n_frames):
    """
    Record n_frames stereo samples via direct MMIO on audio_codec_ctrl.
    Returns int32 numpy array of length n_frames*2 (interleaved L/R, Q1.23).
    Bypasses libaudio.so record() to avoid UIO-related kernel crashes.
    """
    arr = audio_ip.mmio.array          # uint32 numpy array, mmap-backed
    buf = np.zeros(n_frames * 2, dtype=np.int32)
    for i in range(n_frames):
        _wait_frame(arr)
        buf[2 * i]     = np.int32(arr[_RX_L])
        buf[2 * i + 1] = np.int32(arr[_RX_R])
    return buf


def py_play(audio_ip, buf, n_frames):
    """
    Play n_frames stereo samples via direct MMIO on audio_codec_ctrl.
    buf: int32 numpy array, interleaved L/R, Q1.23.
    Bypasses libaudio.so play() to avoid UIO-related kernel crashes.
    """
    arr = audio_ip.mmio.array
    for i in range(n_frames):
        _wait_frame(arr)
        arr[_TX_L] = np.uint32(buf[2 * i])
        arr[_TX_R] = np.uint32(buf[2 * i + 1])


# ------------------------------------------------------------------ #
#  Batch audio processing (Phase 1 Exit Criteria verification)        #
# ------------------------------------------------------------------ #

def process_audio_block(audio_ip, seconds=3.0):
    """
    Record → Effect IP (passthrough) → play.
    Should sound identical to a bypass.

    Parameters
    ----------
    audio_ip : AudioADAU1761
        ol.audio_codec_ctrl_0 (returned by init_audio())
    seconds  : float
        Duration to record and play back.
    """
    n_frames = int(seconds * 48000)
    print("Recording {:.1f}s ({} frames)...".format(seconds, n_frames))
    buf = py_record(audio_ip, n_frames)
    print("Recording done.")

    out_buf = np.zeros_like(buf)
    print("Processing through Effect IP...")
    t0 = time.time()
    for i in range(n_frames):
        in_l = buf[2 * i]     / (1 << 23)
        in_r = buf[2 * i + 1] / (1 << 23)
        fl, fr = run_effect(in_l, in_r)
        out_buf[2 * i]     = int(fl * (1 << 23))
        out_buf[2 * i + 1] = int(fr * (1 << 23))
    elapsed = time.time() - t0
    print("Done: {:.2f}s  ({:.0f} frames/s)".format(elapsed, n_frames / elapsed))

    print("Playing back...")
    py_play(audio_ip, out_buf, n_frames)
    print("Playback complete.")

# ------------------------------------------------------------------ #
#  Main                                                               #
# ------------------------------------------------------------------ #

if __name__ == "__main__":
    print("Addresses:")
    print("  Effect IP  : {}".format(hex(EFFECT_BASE)))
    print("  AXI GPIO 0 : {}  (sw / btn)".format(hex(GPIO0_BASE)))
    print("  AXI GPIO 1 : {}  (led)".format(hex(GPIO1_BASE)))
    print()

    # Effect IP sanity check
    print("Effect IP sanity check (0.5, -0.5):")
    out_l, out_r = run_effect(0.5, -0.5)
    ok_l = "OK" if abs(out_l - 0.5)  < 0.001 else "MISMATCH"
    ok_r = "OK" if abs(out_r + 0.5)  < 0.001 else "MISMATCH"
    print("  in_l= 0.5000  out_l={:.4f}  {}".format(out_l, ok_l))
    print("  in_r=-0.5000  out_r={:.4f}  {}".format(out_r, ok_r))
    print()

    # Codec init + audio test
    try:
        audio = init_audio(ol)
    except Exception as e:
        print("Codec init failed: {}".format(e))
        audio = None

    if audio is not None:
        print()
        print("Phase 1 audio passthrough test (3 seconds).")
        print("Play audio into line-in now...")
        time.sleep(1)
        process_audio_block(audio, seconds=3.0)
        print()
        print("Phase 1 Exit Criteria: playback should match a bypass.")
    else:
        print("Fix codec init before audio test.")
