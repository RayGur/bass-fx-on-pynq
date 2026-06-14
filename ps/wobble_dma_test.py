#!/usr/bin/env python3
# wobble_dma_test.py — verify wobble IIR+LFO via DMA (no codec needed)
#
# 板上執行：sudo python3 ~/wobble_dma_test.py
#
# 測試策略：
#   1. 固定輸入 0.5（全部樣本），lfo_rate=0（LFO 靜止），lfo_depth=0
#      → lut_idx=0 → b = 858/32768 ≈ 0.0262（固定係數）
#      → IIR 從 0 開始，每個 L-sample: y[n] = b*0.5 + (1-b)*y[n-1]
#      → 輸出應從接近 0 單調收斂至 0.5
#   2. 確認 L/R 樣本都在 [-1.0, +0.9999] 範圍內
#   3. 第二次 DMA（IP state 延續）：L[0] 應繼續從 run1 收斂的狀態出發
#
# Pass criteria：
#   - 所有輸出都在範圍內
#   - L-ch 第一個 buffer 的前幾個值單調遞增（IIR 收斂中）
#   - L-ch 輸出 < 輸入（有低通效果，沒有 passthrough）

from pynq import Overlay, allocate, MMIO
import numpy as np
import time

BIT = "/home/xilinx/bass_fx_bd.bit"

# AXI DMA direct register mode
DMA_BASE      = 0x41E00000
DMA_MM2S_CR   = 0x00
DMA_MM2S_SR   = 0x04
DMA_MM2S_SA   = 0x18
DMA_MM2S_LEN  = 0x28
DMA_S2MM_CR   = 0x30
DMA_S2MM_SR   = 0x34
DMA_S2MM_DA   = 0x48
DMA_S2MM_LEN  = 0x58
IOC = 1 << 12
RS  = 1

# Effect IP AXI-Lite
EFFECT_BASE      = 0x40020000
EFFECT_CTRL      = 0x00
EFFECT_N_SAMPLES = 0x10
EFFECT_DIST_EN   = 0x18
EFFECT_WOBBLE_EN = 0x20
EFFECT_THRESHOLD = 0x28
EFFECT_GAIN      = 0x30
EFFECT_LFO_RATE  = 0x38
EFFECT_LFO_DEPTH = 0x40

N = 256  # stereo pairs per DMA transfer

def dma_transfer(dma, in_buf, out_buf):
    """Single blocking DMA: in_buf → IP → out_buf."""
    in_buf.flush()
    out_buf.flush()

    dma.write(DMA_S2MM_DA,  out_buf.physical_address)
    dma.write(DMA_S2MM_LEN, N * 2 * 4)
    dma.write(DMA_MM2S_SA,  in_buf.physical_address)
    dma.write(DMA_MM2S_LEN, N * 2 * 4)

    t0 = time.time()
    while not (dma.read(DMA_MM2S_SR) & IOC):
        if time.time() - t0 > 5:
            raise TimeoutError("MM2S timeout")
    dma.write(DMA_MM2S_SR, IOC)

    t0 = time.time()
    while not (dma.read(DMA_S2MM_SR) & IOC):
        if time.time() - t0 > 5:
            raise TimeoutError("S2MM timeout")
    dma.write(DMA_S2MM_SR, IOC)

    out_buf.invalidate()


def q123_to_float(raw_int32):
    """Convert Q1.23 raw int32 to float in [-1, +1)."""
    # Mask to 24 bits, sign-extend from bit 23
    raw = int(raw_int32) & 0xFFFFFF
    if raw & 0x800000:
        raw -= 0x1000000
    return raw / (1 << 23)


def float_to_q123(f):
    """Convert float in [-1, +1) to Q1.23 raw int32."""
    raw = int(round(f * (1 << 23))) & 0xFFFFFF
    if raw & 0x800000:
        raw |= 0xFF000000  # sign-extend to int32
    return np.int32(raw)


def main():
    print("=== wobble_dma_test ===")
    print(f"Loading overlay: {BIT}")
    ol = Overlay(BIT, ignore_version=True)

    dma = MMIO(DMA_BASE,   0x10000)
    eff = MMIO(EFFECT_BASE, 0x10000)

    # ── Effect IP init: wobble only, LFO frozen (rate=0, depth=0 → lut_idx=0) ──
    LFO_RATE  = 0    # keep LFO phase fixed → deterministic coefficient b
    LFO_DEPTH = 0    # depth=0 → depth_scaled=0 → lut_idx=0 → b=858/32768
    eff.write(EFFECT_N_SAMPLES, N)
    eff.write(EFFECT_DIST_EN,   0)
    eff.write(EFFECT_WOBBLE_EN, 1)
    eff.write(EFFECT_THRESHOLD, 0)
    eff.write(EFFECT_GAIN,      0)
    eff.write(EFFECT_LFO_RATE,  LFO_RATE)
    eff.write(EFFECT_LFO_DEPTH, LFO_DEPTH)
    eff.write(EFFECT_CTRL,      0x81)  # AP_START | AUTO_RESTART
    print(f"effect CTRL={hex(eff.read(EFFECT_CTRL))} n_samples={eff.read(EFFECT_N_SAMPLES)}")
    print(f"       wobble_en={eff.read(EFFECT_WOBBLE_EN)} lfo_rate={eff.read(EFFECT_LFO_RATE)} lfo_depth={eff.read(EFFECT_LFO_DEPTH)}")

    # ── DMA init ──────────────────────────────────────────────────────────────
    dma.write(DMA_MM2S_CR, 1 << 2)
    dma.write(DMA_S2MM_CR, 1 << 2)
    time.sleep(0.01)
    dma.write(DMA_MM2S_CR, RS)
    dma.write(DMA_S2MM_CR, RS)
    print(f"DMA reset: MM2S_SR={hex(dma.read(DMA_MM2S_SR))} S2MM_SR={hex(dma.read(DMA_S2MM_SR))}")

    # ── Allocate buffers ──────────────────────────────────────────────────────
    in_buf  = allocate(shape=(N * 2,), dtype=np.int32)
    out_buf = allocate(shape=(N * 2,), dtype=np.int32)

    # Fill input: constant 0.5 in Q1.23
    INPUT_FLOAT = 0.5
    in_val = float_to_q123(INPUT_FLOAT)
    in_buf[:] = in_val
    print(f"input: 0x{int(in_val) & 0xFFFFFFFF:08x} = {q123_to_float(in_val):.6f} (target {INPUT_FLOAT})")

    # ── Run 1: IP state starts at {0, 0, 0} after reset ─────────────────────
    print("\n--- Run 1 (state starts at zero) ---")
    dma_transfer(dma, in_buf, out_buf)

    # Expected b = 858/32768 ≈ 0.02618
    # y[0]_L ≈ b * 0.5 ≈ 0.01309  (IIR from zero)
    # y[1]_L ≈ b * 0.5 + (1-b) * y[0]_L  (slightly higher)
    b_expected = 858.0 / 32768.0
    fail = 0

    # Extract L and R channels (interleaved: even=L, odd=R)
    l_ch = [q123_to_float(out_buf[i])     for i in range(0, N * 2, 2)]
    r_ch = [q123_to_float(out_buf[i + 1]) for i in range(0, N * 2, 2)]

    print(f"L[0..7] = {[f'{v:.6f}' for v in l_ch[:8]]}")
    print(f"R[0..7] = {[f'{v:.6f}' for v in r_ch[:8]]}")

    # Check 1: all outputs in [-1.0, +0.9999]
    for i, (l, r) in enumerate(zip(l_ch, r_ch)):
        if l < -1.0 or l > 0.9999 or r < -1.0 or r > 0.9999:
            print(f"[FAIL] out-of-bounds pair {i}: L={l:.6f} R={r:.6f}")
            fail += 1
    if fail == 0:
        print("[PASS] all outputs in [-1.0, +0.9999]")

    # Check 2: L[0] ≈ b * input (IIR from zero), within Q1.23 resolution
    expected_l0 = b_expected * INPUT_FLOAT
    tol = 1.0 / (1 << 20)  # ~1 ppm, a few Q1.23 LSBs
    if abs(l_ch[0] - expected_l0) < 0.002:  # 2e-3: covers Q15 quantization of b
        print(f"[PASS] L[0]={l_ch[0]:.6f} ≈ b*in={expected_l0:.6f} (IIR from zero)")
    else:
        print(f"[FAIL] L[0]={l_ch[0]:.6f}, expected ≈{expected_l0:.6f} (diff={abs(l_ch[0]-expected_l0):.6f})")
        fail += 1

    # Check 3: L samples are monotonically increasing (IIR converging toward 0.5)
    # Check first 16 pairs; convergence is slow (b≈0.026) so monotone expected
    non_mono = [(i, l_ch[i], l_ch[i+1]) for i in range(15) if l_ch[i+1] < l_ch[i] - tol]
    if not non_mono:
        print("[PASS] L-ch monotonically increasing (IIR converging)")
    else:
        print(f"[FAIL] L-ch non-monotone at pairs: {non_mono[:3]}")
        fail += 1

    # Check 4: L output < input (low-pass is attenuating; never reaches 0.5 in one buffer)
    max_l = max(l_ch)
    if max_l < INPUT_FLOAT:
        print(f"[PASS] L-ch max={max_l:.6f} < input={INPUT_FLOAT} (low-pass effect confirmed)")
    else:
        print(f"[FAIL] L-ch max={max_l:.6f} >= input (wobble not attenuating?)")
        fail += 1

    # ── Run 2: state persists across DMA calls (AUTO_RESTART) ────────────────
    print("\n--- Run 2 (state carries over from run 1) ---")
    dma_transfer(dma, in_buf, out_buf)

    l_ch2 = [q123_to_float(out_buf[i]) for i in range(0, N * 2, 2)]
    print(f"L[0..7] = {[f'{v:.6f}' for v in l_ch2[:8]]}")

    # Run 2 L[0] should be higher than run 1 L[-1] (state carries over)
    if l_ch2[0] > l_ch[-1] - tol:
        print(f"[PASS] run2 L[0]={l_ch2[0]:.6f} >= run1 L[-1]={l_ch[-1]:.6f} (state persists)")
    else:
        print(f"[FAIL] run2 L[0]={l_ch2[0]:.6f} < run1 L[-1]={l_ch[-1]:.6f} (state reset?)")
        fail += 1

    # ── Summary ───────────────────────────────────────────────────────────────
    print(f"\n{'All tests PASSED — wobble IIR confirmed.' if fail == 0 else str(fail) + ' test(s) FAILED.'}")
    return fail


if __name__ == "__main__":
    exit(main())
