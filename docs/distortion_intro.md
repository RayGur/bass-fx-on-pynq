# Distortion 效果介紹

本文件介紹 `bass-fx-on-pynq` 專案中 distortion 效果的完整訊號處理鏈，包含前置濾波器與 clipping 核心算法。

---

## 系統架構概覽

Distortion 效果運行在 FPGA（PL 端），以 Vitis HLS 合成為 AXI-Stream IP。PS（ARM）透過 AXI-Lite 暫存器即時調整參數。

```
ADAU1761 Codec (48 kHz, 24-bit)
        │
        ▼
   DMA (AXI-Stream)
        │
        ▼
┌───────────────────────────────────┐
│          process_sample (HLS IP)  │
│                                   │
│  input → Notch → HPF → [Wobble] → Distortion → output  │
│                                   │
└───────────────────────────────────┘
        │
        ▼
   DMA (AXI-Stream)
        │
        ▼
   ADAU1761 Codec (output)
```

訊號為立體聲（L/R），逐 sample 交錯處理（L₀, R₀, L₁, R₁, …）。

---

## 訊號處理鏈

Distortion 啟用時，每個 sample 依序經過以下四個步驟：

```
input → [1. Notch 60Hz] → [2. HPF 28Hz] → [3. Noise Gate] → [4. Hard Clip] → output
```

> 注意：Notch 永遠啟用；HPF 在 dist_en 或 wobble_en 任一開啟時啟用；Noise Gate 與 Hard Clip 在 dist_en 開啟時執行。

---

## 1. Notch Filter（60 Hz）

**檔案**：`hls/effect_ip/notch.cpp`

### 目的

消除電源哼聲（60 Hz hum）。若不先濾除，60 Hz 成分進入 distortion 的 gain 放大後會產生大量諧波，嚴重破壞音色，且在撥弦瞬態後留下長達數百毫秒的共鳴雜音。

### 算法：2nd-order IIR Biquad，Direct Form I

差分方程：

```
y[n] = x[n] + b1·x[n-1] + x[n-2] - a1·y[n-1] - a2·y[n-2]
```

係數（Fs = 48000 Hz，f₀ = 60 Hz）：

| 係數 | 公式 | 值 |
|------|------|----|
| b0 | 1 | 1（省略乘法）|
| b1 | -2·cos(ω₀) | -1.999938 |
| b2 | 1 | 1（省略乘法）|
| a1 | -2·r·cos(ω₀) | -1.999338 |
| a2 | r² | 0.99940009 |

其中 ω₀ = 2π·60/48000 ≈ 0.007854 rad，r = 0.9997。

**r 的選擇**：r 越接近 1，notch 越窄，但 ringing 衰減時間 τ 越長。

| r 值 | -3 dB 頻寬 | ringing τ |
|------|-----------|-----------|
| 0.9999 | 0.9 Hz | 208 ms |
| **0.9997** | **4.6 Hz** | **69 ms** |

最終選 r = 0.9997，將 τ 縮短至 69 ms，使撥弦瞬態不會激發明顯的 60 Hz 共鳴進入後段 distortion。60 Hz 精確 null 不受 r 影響，A string（55 Hz）衰減約 0.82 dB，在 distortion 下聽不出來。

### HLS 實作

```cpp
static const ap_fixed<18,2> NOTCH_B1 = -1.999938;
static const ap_fixed<18,2> NOTCH_A1 = -1.999338;
static const ap_fixed<18,2> NOTCH_A2 =  0.99940009;

ap_fixed<32,2> y = (ap_fixed<32,2>)in
                 + NOTCH_B1 * (ap_fixed<32,2>)x1
                 + (ap_fixed<32,2>)x2
                 - NOTCH_A1 * y1
                 - NOTCH_A2 * y2;
```

係數寬度選 `ap_fixed<18,2>`（而非 20-bit）可降低 DSP 乘法器消耗：18×32-bit → 2 個 DSP，20×32-bit → 3 個 DSP。

L/R 各有獨立 state（`notch_x1/x2/y1/y2_L/R`），儲存於 `state_t` 結構，跨 DMA burst 持續保留。

---

## 2. High-Pass Filter（HPF，Fc ≈ 28 Hz）

**檔案**：`hls/effect_ip/hpf.cpp`

### 目的

去除 DC offset 與次低頻雜訊。若有 DC 偏移，gain 放大後偏移量也同等放大，導致 clipping 門檻被非對稱消耗，音色不正確。

### 算法：1st-order IIR DC-blocking HPF

差分方程：

```
y[n] = α · (y[n-1] + x[n] - x[n-1])
```

其中 α = 1 - 2π·Fc/Fs ≈ 0.9963，對應 Fc ≈ 28 Hz。

### HLS 實作

```cpp
static const ap_fixed<18,1> HPF_ALPHA = 0.9963;

ap_fixed<32,2> y = HPF_ALPHA * (y_prev + (ap_fixed<32,2>)in - (ap_fixed<32,2>)x_prev);
```

L/R 各有獨立 state（`hpf_x_prev_L/R`、`hpf_y_prev_L/R`）。

---

## 3. Noise Gate（Hysteresis 雙門檻）

**檔案**：`hls/effect_ip/distortion.cpp`

### 目的

吉他靜音時，底噪（電磁干擾、類比前端雜訊）不應進入 distortion 被放大輸出。

### 為何需要 Hysteresis？

若只設單一門檻（如 0.001），note decay 時訊號幅度在門檻附近振盪，gate 反覆開關，每次過零點都被截斷，產生碎裂聲（chatter）：

```
單門檻：  ╭─╮ ╭─╮ ╭─╮   ← 反覆開關 → 碎裂聲
         0.001────────

雙門檻：  ╭──────────╮   ← 訊號衰減到 0.0003 才關門 → 自然收音
         0.001 ─ ─ ─ ─
         0.0003 ───────
```

### 算法

```
開門門檻 (open)  = 0.001  （≈ -60 dBFS）
關門門檻 (close) = 0.0003 （≈ -70 dBFS）

if gate_open:
    if |in| < 0.0003 → gate 關閉
else:
    if |in| > 0.001  → gate 開啟

if gate 關閉 → 輸出 0，跳過後續處理
```

L/R 各有獨立 gate 狀態（`dist_gate_open_L/R`），儲存於 `state_t`。

---

## 4. Hard Clipping

**檔案**：`hls/effect_ip/distortion.cpp`

### 算法

```
1. amplified = in × gain
2. threshold 解碼（Q1.23 raw bit reinterpret）
3. if amplified >  threshold → amplified = threshold
   if amplified < -threshold → amplified = -threshold
4. output = amplified
```

### 波形示意

```
原始訊號（低增益）：   ╭──╮           ╭──╮
                    ╱      ╲         ╱      ╲
              ─────╯          ╲─────╯          ╲─────

放大後（gain×N）：     ╭──────╮       ╭──────╮
                ╭────╯          ╰────╯
          ─────╯                                 ╲────

Hard Clip 後：  ──────╮           ╭──────╮
               clip  │           │       │  clip
                      ╰───────────╯       ╰──────
```

訊號超過 ±threshold 的部分一律截平，波峰從正弦波變成方波形狀，產生豐富的奇次諧波（3rd, 5th, 7th, …），這正是 distortion 音色的來源。

### 定點數處理細節

| 步驟 | 型別 | 說明 |
|------|------|------|
| 輸入 `in` | `ap_fixed<24,1>`（Q1.23）| codec 24-bit，範圍 [-1, +1) |
| 放大中間值 | `ap_fixed<32,6>`（Q6.26）| 最大 gain=20，避免 overflow |
| gain 參數 | `ap_fixed<6,6>` | 整數 1–20 |
| threshold 解碼 | `ap_int<24>` → `ap_fixed<24,1>` raw bit cast | 用 `.range()` 直接搬位元，非值轉換 |
| 輸出 | `ap_fixed<24,1>` | clip 保證 ≤ threshold ≤ 1，不會 wrap |

**threshold Q1.23 解碼說明**：PS 端將 float（如 0.3）轉成整數寫入 AXI-Lite（`int(0.3 × 2²³) = 0x2666666`）。HLS 端必須用 `.range()` 做 raw bit reinterpret，而非直接 cast（直接 cast 會做值轉換，結果錯誤）：

```cpp
ap_fixed<24, 1> thresh_q;
thresh_q.range(23, 0) = ap_int<24>(threshold);  // 正確：bit reinterpret
```

---

## 效果控制參數（AXI-Lite 暫存器）

| 參數 | 說明 | 範圍 | AXI-Lite offset |
|------|------|------|-----------------|
| `dist_en` | Distortion 開關 | 0 / 1 | 0x18 |
| `threshold` | Clipping 門檻，Q1.23 整數 | 0 – 0x7FFFFF | 0x28 |
| `gain` | 放大倍數 | 1 – 20 | 0x30 |

---

## 相關檔案索引

| 檔案 | 內容 |
|------|------|
| `hls/effect_ip/effect_ip.h` | 所有型別定義（`sample_t`, `state_t`, `param_t`）與函式宣告 |
| `hls/effect_ip/distortion.cpp` | Noise gate + Hard clipping 實作 |
| `hls/effect_ip/notch.cpp` | 60 Hz notch biquad 實作 |
| `hls/effect_ip/hpf.cpp` | 28 Hz HPF 實作 |
| `hls/effect_ip/process_sample.cpp` | 頂層串接邏輯與 AXI-Stream 包裝 |
| `docs/INTERFACE.md` | AXI-Lite 完整位址表 |
| `docs/phase2.md` | Phase 2 distortion 開發細節與技術決策 |
