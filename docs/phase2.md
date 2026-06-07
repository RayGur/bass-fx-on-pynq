# Phase 2 — Distortion 實作計畫書

> **目標**：第一個可聽效果。Hard clipping distortion，以 AXI-Lite 控制 threshold / gain。  
> **負責人**：Ray  
> **前置條件**：Phase 1 Exit Criteria 通過（聲音已能穿過 Effect IP）。  
> **Exit Criteria**：
> - 聽得到失真；調 threshold / gain 聽得到變化
> - 不爆音（clamp 生效）
> - 離線 testbench 對 sine wave 音檔驗證通過

---

## 1. 演算法說明

**Hard clipping distortion**：

1. 先放大輸入 × `gain`
2. 超過 `threshold` 就硬切到 ±threshold

```
amplified = input × gain
output    = clamp(amplified, −threshold, +threshold)
```

聽感：`gain` 大 → 更多波形被截 → 音色更粗糙；`threshold` 小 → 截點低 → 失真更重。

---

## 2. 參數約定（AXI-Lite）

| 參數 | 型別 | 單位 / 範圍 | 說明 |
|------|------|------------|------|
| `threshold` | `param_t` (int32) | Q1.23 raw integer；推薦範圍 838860–8388607（≈ 0.1 – 1.0） | 截點；0.5 → 傳 4194304（= 0.5 × 2²³） |
| `gain` | `param_t` (int32) | 整數 1–8 | 輸入增益倍率；1 = 無放大 |

> **PS 端換算**：`threshold_int = int(threshold_float * (1 << 23))`

---

## 3. HLS 修改：`distortion.cpp`

```c
#include "effect_ip.h"

sample_t apply_distortion(sample_t in, param_t threshold, param_t gain) {
    // 1. 放大：使用寬中間型別防溢位（gain 最大 8，sample 最大 ~1 → 最大 ~8）
    ap_fixed<32, 9> amplified = (ap_fixed<32, 9>)in * (ap_fixed<9, 9>)gain;

    // 2. threshold 從 Q1.23 raw int 轉回 [-1,+1) 定點
    //    除以 2^23 是常數，HLS 會優化為 shift
    ap_fixed<32, 9> thresh = (ap_fixed<32, 9>)threshold
                           * ap_fixed<32, 9>(1.0 / (1 << 23));

    // 3. Hard clip
    if (amplified >  thresh)  amplified =  thresh;
    if (amplified < -thresh)  amplified = -thresh;

    // 4. 縮回 sample_t（ap_fixed 會自動 saturate）
    return (sample_t)amplified;
}
```

**注意事項**：
- `ap_fixed<32, 9>` = 1 符號位 + 8 整數位 + 23 小數位，可容納 gain=8、完整不溢位
- `(ap_fixed<9, 9>)gain` 確保整數乘法路徑；HLS 會合成成 DSP multiply-add
- 禁用 `#pragma HLS UNROLL`、`DATAFLOW`（開發規則 5）
- 每級結束後 clamp 已在步驟 3 完成（開發規則 6）

---

## 4. HLS 修改：`process_sample.cpp`

Phase 1 的 passthrough 版要改成：

```c
// Phase 2：dist_en=true 時走 distortion
if (dist_en)
    *out_l = apply_distortion(in_l, threshold, gain);
else
    *out_l = in_l;

// Clamp after distortion
if (*out_l >  sample_t(0.9999))  *out_l =  sample_t(0.9999);
if (*out_l < -sample_t(1.0))     *out_l = -sample_t(1.0);

// 右聲道同樣處理（mono bass 複製到雙聲道）
*out_r = *out_l;
```

> Phase 3 再加 wobble 路徑；Phase 5 串接。現在只改 distortion 部分。

---

## 5. HLS Testbench：`tb_process_sample.cpp`

在 Phase 1 的 testbench 基礎上增加以下測試項：

| 測試名稱 | 輸入 | threshold | gain | 預期結果 |
|---------|------|-----------|------|---------|
| passthrough（dist_en=false） | 0.3 | 任意 | 任意 | out == 0.3 |
| 不溢位（小信號） | 0.2 | 0.5（Q1.23） | 2 | out == 0.4（未截） |
| 正向 clipping | 0.8 | 0.5（Q1.23） | 2 | out == 0.5（截頂） |
| 負向 clipping | −0.8 | 0.5（Q1.23） | 2 | out == −0.5 |
| gain=1（threshold=1.0） | 0.9 | 1.0（Q1.23） | 1 | out ≈ 0.9（無截） |
| 最大 gain 不 overflow | 0.5 | 0.3（Q1.23） | 8 | out == 0.3（截，不爆） |

Threshold Q1.23 換算：`0.5 → 4194304`，`0.3 → 2516582`

---

## 6. PS 端修改：`pio_loop.py`

### 6.1 新增參數設定 helper

```python
def set_distortion(threshold_float=0.5, gain_int=2):
    """
    threshold_float: 0.0–1.0，截點（佔滿刻度的比例）
    gain_int       : 1–8
    """
    thresh_raw = int(threshold_float * (1 << 23))
    effect.write(ADDR_THRESHOLD, thresh_raw)
    effect.write(ADDR_GAIN, gain_int)

def enable_distortion(on=True):
    effect.write(ADDR_DIST_EN, 1 if on else 0)
```

### 6.2 改寫音訊迴圈為 real-time streaming

`process_audio_block` 是 batch（先錄再播），有大緩衝延遲，不適合 demo。  
Phase 2 改為逐 sample 即時處理：

```python
def run_realtime(audio_ip, seconds=10.0):
    """
    即時 PIO 迴圈：每 I2S frame 讀 → Effect IP → 寫。
    音質仍受 Python ~30μs/frame 限制（Phase 6 DMA 修）。
    """
    arr    = audio_ip.mmio.array
    n      = int(seconds * 48000)
    sw_last = 0

    for _ in range(n):
        _wait_frame(arr)
        in_l_raw = int(np.int32(arr[_RX_L]))
        in_l = in_l_raw / (1 << 23)

        # 讀 switch → 更新效果開關
        sw = read_sw()
        if sw != sw_last:
            effect.write(ADDR_DIST_EN,   sw & 0x1)
            effect.write(ADDR_WOBBLE_EN, (sw >> 1) & 0x1)
            sw_last = sw

        out_l, out_r = run_effect(in_l, in_l)  # mono bass → L=R
        arr[_TX_L] = np.uint32(int(out_l * (1 << 23)) & 0xFFFFFF)
        arr[_TX_R] = np.uint32(int(out_r * (1 << 23)) & 0xFFFFFF)
```

---

## 7. Ray 手動執行 Checklist

### Step A — 修改 HLS
- [ ] 修改 `hls/effect_ip/distortion.cpp`（按第 3 節）
- [ ] 修改 `hls/effect_ip/process_sample.cpp`（按第 4 節）
- [ ] 在 Vitis HLS 執行 C Simulation，所有 testbench case 通過
- [ ] 執行 HLS Synthesis，確認 II=1，無 Error，資源合理

### Step B — Rebuild Bitstream
- [ ] Vivado：更新 IP（Refresh IP）
- [ ] Generate Bitstream 成功
- [ ] `bass_fx.bit` + `bass_fx.hwh` 傳板

### Step C — 上板驗證
- [ ] Overlay 載入正常
- [ ] 執行 `set_distortion(0.5, 4); enable_distortion(True)`
- [ ] 執行 `run_realtime(audio, 10.0)`，接 JB62 + amp
- [ ] 能聽到失真（vs passthrough 有明顯差異）
- [ ] 調 gain（1 vs 8）能聽到失真程度變化
- [ ] 調 threshold（0.1 vs 0.8）能聽到截點變化
- [ ] 切 sw[0] OFF → 聲音恢復 passthrough
- [ ] 無爆音、無溢位

---

## 8. 已知限制與後續處理

| 項目 | 說明 |
|------|------|
| 音質 dropout | 同 Phase 1：Python PIO 掉 sample，Phase 6 DMA 修 |
| stereo 處理 | 目前 `out_r = out_l`（mono bass 複製），Phase 2 維持此做法 |
| state_t | Phase 2 不需要跨 sample 狀態，`state` 參數傳入後不使用 |
| wobble 路徑 | `wobble_en` 目前仍是 passthrough（Phase 1 骨架）；Phase 3 由 Claire 填入 |
