# INTERFACE.md — 介面合約

本檔定義 Ray / Claire 平行開發的介面合約。**訂定後視為凍結**;要修改必須先改本檔、通知對方、再動 code,不可私自更動。改動須有依據(例如實測發現不可行),不是不能改,而是不能默默改。

> 狀態:
> - ✅ 已定案:GPIO / LED 接腳與功能、定點數 Q 格式、AXI-Lite byte offset、distortion 參數編碼（Phase 2）
> - 🔲 待定案：`process_sample()` 完整簽章（Phase 3 wobble 定案後補全）、wobble 濾波器係數查表

---

## 1. 定點數 Q 格式 ✅

業界 / 學術慣例:此類 FPGA 音訊效果普遍採 24-bit 定點 datapath(對應 codec 的 24-bit 取樣)。本專案採此標準。

| 用途 | 型別 | Q 格式 | 說明 |
|------|------|--------|------|
| 音訊 sample | `ap_fixed<24,1>` | Q1.23 | 1 符號位 + 23 小數位,範圍 [−1, +1);對應 codec 24-bit |
| 中間運算（distortion）| `ap_fixed<32,6>` | Q6.26 | 6 整數位（範圍 [−32, +32)），可承載 gain 20x；運算完 clamp 回 Q1.23 |
| 中間運算（wobble）| `ap_fixed<W,I>`（W≥32）| 待 Phase 3 定案 | IIR / LFO 運算範圍待確認後回填 |
| 參數(AXI-Lite 傳入) | `int`(32-bit) | — | PS 傳整數,IP 內部解讀規則見下方各效果說明 |

原則:
- 運算過程用較寬中間型別,**每級結束 clamp 回 [−1, +1)**(見開發規則 6)。
- 低頻 IIR 對係數量化敏感;wobble 濾波器係數**建議預先算好存查表(LUT)**,避免掃頻時即時重算造成量化誤差(文獻常見做法)。

### Distortion 參數編碼（Phase 2 定案）✅

| 參數 | AXI-Lite 型別 | HLS 解讀 | PS 端寫法範例 |
|------|--------------|---------|------------|
| `threshold` | `int`（32-bit）| `ap_fixed<24,1>(ap_int<24>(threshold))`，即 Q1.23 | `int(0.3 * (1<<23))` → clip 點 0.3 |
| `gain` | `int`（32-bit）| 直接做整數乘法，有效範圍 1–20 | `8` → 放大 8 倍 |

Distortion 演算法（hard clipping）：
```
ap_fixed<32,6> amp = in * gain;         // 放大，中間型別防溢位
// threshold decode：必須用 .range() raw bit 指定，不能用值轉換
ap_fixed<24,1> thr_q; thr_q.range(23,0) = ap_int<24>(threshold);
ap_fixed<32,6> thr = thr_q;
out = clamp(amp, -thr, +thr);           // hard clip
// step 3 clip 後 |out| <= thr <= 1.0，cast 回 sample_t 不會 wrap
```

---

## 2. `process_sample()` 簽章 🔲(候選,待 Phase 2/3 定案)

效果運算核心。與外殼(PIO / DMA)解耦,A、B 階段共用同一份。外殼負責搬資料,本函式只負責運算。

### HLS 檔案結構(見 D11)

| 檔案 | 責任人 | 說明 |
|------|--------|------|
| `hls/effect_ip/process_sample.cpp` | Ray | 薄整合層,呼叫 distortion / wobble;HLS 合成入口 |
| `hls/effect_ip/distortion.cpp` | Ray | distortion hard clipping 演算法 |
| `hls/effect_ip/wobble.cpp` | Claire | wobble IIR + LFO 演算法 |

三個檔案合成成**單一 IP**,各自負責各自的 .cpp,Git 衝突最小化。

```c
// 候選簽章 — 處理單一 stereo sample
// 參數由 AXI-Lite 傳入;開關來自 AXI GPIO;運算核心不接觸 AXI/stream 細節
//
// void process_sample(
//     sample_t in_l, sample_t in_r,        // 輸入(mono 來源已於 IP 入口複製為 L/R)
//     sample_t *out_l, sample_t *out_r,    // 輸出
//     bool  dist_en, bool wobble_en,       // 效果開關(來自 sw[0]/sw[1])
//     param_t threshold, param_t gain,     // distortion 參數
//     param_t lfo_rate, param_t lfo_depth, // wobble 參數
//     state_t *state                       // 跨 sample 狀態(LFO 相位、IIR 歷史)
// );
//
// sample_t = ap_fixed<24,1>;  param_t = int
```

待 Phase 2/3 定案:
- 跨 sample 狀態(`state_t`)的具體欄位:LFO 相位累加器、IIR 前一輸出值等。
- 串接時級間順序與 clamp 點(規劃:in → distortion → wobble → out,每級後 clamp)。
- 串接由 `dist_en` / `wobble_en` 控制;兩者皆 true 即串接。

---

## 3. AXI-Lite 位址表

PS 寫入 → PL 即時讀取,音訊不中斷。

來源：`xprocess_sample_hw.h`（HLS 產出）+ `.hwh`（Vivado 整合後）

### Effect IP（process_sample_0）— Phase 1–5 PIO 版本 ✅

> **IP base address：`0x4002_0000`**

| 參數 | 方向 | byte offset | 說明 |
|------|------|-------------|------|
| ap_ctrl | R/W | `0x00` | bit0=ap_start, bit1=ap_done, bit2=ap_idle |
| `in_l` | PS→PL | `0x10` | 左聲道輸入，24-bit Q1.23，低 24 bits 有效 **[Phase 6 移除]** |
| `in_r` | PS→PL | `0x18` | 右聲道輸入，24-bit Q1.23，低 24 bits 有效 **[Phase 6 移除]** |
| `out_l` | PL→PS | `0x20` | 左聲道輸出，24-bit Q1.23（read only）**[Phase 6 移除]** |
| `out_l_ap_vld` | PL→PS | `0x24` | out_l 有效旗標（read, clear on read）**[Phase 6 移除]** |
| `out_r` | PL→PS | `0x30` | 右聲道輸出，24-bit Q1.23（read only）**[Phase 6 移除]** |
| `out_r_ap_vld` | PL→PS | `0x34` | out_r 有效旗標（read, clear on read）**[Phase 6 移除]** |
| `dist_en` | PS→PL | `0x40` | distortion 開關，bit0 |
| `wobble_en` | PS→PL | `0x48` | wobble 開關，bit0 |
| `threshold` | PS→PL | `0x50` | distortion clip 點，Q1.23 int；PS 寫 `int(clip_float * (1<<23))`，例如 0.3 → `0x2666666` |
| `gain` | PS→PL | `0x58` | distortion 輸入增益，純整數 1–20；PS 直接寫整數，例如 `8` |
| `lfo_rate` | PS→PL | `0x60` | wobble 掃動速率（32-bit int） |
| `lfo_depth` | PS→PL | `0x68` | wobble 掃動範圍（32-bit int） |
| `state` | PS→PL | `0x70` | 跨 sample 狀態（Phase 1 placeholder）**[Phase 6 移除，改 HLS static]** |

### Effect IP — Phase 6 DMA 版本 ✅（RTL Synthesis 確認，2026-06-12）

> **IP base address：`0x4002_0000`**（Address Editor 確認，2026-06-13）  
> **Parameter offsets 已由 RTL Synthesis 產出的 `csynth.rpt` 確認。**

| 參數 | 方向 | byte offset | 說明 |
|------|------|-------------|------|
| ap_ctrl（CTRL） | R/W | `0x00` | bit0=AP_START, bit1=AP_DONE, bit2=AP_IDLE, bit7=AUTO_RESTART |
| GIER | R/W | `0x04` | Global Interrupt Enable, bit0=Enable |
| IP_IER | R/W | `0x08` | Interrupt Enable, bit0=CHAN0, bit1=CHAN1 |
| IP_ISR | R/W | `0x0c` | Interrupt Status, bit0=CHAN0, bit1=CHAN1 |
| `n_samples` | PS→PL | `0x10` | 每次 DMA 傳輸的 stereo pair 數（通常 256） |
| `dist_en` | PS→PL | `0x18` | distortion 開關，bit0 |
| `wobble_en` | PS→PL | `0x20` | wobble 開關，bit0 |
| `threshold` | PS→PL | `0x28` | distortion clip 點，Q1.23 int；PS 寫 `int(clip_float * (1<<23))` |
| `gain` | PS→PL | `0x30` | distortion 輸入增益，純整數 1–20 |
| `lfo_rate` | PS→PL | `0x38` | wobble 掃動速率（32-bit int） |
| `lfo_depth` | PS→PL | `0x40` | wobble 掃動範圍（32-bit int） |

### Effect IP — Phase 6 AXI-Stream 介面 ✅

HLS top function 新增兩個 AXI-Stream port，取代原有 in_l/in_r/out_l/out_r：

| Port | 方向 | 型別 | 說明 |
|------|------|------|------|
| `s_in` | DMA→IP | `hls::stream<ap_axis<32,0,0,0>>` | 音訊輸入 stream，L/R 交錯（L[0], R[0], L[1], R[1], …） |
| `s_out` | IP→DMA | `hls::stream<ap_axis<32,0,0,0>>` | 音訊輸出 stream，L/R 交錯 |

**資料格式**：每個 32-bit word 的低 24 bits 為 Q1.23 sample，高 8 bits 為 0。L/R 交錯，共 `n_samples × 2` words per transfer。  
**TLAST**：stream 最後一個 word（index = n_samples×2−1）必須設 `.last=1`（見 D21）。  
**TKEEP（重要）**：每個輸出 packet 必須顯式設 `.keep = ~0`（32-bit stream → `0xF`，4 bits 全 1）。若不設，HLS 合成後 `.keep` 預設為 0 → TKEEP=0 → AXI DMA DataMover 將 WSTRB 設為 0 → HP0 接受交易但不寫 DDR → `out_buf` 永遠不更新（見 D23）。

### AXI DMA — Phase 6 ✅（BD 確認，2026-06-12）

| 項目 | 值 |
|------|----|
| IP 實例 | `axi_dma_0` |
| base address | **`0x41E0_0000`** |
| MM2S（PS→PL）| DMA_CR offset `0x00`，DMA_SR `0x04`，SRC_ADDR `0x18`，LENGTH `0x28` |
| S2MM（PL→PS）| DMA_CR offset `0x30`，DMA_SR `0x34`，DST_ADDR `0x48`，LENGTH `0x58` |
| 中斷 | mm2s_introut + s2mm_introut → xlconcat_0 → `IRQ_F2P[0]` ✅ |
| Buffer size | 256 samples × 2 ch × 4 bytes = 2048 bytes/transfer |
| 模式 | Direct Register Mode（無 SG）|

### audio_codec_ctrl — Phase 6

| 項目 | 值 |
|------|----|
| IP 實例 | `audio_codec_ctrl_0` |
| base address | **`0x44A0_0000`** |

### AXI GPIO

| IP 實例 | base address | 功能 |
|---------|-------------|------|
| `axi_gpio_0` | `0x4000_0000` | GPIO=sw[1:0]（input），GPIO2=btn[3:0]（input） |
| `axi_gpio_1` | `0x4001_0000` | GPIO=led[3:0]（output） |

> AXI GPIO 暫存器 offset：DATA=0x000（ch1），DATA2=0x008（ch2），TRI=0x004，TRI2=0x00C

### AXI IIC（ADAU1761 I2C 配置）

| IP 實例 | base address | 功能 |
|---------|-------------|------|
| `axi_iic_0` | **`0x4080_0000`** | ADAU1761 I2C 配置（codec PLL + 暫存器初始化） |

> 硬體接腳：SCL = U9，SDA = T9（PL IO，LVCMOS33，需 PULLUP）  
> **注意**：PYNQ 2.5 HWH parser 不自動建 DT entry，`/dev/i2c-X` 不會出現。  
> 實際存取方式：`pynq.lib.iic.AxiIIC`（直接操作 MMIO），搭配 `Overlay(..., ignore_version=True)`（詳見 D14）。

---

## 4. AXI GPIO / LED 接腳與功能 ✅

接腳值來自 PYNQ-Z2 官方 Reference Manual 與 Xilinx 官方 `base.xdc`,多來源交叉驗證一致。所有 IOSTANDARD 為 `LVCMOS33`。

### 輸入:switch + button

| 訊號 | PACKAGE_PIN | 功能 |
|------|-------------|------|
| sw[0] | M20 | distortion on / off |
| sw[1] | M19 | wobble on / off(兩者同開 = 串接) |
| btn[0] | D19 | 短按 toggle:distortion low ↔ high |
| btn[1] | D20 | 短按 toggle:wobble low ↔ high |
| btn[2] | L20 | 保留未用 |
| btn[3] | L19 | 保留未用 |

### 輸出:LED(狀態顯示)

| 訊號 | PACKAGE_PIN | 功能 |
|------|-------------|------|
| led[0] | R14 | distortion 狀態:亮 = high,暗 = low |
| led[1] | P14 | wobble 狀態:亮 = high,暗 = low |
| led[2] | N16 | 保留未用 |
| led[3] | M14 | 保留未用 |

### 按鈕 toggle 行為(PS 端軟體實作)

- **短按即翻轉**:偵測「從未按 → 按下」的上升邊緣,翻轉對應 state,寫對應參數(low/high 值)並更新 LED。不加「按住 N 秒」條件(demo 為刻意操作,無誤觸顧慮)。
- **需處理**:邊緣偵測(記住上次值,只在邊緣翻轉)+ debounce(輪詢間隔或連續 N 次一致才認定,避免機械抖動造成一按多跳)。
- 全部在 PS 端輪詢迴圈處理,不佔 PL 邏輯。

### 備註

- base overlay 中 LED port 名為 `leds_4bits_tri_o[0..3]`;自訂 AXI GPIO output 時 port 名須與 .xdc 的 `get_ports` 一致。
- 普通 4 顆 LED 即足夠;RGB LED(`rgbleds_6bits_tri_o`)不使用。
- low/high 的具體參數數值(threshold/gain/lfo_rate/lfo_depth 的兩組值)待 Phase 4 調出好聽範圍後填入。

---

## 變更紀錄

| 版本 | 變更 |
|------|------|
| 初版 | GPIO/LED 接腳與功能、Q 格式定案；AXI-Lite offset 與 process_sample 簽章待後續 Phase |
| Phase 1 | AXI-Lite offset 定案（from HLS xprocess_sample_hw.h）；AXI GPIO base address 定案；AXI IIC base address 定案（`0x40800000`）；PYNQ 2.5 DT 限制與 AxiIIC 存取方式補充 |
| Phase 2 | distortion 參數編碼定案：threshold 為 Q1.23 int，gain 為純整數 1–20；中間運算型別定案：`ap_fixed<32,6>`（Q6.26） |
| Phase 6 | 資料 port（in_l/in_r/out_l/out_r/state）標記為 Phase 6 移除；新增 AXI-Stream 介面規格；新增 AXI DMA 欄位（base address TBD）；parameter offsets TBD 待 re-synthesis |
| Phase 6 RTL | Effect IP Phase 6 AXI-Lite parameter offsets 確認（n_samples=0x10, dist_en=0x18, wobble_en=0x20, threshold=0x28, gain=0x30, lfo_rate=0x38, lfo_depth=0x40）；II=1（per-sample loop，n_samples×2 iters，見 D24）；IP base address 待 Vivado BD |
| Phase 6 BUG D23 | AXI-Stream 輸出 packet 必須顯式設 `.keep = ~0`；TKEEP=0 → WSTRB=0 → HP0 不寫 DDR（見 D23）；已修入 `process_sample.cpp` |
| Phase 6 BUG D24 | `hls::stream` 單端口 FIFO：原 2×read+2×write per iter 強制 II=2 → R channel 所有 sample 不寫 DDR；改 `n_samples×2` iters 每次 1 read+1 write → II=1（見 D24）；已修入 `process_sample.cpp`，重新合成 Final II=1 確認 ✅ |
| Phase 6 BUG D25 | Zynq HP0 內部 64-bit 匯流排；`PCW_S_AXI_HP0_DATA_WIDTH=32` 只改 HWH 不改硬體 → WSTRB=0x0F per 64-bit beat → 每隔一個 32-bit word 不寫 DDR；修正：Vivado PS7 HP0 Data Width 改為 64，重建 bitstream（見 D25）；板上驗證 total written=512 ✅ |
