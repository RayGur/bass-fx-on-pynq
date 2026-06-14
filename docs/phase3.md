# Phase 3 — Wobble 實作計畫書

> **狀態：✅ 完成**（2026-06-14，整合於 Phase 6 DMA branch `phase6/wobble`）  
> Exit Criteria 全數通過；板上音訊驗證 PASS；`wobble_dma_test.py` IIR 行為驗證 PASS。  
> Post-MVP 優化待辦：wobble 深度不足（D26）、distortion 底噪（D27）— 見 `docs/decisions.md`。

> **目標**：第二個可聽效果。一階 IIR lowpass + LFO 掃頻，產生「哇嗚」週期性音色。  
> **負責人**：Claire（演算法）、Ray（Phase 6 整合）  
> **前置條件**：
> - Phase 1 Exit Criteria 通過（Effect IP 路徑通）
> - Phase 2 不需先完成（wobble 路徑獨立，可平行）
> - **動 `effect_ip.h` 前先告知 Ray**（state_t 是共用 header）
>
> **Exit Criteria**：
> - ✅ 能聽到週期性音色掃動（「哇嗚」感）
> - ✅ 調 `lfo_rate` 能聽到掃動快慢明顯變化（AXI-Lite 熱改驗證）
> - ✅ 調 `lfo_depth` 能聽到效果深淺變化
> - ✅ 無溢位、不爆音（clamp 生效，wobble_dma_test.py bounds check PASS）
> - ✅ HLS C Simulation testbench 通過（IIR 收斂、bounded output、state 跨呼叫保留）

---

## 1. 演算法說明

### 1.1 一階 IIR Lowpass

```
y[n] = b × x[n] + (1 − b) × y[n−1]
```

- `b` = 濾波器係數，決定截止頻率 fc：`b ≈ 2π × fc / fs`（fc << fs 時成立）
- `b` 越大 → fc 越高 → 聲音越亮；`b` 越小 → fc 越低 → 聲音越悶
- `y[n−1]` = 前一個輸出樣本，需存在 `state_t` 中跨 sample 保留

### 1.2 LFO（低頻振盪器）掃動 fc

LFO 週期性改變 `b`，使截止頻率在 fc_low 和 fc_high 之間來回，產生哇嗚音色。

```
lfo_wave[n] = triangle(lfo_phase[n])         # −1 到 +1 三角波
fc[n]       = fc_center + fc_range × lfo_wave[n]
b[n]        = b_table[ index_from_fc(fc[n]) ]  # 查預算好的係數表
```

**LFO 相位累加（phase accumulator）**：
```
lfo_phase[n] = lfo_phase[n−1] + lfo_step   （mod 2³²，自然溢出）
lfo_step     = lfo_rate × 2³² / (fs × 1000)  （lfo_rate 單位 mHz）
```

- 相位累加器 wrap-around 即為一個完整 LFO 週期
- `lfo_step` 由 PS 端計算後以 `param_t` 傳入（見第 2 節）
- `lfo_phase` 存在 `state_t` 中（24-bit 整數較 uint32 節省資源，但 uint32 最穩定）

### 1.3 三角波（不需 sin LUT，MVP 首選）

```
phase_top  = lfo_phase[31]      // 1 bit：上升 or 下降半周
phase_frac = lfo_phase[30:23]   // 8 bits：在半周內的位置 (0–255)

if phase_top == 0:
    lfo_wave = phase_frac / 255.0          // 0 → +1（上升段）
else:
    lfo_wave = (255 - phase_frac) / 255.0  // +1 → 0（下降段）

// 變成 [-1, +1]：lfo_wave_centered = lfo_wave * 2 - 1
```

> **升級**：若三角波音色不夠平滑，Phase 5 後可換 sin LUT（16 entry 即可）。

---

## 2. 參數約定（AXI-Lite）

| 參數 | 型別 | 單位 / 範圍 | PS 端換算 | 說明 |
|------|------|------------|---------|------|
| `lfo_rate` | `param_t` (int32) | 但傳入的是 `lfo_step`（uint32） | `lfo_step = int(rate_hz * (1<<32) / 48000)` | 例如 1 Hz → lfo_step ≈ 89478 |
| `lfo_depth` | `param_t` (int32) | 整數 0–100（百分比） | 直接傳 0–100 | 100 = 最大掃頻範圍 |

> **設計選擇**：`lfo_rate` 直接傳 `lfo_step`（已乘好），IP 內不做除法，省 DSP。  
> PS 端算一次 `int(rate_hz * (1<<32) / 48000)` 很便宜。

**推薦預設值（demo 用）**：
- 慢 wobble：rate ≈ 0.5 Hz，lfo_step ≈ 44739，depth = 60
- 快 wobble：rate ≈ 2.0 Hz，lfo_step ≈ 178956，depth = 80

---

## 3. 係數 LUT 設計

一階 IIR 係數 `b = 2π × fc / fs`，對應 fc 範圍（bass 用）：

| fc | b（浮點） | b（Q0.15，× 32768） | 說明 |
|----|---------|---------------------|------|
| 200 Hz | 0.02618 | 858 | 低音最悶 |
| 500 Hz | 0.06544 | 2144 | 中低 |
| 1000 Hz | 0.1305 | 4277 | 中 |
| 2000 Hz | 0.2549 | 8353 | 中高 |
| 4000 Hz | 0.4759 | 15599 | 最亮 |

LUT：預先算好 16（或 8）個 `b` 值，存成 `ap_uint<16>` 陣列，  
LFO phase 的高 4 bits（或 3 bits）作為 index。

```c
// 16 entry LUT（fc 等比分布 200 Hz–4000 Hz）
static const ap_uint<16> B_LUT[16] = {
    858, 1068, 1329, 1655, 2061, 2566, 3196, 3981,
    4959, 6175, 7692, 9581, 11933, 14863, 18513, 23061
};
// index = (lfo_phase >> 28) & 0xF  → 取高 4 bits
```

> 這些數值可用 Python 預算：`int(2 * math.pi * fc / 48000 * 32768)` for each fc in geometric series.

---

## 4. `state_t` 欄位（需與 Ray 協調）

**目前 `effect_ip.h` 中 `state_t` 是空 placeholder。**  
Phase 3 開始前，Claire 先告知 Ray 需要哪些欄位，  
Ray 更新 `effect_ip.h` 並通知 Claire 確認後再開始 coding。

建議欄位：

```c
typedef struct {
    ap_uint<32>  lfo_phase;   // LFO 相位累加器（uint32，自然 wrap）
    sample_t     iir_prev;    // IIR 前一輸出 y[n-1]，型別同 sample_t
} state_t;
```

> **重要**：`state_t` 改動後，Ray 需在 `process_sample.cpp` 的 AXI-Lite pragma 確認 `state` port 的 offset 是否需更新；  
> 重新 HLS Synthesis 後 check `xprocess_sample_hw.h` 中 `ADDR_STATE` 是否有變。

---

## 5. HLS 修改：`wobble.cpp`

```c
#include "effect_ip.h"
#include <ap_int.h>

// b 係數 LUT（16 entry，fc 200–4000 Hz，Q0.15 格式）
static const ap_uint<16> B_LUT[16] = {
    858,  1068,  1329,  1655,  2061,  2566,  3196,  3981,
    4959, 6175,  7692,  9581, 11933, 14863, 18513, 23061
};

sample_t apply_wobble(sample_t in, param_t lfo_step, param_t lfo_depth,
                      state_t *state) {
    // --- 1. 更新 LFO 相位 ---
    state->lfo_phase += (ap_uint<32>)lfo_step;   // 自然 wrap

    // --- 2. 三角波 → LFO 值（0 to 255，uint8 範圍）---
    ap_uint<1>  direction = state->lfo_phase[31];
    ap_uint<8>  frac      = (state->lfo_phase >> 23) & 0xFF;
    ap_uint<8>  wave      = direction ? (ap_uint<8>)(255 - frac) : frac;

    // --- 3. depth 調制：lfo_depth 0–100 → index 0–15 ---
    //    depth 100 → 使用完整 0–15 index range
    //    depth 0   → index 固定在中間（不掃頻，passthrough）
    ap_uint<8> depth_scaled = (ap_uint<8>)((wave * lfo_depth) / 100);
    ap_uint<4> lut_idx = depth_scaled >> 4;   // 256 → 16 index

    // --- 4. 查 LUT 取係數 b ---
    ap_uint<16> b_raw = B_LUT[lut_idx];
    // b_raw 是 Q0.15（× 32768），轉回 ap_fixed<16,1> 做乘法
    ap_fixed<16, 1> b = ap_fixed<16, 1>(b_raw) / ap_fixed<16, 1>(32768.0);

    // --- 5. 一階 IIR ---
    // y[n] = b × x[n] + (1-b) × y[n-1]
    ap_fixed<32, 2> y = b * (ap_fixed<32, 2>)in
                      + (ap_fixed<16,1>(1.0) - b) * (ap_fixed<32,2>)state->iir_prev;

    // --- 6. Clamp 回 sample_t 並更新 state ---
    sample_t out;
    if      (y >  sample_t(0.9999))  out =  sample_t(0.9999);
    else if (y < -sample_t(1.0))     out = -sample_t(1.0);
    else                             out  = (sample_t)y;

    state->iir_prev = out;
    return out;
}
```

**HLS 重點**：
- `B_LUT` 會被合成成 LUT ROM，資源極省
- 禁用 `DATAFLOW`、非必要 `UNROLL`（開發規則 5）
- 每級後做 clamp（開發規則 6）
- `state_t *state` 為 pass-by-pointer；HLS 會分配暫存器（非 BRAM）

---

## 6. `process_sample.cpp` 更新（Ray 負責）

Phase 3 完成後，Ray 需在 `process_sample.cpp` 加入 wobble 路徑：

```c
// Phase 3 追加（wobble 路徑）
if (wobble_en)
    *out_l = apply_wobble(*out_l, lfo_rate, lfo_depth, state);
// 注意：wobble 作用在 distortion 之後（in → dist → wobble → out）
// 若 dist_en=false，wobble 直接作用在原始 in 上

*out_r = *out_l;  // mono bass 複製到右聲道
```

> Phase 5 串接時會再整理這段邏輯。

---

## 7. PS 端：`pio_loop.py` 新增 wobble helper

Ray 在 PS 端加入以下（Claire 提供換算公式即可）：

```python
import math

def set_wobble(rate_hz=1.0, depth_pct=70):
    """
    rate_hz  : LFO 速率（Hz），0.1–5.0
    depth_pct: 掃頻深度（%），0–100
    """
    lfo_step = int(rate_hz * (1 << 32) / 48000)
    effect.write(ADDR_LFO_RATE,  lfo_step)
    effect.write(ADDR_LFO_DEPTH, depth_pct)

def enable_wobble(on=True):
    effect.write(ADDR_WOBBLE_EN, 1 if on else 0)
```

---

## 8. HLS Testbench 補充項目

在 `tb_process_sample.cpp` 增加 wobble 測試：

| 測試名稱 | 說明 | 驗證方式 |
|---------|------|---------|
| 無效果（wobble_en=false） | 輸出 == 輸入 | assert 相等 |
| depth=0 | 等同固定 fc，輸出是固定 IIR，不是 passthrough | 觀察輸出收斂 |
| 多 sample 掃頻 | 餵 1000 sample，dump lfo_phase 與 b_raw | 觀察相位遞增、LUT index 變化 |
| IIR 穩定性 | 輸入 0 後 IIR 輸出應收斂回 0（有限 impulse response 觀察） | 超過 500 sample 後 out ≈ 0 |
| 不溢位 | 輸入 ±0.9，任意 depth，確認輸出 ∈ (−1, +1) | assert abs(out) < 1.0 |

---

## 9. Claire 工作 Checklist

### Step A — 協調介面
- [ ] 確認第 4 節 `state_t` 欄位，告知 Ray
- [ ] Ray 更新 `effect_ip.h` 並回報（不要自行改 `.h`）
- [ ] pull 最新 `effect_ip.h`，確認 `state_t` 有 `lfo_phase` 與 `iir_prev`

### Step B — 實作 `wobble.cpp`
- [ ] 實作 `B_LUT`（16 entry，可用 Python 預算，見第 3 節）
- [ ] 實作 LFO phase accumulator + 三角波
- [ ] 實作一階 IIR（b × x + (1-b) × y_prev）
- [ ] 每級後 clamp
- [ ] 更新 state（`lfo_phase`, `iir_prev`）

### Step C — HLS C Simulation
- [ ] 在 Vitis HLS 執行 C Simulation（第 8 節測試項全過）
- [ ] 無 warning / error
- [ ] 觀察掃頻 LUT index 輸出，確認三角波形態正確

### Step D — HLS Synthesis
- [ ] `csynth_design` 成功，無 Error
- [ ] 確認 II = 1（若不是，回報 Ray 一起看）
- [ ] 確認 LUT ROM 合成（resource report 中 `ROM_*` 或 LUT 使用量合理）
- [ ] commit `wobble.cpp`（只 commit 這一個檔案）

### Step E — 通知 Ray 整合
- [ ] 告知 Ray「wobble.cpp HLS Sim + Synthesis 通過」
- [ ] Ray 更新 `process_sample.cpp`（加入 wobble 路徑）→ Rebuild Bitstream
- [ ] 上板實測：接 JB62 + amp，執行 `run_realtime(audio)` + `enable_wobble(True)`
- [ ] **Phase 3 Exit Criteria 確認**（聽到掃動 / rate 有變化 / 無 click / 無爆音）

---

## 10. 常見陷阱

| 陷阱 | 說明 | 對策 |
|------|------|------|
| IIR 不穩定 | b 值算錯（太大超過 1.0） | 確認 B_LUT 最大值 < 32768；b_raw/32768 < 1.0 |
| 係數即時重算 | 在 HLS 內做浮點 fc→b 運算 | 用 LUT，不即時算 |
| click 聲 | lfo_step 突變（PS 突然改 rate） | Step 本身平滑（相位連續）；lfo_step 改變時不重置 lfo_phase |
| state 沒更新 | `iir_prev` 忘記在每 sample 結束時賦值 | 確認 `state->iir_prev = out` 在每次呼叫結尾 |
| lfo_depth=0 異常 | `depth_scaled=0 → lut_idx=0 → b=最小`（非 passthrough） | depth=0 時在 PS 端直接 disable wobble_en；IP 內不需要特判 |

---

## 11. ⚠️ Loop 結構變更（Phase 6 修正，Claire 實作前必讀）

**背景**：Phase 6 除錯時發現 HLS PIPELINE 限制 —— `hls::stream` 是單端口 FIFO，每個 clock 只能 1 read 或 1 write。原先每個 loop iteration 做 2 reads + 2 writes（L/R 成對），HLS 強制 II=2，造成 S2MM 跳寫，R samples 全數丟失。

**現行 `process_sample.cpp` loop 結構（已改）**：
```c
// n_samples = stereo pairs（PS 仍寫 256，語義不變）
for (int i = 0; i < n_samples * 2; i++) {
#pragma HLS PIPELINE II=1
    audio_pkt_t pkt = s_in.read();   // 一次 1 read
    // process one sample...
    s_out.write(out_pkt);             // 一次 1 write
}
// L 在 i%2==0，R 在 i%2==1
```

**對 wobble 實作的影響**：

1. **`state_t.iir_prev` 需分成 L/R**  
   原設計的 `iir_prev` 是 L/R 共用，現在 L 和 R 各自獨立進 `apply_wobble`，需要分開的 IIR 狀態。  
   建議改為：
   ```c
   typedef struct {
       ap_uint<32>  lfo_phase;
       sample_t     iir_prev_l;   // L channel IIR 前一輸出
       sample_t     iir_prev_r;   // R channel IIR 前一輸出
   } state_t;
   ```
   > 更動 `state_t` 前先通知 Ray 更新 `effect_ip.h`。

2. **LFO phase 只在 L sample（i%2==0）推進**  
   每個 stereo pair 應有一個 LFO step，不能 L 和 R 各推進一次（頻率加倍）。  
   `apply_wobble` 加入 `bool is_l` 參數，`is_l=false` 時跳過 `lfo_phase +=`，只做 IIR：
   ```c
   sample_t apply_wobble(sample_t in, param_t lfo_step, param_t lfo_depth,
                         state_t *state, bool is_l) {
       if (is_l) state->lfo_phase += (ap_uint<32>)lfo_step;  // 只 L 推進
       // b 查表用同一個 lfo_phase（L 設好，R 直接用）
       ...
       sample_t *iir_prev = is_l ? &state->iir_prev_l : &state->iir_prev_r;
       // IIR 計算用對應 channel 的 state
   }
   ```

3. **呼叫點（`process_sample.cpp`）**：
   ```c
   bool is_l = (i % 2 == 0);
   if (wobble_en) out_s = apply_wobble(out_s, lfo_rate, lfo_depth, &state, is_l);
   ```

**結論**：`apply_wobble` 的函式簽章需加 `bool is_l`；`state_t` 的 `iir_prev` 分成 `iir_prev_l` + `iir_prev_r`。兩者都要在 Phase 3 開始前與 Ray 確認。
