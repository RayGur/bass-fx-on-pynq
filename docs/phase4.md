# Phase 4 + 5 — GPIO 控制 + 效果串接（MVP 完成）

> **狀態：🔲 進行中**（branch: `feat/gpio`）
>
> **目標**：
> - Phase 4：sw/btn/LED 完整控制迴路；按鈕切換 low/high 參數組；MVP 完成。
> - Phase 5：sw[0]+sw[1] 同開即串接（dist → wobble），HLS 已支援，PS 端直接達成。
>
> **Exit Criteria（Phase 4 + 5 合並驗證）**：
> - [ ] sw[0] 切開/關，distortion 即時生效；sw[1] 同理
> - [ ] btn[0] 短按翻轉 distortion 強度，led[0] 對應更新
> - [ ] btn[1] 短按翻轉 wobble 速率，led[1] 對應更新
> - [ ] RGB LD4 亮滅與 sw[0] 同步；LD5 與 sw[1] 同步
> - [ ] sw[0]+sw[1] 同開，串接效果可聽、不爆音（Phase 5 Exit Criteria）
> - [ ] 效果切換期間音訊無爆音（clamp 生效）

---

## 1. 架構說明

本 Phase 純 PS 端工作，**HLS IP 與 BD 不需重新合成 effect_ip**（dist_en / wobble_en 已存在且已驗證）。

唯一的硬體改動：BD 加入 `axi_gpio_2`（6-bit output，驅 RGB LED），須重新合成 bitstream。

```
                           ┌─── sw[1:0] ──────── axi_gpio_0 ch1
PYNQ-Z2 板子輸入 ─────────┤
                           └─── btn[3:0] ─────── axi_gpio_0 ch2

PS control_poll() ─── 讀 sw/btn，寫 Effect IP AXI-Lite
                   └── 寫 led[1:0]    → axi_gpio_1
                   └── 寫 rgbleds[5:0] → axi_gpio_2
```

### 1.1 Phase 5 串接

串接邏輯已在 Phase 6 HLS 合成時完成（`process_sample_core()`：`in → dist → wobble → out`）。  
PS 端只需讓 `dist_en=1` 且 `wobble_en=1` 同時成立，即達成 Phase 5。  
由 sw[0]/sw[1] 同開自動觸發，無需任何 HLS 改動。

---

## 2. BD 改動（Ray 手動）

### 2.1 加入 axi_gpio_2

| 步驟 | 說明 |
|------|------|
| Add IP | `AXI GPIO`，GPIO Width=6，All Outputs，不需 Dual Channel |
| 連接 | S_AXI → AXI Interconnect（PS GP0 側） |
| Make External | port 命名 `rgbleds_tri_o`（6-bit），與 XDC 一致 |
| Address Editor | 已確認：`0x4003_0000` |

### 2.2 hw_cons.xdc 加入 RGB LED 接腳

從 PYNQ-Z2 官方 `base.xdc` 確認，無 pin 衝突：

```tcl
# RGB LEDs (LD4=bits[2:0], LD5=bits[5:3]) — Phase 4
set_property PACKAGE_PIN L15 [get_ports {rgbleds_tri_o[0]}]
set_property PACKAGE_PIN G17 [get_ports {rgbleds_tri_o[1]}]
set_property PACKAGE_PIN N15 [get_ports {rgbleds_tri_o[2]}]
set_property PACKAGE_PIN G14 [get_ports {rgbleds_tri_o[3]}]
set_property PACKAGE_PIN L14 [get_ports {rgbleds_tri_o[4]}]
set_property PACKAGE_PIN M15 [get_ports {rgbleds_tri_o[5]}]
set_property IOSTANDARD LVCMOS33 [get_ports {rgbleds_tri_o[0]}]
set_property IOSTANDARD LVCMOS33 [get_ports {rgbleds_tri_o[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {rgbleds_tri_o[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {rgbleds_tri_o[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {rgbleds_tri_o[4]}]
set_property IOSTANDARD LVCMOS33 [get_ports {rgbleds_tri_o[5]}]
```

> **注意**：若 Vivado make external 產生的 port 名稱不是 `rgbleds_tri_o`，  
> 需同步修改 XDC 的 `get_ports` 名稱以匹配 BD wrapper。

---

## 3. PS 端改動：audio_dma.c

### 3.1 新增 GPIO2 mmap

```c
#define GPIO2_BASE 0x40030000UL

static volatile uint32_t *gpio0;   // sw + btn input
static volatile uint32_t *gpio1;   // led[3:0] output
static volatile uint32_t *gpio2;   // rgbleds[5:0] output
```

### 3.2 預設參數組

| Preset | threshold | gain | lfo_rate | lfo_depth |
|--------|-----------|------|----------|-----------|
| distortion **low** | `0.5 × (1<<23)` = 4194304 | 4 | — | — |
| distortion **high** | `0.2 × (1<<23)` = 1677722 | 12 | — | — |
| wobble **slow** | — | — | 89478（1 Hz） | 80 |
| wobble **fast** | — | — | 357914（4 Hz） | 100 |

> 上板調出好聽範圍後更新此表與 INTERFACE.md 第 4.3 節。

### 3.3 LED 對應

| LED | 功能 | 邏輯 |
|-----|------|------|
| led[0]（R14）| distortion preset | high=亮，low=滅 |
| led[1]（P14）| wobble preset | high=亮，low=滅 |
| LD4（rgbleds[2:0]，pins L15/G17/N15）| sw[0] distortion on/off | 開=亮（全彩），關=滅 |
| LD5（rgbleds[5:3]，pins G14/L14/M15）| sw[1] wobble on/off | 開=亮（全彩），關=滅 |

### 3.4 control_poll() 邏輯（每 audio buffer ≈ 5.33 ms 呼叫一次）

```
讀 sw[1:0]
  → 變動時寫 EFFECT_DIST_EN / EFFECT_WOBBLE_EN

讀 btn[1:0]
  → 按下後連續 3 次確認（≈ 16 ms debounce）
  → 翻轉 dist_preset / wobble_preset
  → 寫對應參數組至 Effect IP AXI-Lite
  → 放開後重置計數器，允許下次按壓

更新 led[1:0] (gpio1) + rgbleds[5:0] (gpio2)
```

---

## 4. 驗證步驟

> **前置**：執行前需先跑 `codec_init.py`（全域要求，非本 Phase 特有，見 README §5）。

1. **編譯** `gcc audio_dma.c -lcma -lpthread -O2 -o audio_dma`
2. **啟動** `sudo ./audio_dma`，確認 init log 顯示 dist_en/wobble_en 正確
3. **sw 驗證**：撥 sw[0]，確認 LD4 亮滅 + 音訊有無 distortion
4. **btn 驗證**：按 btn[0]，確認 led[0] 翻轉 + distortion 強度可聽到變化
5. **串接驗證（Phase 5）**：sw[0]+sw[1] 同開，確認效果可聽、不爆音
6. **電平管理**：調高 gain 後確認無 wrap-around（注意 D27 底噪，為已知 post-MVP 問題）

---

## 5. 已知限制（Post-MVP）

- D26：wobble 深度不足（B_LUT 掃動範圍需往低頻推）
- D27：distortion 高 gain 底噪放大（可加 noise gate）

兩者不阻擋 Phase 4+5 Exit Criteria，留 MVP 後處理。
