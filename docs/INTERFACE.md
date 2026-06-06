# INTERFACE.md — 介面合約

本檔定義 Ray / Claire 平行開發的介面合約。**訂定後視為凍結**;要修改必須先改本檔、通知對方、再動 code,不可私自更動。改動須有依據(例如實測發現不可行),不是不能改,而是不能默默改。

> 狀態:
> - ✅ 已定案:GPIO / LED 接腳與功能、定點數 Q 格式
> - 🔲 待定案:AXI-Lite byte offset(Phase 1 後填)、`process_sample()` 完整簽章(Phase 2/3 定案)

---

## 1. 定點數 Q 格式 ✅

業界 / 學術慣例:此類 FPGA 音訊效果普遍採 24-bit 定點 datapath(對應 codec 的 24-bit 取樣)。本專案採此標準。

| 用途 | 型別 | Q 格式 | 說明 |
|------|------|--------|------|
| 音訊 sample | `ap_fixed<24,1>` | Q1.23 | 1 符號位 + 23 小數位,範圍 [−1, +1);對應 codec 24-bit |
| 中間運算 | `ap_fixed<W, I>`(W≥32) | 待 Phase 2/3 定 W,I | 須比 sample 寬以留 headroom 防溢位;運算完 clamp 回 Q1.23 |
| 參數(AXI-Lite 傳入) | `int`(32-bit) | — | PS 傳整數,IP 內部解讀為對應定點值 |

原則:
- 運算過程用較寬中間型別,**每級結束 clamp 回 [−1, +1)**(見開發規則 6)。
- 低頻 IIR 對係數量化敏感;wobble 濾波器係數**建議預先算好存查表(LUT)**,避免掃頻時即時重算造成量化誤差(文獻常見做法)。
- 中間型別 W/I 待實作 distortion / wobble 時依實際運算範圍定案並回填。

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

## 3. AXI-Lite 位址表 ✅(Phase 1 完成)

PS 寫入 → PL 即時讀取,音訊不中斷。

來源：`xprocess_sample_hw.h`（HLS 產出）+ `.hwh`（Vivado 整合後）

### Effect IP（process_sample_0）

> **IP base address：`0x4002_0000`**

| 參數 | 方向 | byte offset | 說明 |
|------|------|-------------|------|
| ap_ctrl | R/W | `0x00` | bit0=ap_start, bit1=ap_done, bit2=ap_idle |
| `in_l` | PS→PL | `0x10` | 左聲道輸入，24-bit Q1.23，低 24 bits 有效 |
| `in_r` | PS→PL | `0x18` | 右聲道輸入，24-bit Q1.23，低 24 bits 有效 |
| `out_l` | PL→PS | `0x20` | 左聲道輸出，24-bit Q1.23（read only） |
| `out_l_ap_vld` | PL→PS | `0x24` | out_l 有效旗標（read, clear on read） |
| `out_r` | PL→PS | `0x30` | 右聲道輸出，24-bit Q1.23（read only） |
| `out_r_ap_vld` | PL→PS | `0x34` | out_r 有效旗標（read, clear on read） |
| `dist_en` | PS→PL | `0x40` | distortion 開關，bit0 |
| `wobble_en` | PS→PL | `0x48` | wobble 開關，bit0 |
| `threshold` | PS→PL | `0x50` | distortion 切點（32-bit int） |
| `gain` | PS→PL | `0x58` | distortion 輸入增益（32-bit int） |
| `lfo_rate` | PS→PL | `0x60` | wobble 掃動速率（32-bit int） |
| `lfo_depth` | PS→PL | `0x68` | wobble 掃動範圍（32-bit int） |
| `state` | PS→PL | `0x70` | 跨 sample 狀態（Phase 1 為 placeholder） |

### AXI GPIO

| IP 實例 | base address | 功能 |
|---------|-------------|------|
| `axi_gpio_0` | `0x4000_0000` | GPIO=sw[1:0]（input），GPIO2=btn[3:0]（input） |
| `axi_gpio_1` | `0x4001_0000` | GPIO=led[3:0]（output） |

> AXI GPIO 暫存器 offset：DATA=0x000（ch1），DATA2=0x008（ch2），TRI=0x004，TRI2=0x00C

### AXI IIC（ADAU1761 I2C 配置）

| IP 實例 | base address | 功能 |
|---------|-------------|------|
| `axi_iic_0` | **🔲 待填（rebuild 後從 Vivado Address Editor 取得）** | ADAU1761 I2C 配置（codec PLL + 暫存器初始化） |

> 硬體接腳：SCL = U9，SDA = T9（PL IO，LVCMOS33，需 PULLUP）  
> libaudio.so 的 `iic_index` 對應此 AXI IIC 在 Linux /dev/i2c-X 的編號（rebuild + 上板後確認）

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
| Phase 1 | AXI-Lite offset 定案（from HLS xprocess_sample_hw.h）；AXI GPIO base address 定案；加入 AXI IIC 條目（address 待 rebuild 後填入） |
