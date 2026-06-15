# Bass 數位效果器(FPGA / PYNQ-Z2)專案計畫書

> 內部開發文件。亦作為日後 Claude Code 的起始文件。
> 平台:PYNQ-Z2(Zynq-7000, xc7z020) · 工具:Vivado / Vitis HLS · 團隊:Ray、Claire(2 人)

---

## 1. 專案概述

### 1.1 目標

在 PYNQ-Z2 的可重構邏輯(PL)上實作一個即時 bass 數位效果器,核心目的是**證明音訊效果運算實際跑在 FPGA 硬體上**,而非在 ARM 處理器上以軟體計算。輸入為一把被動式 bass(Fender JB62 規格),經板上 ADAU1761 codec 取樣,送進 PL 內的自訂效果電路處理,再經 codec 輸出至音箱。

本專案不追求音色品質或商業效果器水準,衡量成功的標準是:**系統能即時運作,且現場能聽出效果差異。**

### 1.2 範圍界定

| 範圍 | 內容 |
|---|---|
| **MVP(保底)** | distortion + wobble 兩個效果、按鈕單選一個效果、PS per-sample 搬運(PIO)、現場彈奏 demo |
| **MVP 後延伸** | 效果串接(兩個 switch 可同時開) |
| **必要步驟** | A→B 資料路徑升級:AXI DMA + 雙緩衝 + 中斷（時間不足 fallback：C PIO） |

### 1.3 開發策略

採「保底 + 加分」分層策略,確保任何時間點都有可 demo 的成果:

```
A(PIO,最簡單能動)  →  效果串接  →  B(DMA 優化,技術亮點)
   保底 MVP              第一延伸        加分
```

先用最簡單的 PIO 把整條資料路徑與兩個效果跑通(確保一定有東西能 demo),再往上疊串接與 DMA。每一層都建立在已驗證的下一層之上。DMA 升級（Phase 6）原列加分，因 Python PIO 音質不可接受且 libaudio 路徑與自訂 BD 不相容（詳見 decisions.md D15），已調整為必要步驟；時間不足時 fallback 為 C PIO。

### 1.4 團隊分工

Ray 與 Claire 兩人協作開發。具體模組分工視進度彈性調整,但兩人之間的**介面約定**(見第 2 節)一旦講定即凍結,作為平行開發的合約基礎。

---

## 2. 協作約定

本專案為 FPGA / Zynq 異質設計,且兩人各持一塊板子平行開發。FPGA 專案對版本與介面一致性極度敏感(同一份 tcl 在不同 Vivado 版本可能無法重現),故協作約定的優先序高於分工細節。

### 2.1 介面合約(凍結並版本化)

以下三個介面一旦講定即視為合約,任何修改必須先告知對方、改完通知,並更新文件。建議獨立維護一份 `INTERFACE.md` 於 repo。

| 介面 | 內容 | 影響 |
|---|---|---|
| `process_sample()` 簽章 | 效果運算核心函式的輸入/輸出/參數定義 | IP 內部與外殼的邊界 |
| AXI-Lite 位址表 | 每個可調參數對應的 byte offset | PS 寫參數 ↔ PL 讀參數的對應 |
| AXI GPIO bit 對應 | 哪個 switch / 按鈕對應哪個 bit、代表什麼 | PS 讀控制 ↔ 效果開關/切換 |

> FPGA 協作最痛的 bug:一方改了位址或參數順序,另一方不知道,bitstream 與 PS 程式對不上**且不報錯**,只是行為怪異。凍結介面是主要防線。

### 2.2 環境版本對齊

兩塊板子的環境必須一致,於 Phase 0 對齊並寫入文件作為基準:

- PYNQ SD card image 版本
- Vivado / Vitis HLS 版本
- 相關套件版本

指定一塊板子作為**整合 golden 板**(整合與 bitstream 生成以此為準),另一塊用於驗證可重現性。

### 2.3 版本控制約定

- **進 Git**:HLS C source、tcl 腳本、PS 端程式、文件。
- **不直接進 Git diff**:`.bit` / `.hwh` 等 binary(無法 diff、撐大 repo)。約定統一命名與同步方式(release 或共享資料夾),且 `.bit` 與 `.hwh` 必須同名成對。

### 2.4 整合同步點

兩人平行開發,但 block design 整合(IP + PS 兜起來)是必經會合點。每個 Phase 的整合節點安排「一起對接」的時間,避免各做各的到最後才合。

### 2.5 共用驗證資產

測試用的 bass 音檔、各效果的預期輸出,兩人使用同一份,確保「我板子上好的、你板子上怪的」時有共同判斷基準。

---

## 3. 背景與參考

### 3.1 Zynq SoC 異質架構

Zynq-7000 同時包含 ARM 處理器(PS, Processing System)與 FPGA 可重構邏輯(PL, Programmable Logic),兩者透過 AXI 介面溝通。本專案的職責劃分遵循此架構的標準範式:

- **PL(FPGA)**:負責高吞吐、平行的音訊效果運算 —— **本專案主體**。
- **PS(ARM)**:負責控制流與資料搬運(把音訊從 codec 端搬到 PL、處理完再搬回),是協調者而非運算者。

> 關鍵認知:效果運算(distortion / wobble)由 HLS 合成成 PL 內的硬體電路,實際處理每個 sample 的是 FPGA 邏輯,不是 ARM 在算。資源報告中的 BRAM / DSP / LUT 用量、II、bitstream 全是 PL 的產物。這是本專案「是 FPGA 專案」的立足點。

### 3.2 參考專案分析:UCSD FPGA Guitar Pedal

參考一份 PYNQ-Z2 吉他效果器專案(ping-pong delay + overdrive)。其價值在於已驗證了 PYNQ-Z2 音訊路徑的若干硬性限制與踩坑經驗。

**沿用(板子硬性限制 / 已驗證做法):**

- ADAU1761,48 kHz,立體聲,I2S;每 sample 24-bit pad 到 32-bit。
- codec 須先經 I2C 設定才動作 —— 沿用 PYNQ base overlay 既有流程。
- 開發 de-risk 順序:base overlay → `pAudio.bypass()` sanity check → 才加自訂 IP。
- 不碰純 PL I2S 路徑:該專案花大量時間發現 `sdata_i`/`sdata_o` 並非單純音訊流,純 PL 串接 I2S receiver→transmitter 行不通,最後繞道由 PS 介入。本專案直接沿用「PS 餵資料進自訂 IP」的方向,不啃 codec VHDL。
- mono 輸入餵雙聲道(配合 JB62 為 mono、line-in 為 stereo)。
- GPIO 按鈕切換效果。

**改掉(本專案優化):**

- **資料搬運**:該專案為 per-sample PIO(CPU 全程被綁、延遲高)。本專案 MVP 仍用 PIO 保底,但 Phase 6 升級為 AXI DMA + 雙緩衝 + 中斷（已列為必要步驟，見 D5/D15）。
- **不改系統 `libaudio.so`**:該專案直接改 PYNQ 系統函式庫,風險高(改壞整個音訊子系統、重刷 image 全失)。本專案於自有 userspace / notebook 操作,不動系統庫。

### 3.3 bass 與吉他的差異對設計的影響

| 差異 | 影響 |
|---|---|
| **低頻基音(~40 Hz)** | 48 kHz 下單一週期約 1200 取樣點,波形變化緩慢;在衰減/反饋/濾波運算中,接近量化精度下限的「小信號」細節易被截斷,累積成低頻雜訊。低頻 IIR 濾波器對係數量化也更敏感。 |
| **被動 pickup(高阻抗、低電平)** | 直插 line-in 會因阻抗不匹配掉高頻、染色、電平偏低(詳見 5.3)。 |
| **大動態範圍(撥弦 vs slap)** | clipping 閾值與 gain staging 需謹慎;demo 可避開 slap、用一般撥弦縮小動態範圍。 |

> 此處「小信號」指**數值上接近量化精度下限的部分**,與音量大小無關。低頻訊號即使整體很大聲,相鄰取樣點的差值仍小,易踩到精度地板。

---

## 4. 系統架構

### 4.1 整體資料流

```
JB62 (被動, mono)
   │  6.3→3.5 線 (開發階段直插; demo 可選加 DI)
   ▼
ADAU1761 line-in ──I2C 設定──┐
   │ I2S, 48kHz, 24-bit          │ (沿用 base overlay)
   ▼                             │
[audio_codec_ctrl] ◄────────────┘
   │
   ▼  PS 介入搬運 (A: PIO / B: DMA+雙緩衝+中斷)
┌──────────────────────────────────────┐
│  自訂 Effect IP (HLS) ── 跑在 PL        │
│   ├ mono→stereo 複製                    │
│   ├ process_sample(): distortion / wobble│
│   ├ 級間 clamp(防溢位,為串接鋪路)      │
│   └ AXI-Lite 參數暫存器                  │
└──────────────────────────────────────┘
   ▲
   │  AXI GPIO ← 2 switch(效果開關)+ 4 按鈕(參數切換)
   ▼
ADAU1761 HP-out ──3.5→6.3──► bass amp
```

### 4.2 PS / PL 職責劃分

| 工作 | 跑在哪 |
|---|---|
| 音訊效果運算(distortion clipping、wobble 濾波) | **PL(FPGA)** ← 主體 |
| 音訊樣本搬進/搬出 PL | PS(ARM) |
| codec I2C 設定、按鈕/switch 讀取、參數寫入 | PS(ARM) |

### 4.3 訊號格式

- 取樣率:48 kHz
- 位元深度:24-bit,pad 到 32-bit
- 通道:I2S 立體聲;JB62 為單聲道輸入,於 IP 入口複製成雙聲道處理。

### 4.4 三條通道

IP 同時掛三條獨立通道,彼此不衝突:

1. **音訊資料路徑**(高吞吐):A 階段 PIO,B 階段 AXI-Stream + DMA。
2. **參數控制**(低頻、偶爾改):AXI-Lite 暫存器,PS 寫入即時生效。
3. **效果切換 / 開關**:AXI GPIO,讀取 switch 與按鈕。

> AXI-Lite(調參)與資料路徑(PIO/DMA)互相獨立,故參數控制設計在 A、B 階段共用,升級時不需更動。

---

## 5. 硬體設計

### 5.1 PYNQ-Z2 / ADAU1761 硬性規格與限制

- SoC:Xilinx Zynq XC7Z020-1CLG400C(dual-core Cortex-A9 + xc7z020 PL)。
- 音訊:ADAU1761 codec,3.5mm HP/Mic jack + 3.5mm line-in jack,支援立體聲 48 kHz 錄放(取樣率 8 kHz–96 kHz),具數位音量控制。
- codec 經 I2C 由 PS 設定。
- 板上資源:4 按鈕、2 slide switch、4 LED、2 RGB LED。

### 5.2 訊號鏈與接線

板上兩個 jack 皆為 3.5mm,bass 與 amp 皆為 6.3mm(¼"),需轉接。輸入輸出分開處理:

**輸入端(bass → 板子 line-in):**
- 6.3mm(TS, mono)→ 3.5mm 線或轉接頭。
- bass 為 mono(TS),line-in 為 stereo(TRS);訊號預設只進單聲道,於 IP 內複製成雙聲道。
- (可選)中間插主動 DI:bass → DI → DI line out → 3.5mm → line-in。

**輸出端(板子 HP-out → amp):**
- 3.5mm(TRS)→ 6.3mm 線,接 bass amp input。
- HP-out 電平可能偏高,先把板子數位音量與 amp gain 調低再慢慢開,避免爆音。

最小接線:6.3→3.5 線 ×1(輸入)、3.5→6.3 線 ×1(輸出);DI 視需要。

### 5.3 阻抗問題與對 demo 的實際影響

JB62 為被動式(典型輸出阻抗約 7–10 kΩ 級、輸出電平約 1 V),line-in 輸入阻抗通常僅約 10–20 kΩ,被動樂器理想上需 1 MΩ 以上負載。直插 line-in 會發生:

1. **高頻衰減** → 聲音變悶(pickup 電感與低負載形成低通)。
2. **頻率染色** → 低頻樂器可能出現不自然的「呱呱」感。
3. **電平偏低 + 噪訊比變差** → 有效位元數變少,與低頻定點量化問題疊加。

**對專案的影響分級:**

| 層面 | 影響 |
|---|---|
| 系統能否運作 | **不受影響**:ADC 仍收得到訊號,效果開發與驗證照常。 |
| 音色品質 | 偏悶、可能帶怪音、噪訊偏高。 |
| 與量化疊加 | 安靜段落底噪較明顯。 |

**對策**:開發階段直插(不擋進度);demo 求音質時可加一個便宜主動 DI 救回高頻與電平。本專案不追求音色,直插即可 demo,DI 為可選。

### 5.4 板上資源運用

- **2 slide switch**:各控一個效果的 on/off(distortion / wobble)。兩個同開 = 串接(Phase 5)。
- **4 按鈕**:切換預設參數組(如快/慢 wobble、輕/重 distortion)。
- 皆透過 AXI GPIO 讀取(一個 bit 一個開關/按鈕)。

---

## 6. PL 設計:Effect IP(HLS)

### 6.1 IP 介面設計原則:運算與外殼解耦

**為 A→B 升級鋪路的核心約定**:效果運算寫成獨立純函式 `process_sample()`(吃一個 sample、回一個 sample,不綁定任何 AXI/mmap/stream 細節)。

- A 階段(PIO):外殼以 PIO 方式逐 sample 呼叫 `process_sample()`。
- B 階段(DMA):外殼改為 AXI-Stream loop,反覆呼叫**同一個** `process_sample()`。

如此 A→B 升級時運算核心零修改,只換外殼。符合 module-isolated 開發。

### 6.2 distortion

- 演算法:hard clipping —— 輸入經 gain 放大後,超過 threshold 即硬切。全頻處理,不分頻,接受音色「糊一點」。
- 參數:`threshold`(切點)、`gain`(輸入增益)。
- 資源:極低(少量比較與乘法)。

### 6.3 wobble

- 演算法:一階 IIR low-pass,由 LFO(低頻振盪器)掃動截止頻率,產生週期性的「哇~嗚~」音色。
- 參數:`lfo_rate`(掃動速率)、`lfo_depth`(掃動範圍)。
- **注意事項**:
  - 一階先求穩(理論門檻低、低頻 IIR 係數量化敏感);效果不足再考慮加深。
  - 截止頻率掃動時係數需即時重算 —— 定點數陷阱,實作時細處理。
  - 改 `lfo_rate` 時 LFO 相位不可突跳(會有 click 聲),需平滑接續。

### 6.4 mono→stereo 處理

IP 入口將單聲道輸入複製到兩聲道後續處理(沿用參考專案做法)。

### 6.5 AXI-Lite 參數暫存器設計

四個參數各開一個 AXI-Lite 暫存器,PS 寫入即時生效,不需重新合成 bitstream。

| 參數 | 效果 | 說明 |
|---|---|---|
| `threshold` | distortion | 切點(失真程度) |
| `gain` | distortion | 輸入增益 |
| `lfo_rate` | wobble | 掃動速率(快/慢) |
| `lfo_depth` | wobble | 掃動範圍(明顯程度) |

HLS 以 `#pragma HLS INTERFACE s_axilite port=...` 將參數合成成帶位址的 PL 暫存器;PS 端對對應 byte offset 寫值即可。位址表納入介面合約(2.1)。

### 6.6 定點數格式與低頻量化考量

- 先定 Q 格式(候選 Q1.23 對應 24-bit 音訊資料、中間運算用較寬位元),作為所有 DSP 模組共同前提。
- 低頻 + 反饋/濾波運算易放大量化誤差(見 3.3);MVP 的 wobble 為一階、distortion 無深度反饋,問題相對可控。
- 沿用參考專案的 bitwidth 最佳化思路(縮減過寬中間值、以 bitshift 取代除法)以降資源用量,但**不以此為 MVP 必要項**。

### 6.7 級間 clamp 與防溢位(為串接鋪路)

每一級運算後做 clamp,將值限制在合法範圍。如此單效果不溢位,串接(Phase 5)時兩級連續放大也不會 wrap-around 爆音。呼應 distortion / wobble 本身需要的 clipping。

---

## 7. PS 設計:控制與資料搬運

### 7.1 A 階段:per-sample PIO 搬運

PS 在迴圈中逐 sample 將音訊寫入 IP 輸入、讀回 IP 輸出。最簡單、能動;CPU 全程參與、延遲較高,但在可接受範圍(目標延遲遠寬於演奏舒適區)。

### 7.2 codec I2C 設定

沿用 PYNQ base overlay 既有 I2C 設定流程,不自行實作。

### 7.3 參數控制與效果切換的 PS 端操作

- **參數**:對 AXI-Lite 對應位址寫值(如 notebook 內 `effect.write(THRESHOLD_OFFSET, 5000)`),即時改變 PL 行為。
- **切換 / 開關**:讀 AXI GPIO 取得 switch 與按鈕狀態,對應到效果 enable 位元或預設參數組。

### 7.4 B 階段預留

IP 外殼以 AXI-Stream 介面設計,使升級 DMA 時:運算核心(`process_sample()`)不動,僅改外殼介面、block design 加 DMA、PS 改為 DMA + 中斷 + 雙緩衝管理(詳見第 9 節 Phase 6 與 A→B 成本)。

---

## 8. Block Design 整合

### 8.1 從 base overlay 裁剪

以 PYNQ-Z2 base overlay 為起點,保留與 `audio_codec_ctrl` 相關的音訊路徑,移除其餘無關元件,作為自訂設計的基準。

### 8.2 加入自訂元件

- 接入 Effect IP(AXI-Lite 參數 port + 音訊資料 port)。
- 接入 AXI GPIO,連接 2 switch + 4 按鈕(於 .xdc 暴露對應接腳,port 命名須與約束檔一致)。

### 8.3 位址映射與 bitstream

於 Address Editor 取得各 port / 控制訊號的 base address 與 byte offset(納入介面合約 2.1),產生 bitstream,輸出同名的 `.bit` 與 `.hwh`。

---

## 9. 開發階段規劃(里程碑)

每個 Phase 含:目標、產出、Exit Criteria(**通過才進下一階段**)、預計小時。Exit Criteria 為硬性門檻 —— 採分層驗證,確保出問題時永遠知道是哪一層,因前面每層皆已驗證。

### Phase 0 — 環境與硬體 sanity check

- **目標**:對齊兩塊板子環境;確認硬體與接線可通。
- **產出**:環境版本基準文件;JB62 直插下 `pAudio.bypass()` 出聲。
- **產出不含**:任何 HLS。
- **Exit Criteria**:JB62 直插,`bypass()` 能從 amp 聽到 bass 聲(悶可接受);兩塊板子環境一致。
- **預計**:6 小時。

### Phase 1 — 最小 IP(passthrough)

- **目標**:打通自訂 IP 的完整資料路徑(尚無效果)。
- **產出**:passthrough Effect IP(內含 `process_sample()` 骨架,直接回傳輸入)+ 自訂 block design + PS PIO 搬運。
- **Exit Criteria**:聲音能穿過自訂 IP 從 amp 出來,與 bypass 聽感一致(證明路徑通)。
- **預計**:12 小時。

### Phase 2 — distortion

- **目標**:第一個可聽效果。
- **產出**:`process_sample()` 內 hard clipping;AXI-Lite 接 threshold/gain。
- **Exit Criteria**:能聽到失真;調 threshold/gain 聽得到變化;不爆音(clamp 生效)。離線 testbench 對單一音檔驗證通過。
- **預計**:10 小時。

### Phase 3 — wobble

- **目標**:第二個可聽效果。
- **產出**:一階 IIR + LFO 掃頻;AXI-Lite 接 lfo_rate/lfo_depth;相位平滑。
- **Exit Criteria**:能聽到週期性掃動;調 lfo_rate 聽得到快慢變化;無 click、不溢位。離線驗證通過。
- **預計**:14 小時。

### Phase 4 — 按鈕單選切換 + AXI-Lite 調參(MVP 完成)

- **目標**:完成可 demo 的 MVP。
- **產出**:AXI GPIO 讀按鈕,單選 bypass / distortion / wobble;notebook 即時調參流程。
- **Exit Criteria**:現場彈奏,按鈕在三種狀態間切換正常,各效果如預期;**MVP 達成**。
- **預計**:8 小時。

### Phase 5 — 效果串接(MVP 後第一延伸)

- **前置**:Phase 2、Phase 3 各自通過。
- **目標**:2 switch 可同時開,訊號串接(bass → distortion → wobble)。
- **產出**:IP 內依 switch enable 旗標串接;級間 clamp 防爆音。
- **Exit Criteria**:兩效果可獨立開關;同開時串接生效且不爆音(電平管理 OK)。
- **預計**:6 小時。

### Phase 6 — A→B 升級:DMA + 雙緩衝 + 中斷（必要；時間不足 fallback C PIO）

- **目標**:以 DMA 取代 PIO,降低延遲、解放 CPU,作為技術亮點。
- **產出**:IP 介面改 AXI-Stream;block design 加 AXI DMA(MM2S/S2MM)+ 中斷線到 PS;PS 改為 DMA 描述符 + 中斷處理 + ping-pong 雙緩衝。
- **Exit Criteria**:DMA 搬運下音訊正常、效果不變;延遲較 PIO 改善;`process_sample()` 未修改。
- **預計**:30 小時(含 block design 中斷/DMA 接線除錯緩衝)。

**A→B 改動成本拆解(供 Phase 6 參考):**

| 改動項 | 成本 | 說明 |
|---|---|---|
| 運算核心 `process_sample()` | 0 | 解耦設計的回報 |
| IP 資料介面 | 小(0.5–1 天) | 改 AXI-Stream + pragma,包 loop |
| Block design | 中(1–2 天) | 加 DMA、接 stream、接中斷、重生 bitstream |
| PS 端軟體 | 大(2–3 天) | DMA 設定 + 中斷 + 雙緩衝管理(真正難點) |

---

## 10. 測試與驗證計畫

### 10.1 分層驗證策略

硬體層 → IP 層 → 整合層,逐層驗證。每層通過才往上(對應第 9 節 Exit Criteria)。出問題時因下層已驗證,可快速定位至當前層。

### 10.2 IP 離線驗證

以 HLS testbench 對單一 bass 音檔套用效果並存檔,確認 distortion / wobble 行為符合預期(可離線比對波形)。在進實機前先排除 IP 邏輯錯誤。

### 10.3 現場即時驗證 + 備援

- 主:現場彈奏 JB62,即時聽效果與切換。
- 備援:預錄一段 bass 音檔可跑效果(若 PS 端載入音檔太繁瑣,則改以現場 demo + 手機錄影備份)。確保 demo 一定有可聽成果。

### 10.4 資源使用與時序檢查

對照 xc7z020 預算檢查 DSP / BRAM / LUT / FF 用量與時序收斂。MVP 效果簡單,預期用量極低(參考專案最終 LUT 4%、DSP/BRAM 7% 量級),資源非瓶頸;DATAFLOW、非必要 UNROLL 為已知過用元凶,避免之。

---

## 11. 風險與對策

| 風險 | 等級 | 徵兆 | 對策 | 擋 demo? |
|---|---|---|---|---|
| PIO 延遲 | 低 | 直通有可感延遲 | 目標延遲遠寬於演奏舒適區;不行再上 DMA | 否 |
| 阻抗音質劣化 | 低 | 聲音悶、呱呱、噪訊 | 開發直插;demo 可加主動 DI | 否 |
| 低頻 IIR 量化不穩 | 中 | wobble 失真、雜訊、不穩 | 一階先行;clamp;謹慎選 Q 格式與係數 | 部分(僅 wobble) |
| 串接爆音 | 中 | 同開時破音/wrap | 級間 clamp;級間電平管理 | 否(串接為延伸) |
| DMA / 中斷除錯 | 高 | bitstream「看似對卻不動」 | 留充足除錯緩衝;先 PIO 保底;時間不足 fallback C PIO | 否(PIO 先保底) |
| I2S codec 整合 | 中 | codec 不發聲 | 沿用 base overlay,不碰純 PL I2S | 是(Phase 0 須先過) |
| 環境版本不一致 | 中 | 一板可重現另一板不行 | Phase 0 對齊;指定 golden 板 | 可能 |
| 時程 | 中 | 加分項做不完 | 分層策略,MVP(Phase 0–4)即可交付 | 否 |

---

## 12. 預期成果與 demo 形式

### 12.1 MVP 交付物(Phase 0–4)

- 在 PL 上運作的 bass 效果器:distortion + wobble,按鈕單選。
- 現場彈奏即時處理,AXI-Lite 即時調參。
- 自訂 block design、HLS IP source、PS 控制程式。

### 12.2 延伸 / 加分交付物

- 效果串接(Phase 5)。
- DMA + 雙緩衝 + 中斷的低延遲資料路徑(Phase 6，必要步驟)。
- 資源使用與(若做)最佳化分析。

### 12.3 demo 腳本(現場為主)

1. 接上 JB62 與 amp,展示 bypass 乾淨直通(證明訊號鏈)。
2. 按鈕切到 distortion,彈奏並調 gain,展示失真由輕到重。
3. 按鈕切到 wobble,調 lfo_rate,展示掃動快慢變化。
4. (若 Phase 5 完成)兩 switch 同開,展示串接。
5. (若 Phase 6 完成)說明資料路徑由 PIO 升級 DMA,強調運算全程在 PL。
6. 備援:預錄音檔 / 錄影,確保即使現場出狀況仍有成果可呈現。

---

## 13. 時程總表

| Phase | 內容 | 預計小時 |
|---|---|---|
| 0 | 環境與硬體 sanity check | 6 |
| 1 | 最小 IP(passthrough) | 12 |
| 2 | distortion | 10 |
| 3 | wobble | 14 |
| 4 | 按鈕單選 + AXI-Lite(MVP 完成) | 8 |
| **MVP 小計(0–4)** | | **50** |
| 5 | 效果串接 | 6 |
| 6 | A→B:DMA + 雙緩衝 + 中斷 | 30 |
| **延伸 + 加分小計(5–6)** | | **36** |
| **總計** | | **86** |

> 小時為估算,含除錯緩衝。Phase 6 不確定性最高(block design 中斷/DMA 除錯)，已列為必要步驟；時間不足時 fallback 為 C PIO（板上編譯 C 程式輪詢 MMIO，可跟上 48 kHz）。MVP(0–4)為保底可交付範圍。

---

## 14. Post-MVP 音質優化待辦（板上實測發現，2026-06-14）

以下問題在 Phase 3+6 整合板上實測後發現，不阻擋 MVP（Phase 4）完成，列為 MVP 後的優化項目。

### 14.1 Wobble 效果深度不足（D26）

**症狀**：wobble 掃動效果太細微，與 distortion 串接時尤為明顯。  
**根因**：B_LUT 低端 fc≈200 Hz 高於 bass 基音（41–98 Hz），LFO 最低值時基音仍在 passband；加上一階 IIR（6 dB/oct）斜率太緩，開合幅度有限。  
**處置（2026-06-15）**：同時執行 B_LUT 調整 + 升 2nd-order IIR cascade + 新增 lfo_floor runtime 切換。

| 項目 | 說明 |
|------|------|
| B_LUT 新範圍 | 10–2000 Hz 對數等比（原 200–9300 Hz）；`wobble.cpp` |
| 2nd-order cascade | 串聯兩級一階 IIR → 12 dB/oct；state_t 加 `iir_prev2_L/R` |
| 效果 | 80 Hz bass 在 LFO 最低值時衰減 ~36 dB（原 ~0 dB） |
| `lfo_floor` 參數 | 新增 AXI-Lite 參數（offset 0x48 待 synthesis 確認）；控制 B_LUT 最小 index，即波谷截止頻率 |
| btn2 preset cycle | A（floor=6，fc≈83 Hz，−6 dB）→ B（floor=4，fc≈41 Hz，−18 dB）→ C（floor=0，fc=10 Hz，−36 dB）→ A |
| 介面影響 | state_t 為 HLS 內部 static；`lfo_floor` 為新增 AXI-Lite 暫存器（需 INTERFACE.md + re-synthesis）|

**待驗證**：HLS C-sim PASS → synthesis II=1（確認 lfo_floor offset 0x48）→ Vivado rebuild → 板上音訊驗聽三個 preset。

### 14.2 Distortion 高 gain 雜訊放大（D27）

**症狀**：gain 調高時底噪（noise floor）明顯放大，與 passthrough / wobble 相比差異顯著。  
**根因**：hard clipping 在 clip 前對全頻訊號（含底噪）做 gain 倍放大；被動 bass 直插 line-in 阻抗不匹配使底噪源頭偏高。  
**優化方向**（依實作成本排序）：

| 選項 | 成本 | 說明 |
|------|------|------|
| 加 noise gate（`\|in\| < noise_thr → out=0`） | 低（`distortion.cpp` 加一個 `if`） | 直接消除底噪；可加 AXI-Lite 參數動態調整門限 |
| Soft-knee clipping | 中（換 clipping 函式） | 減少截波高諧波，但不解決底噪 |
| 硬體 DI（demo 佈置） | 不改 IP | 改善阻抗匹配，降低底噪源頭 |

**建議**：先加 noise gate，實作最簡單且效果直接。若新增 `noise_threshold` 為 AXI-Lite 參數，需更新 `INTERFACE.md` 並通知 Claire。

### 14.3 RGB LED（LD4/LD5）亮度過高（板上實測，2026-06-14）

**症狀**：LD4/LD5 全彩（R+G+B 三 channel 同亮）太刺眼，demo 環境下不舒適。  
**根因**：`audio_dma.c` 目前驅動全部三個 channel（`RGBLED_LD4=0x07`，`RGBLED_LD5=0x38`）。  
**處置（2026-06-15）**：改為只驅綠色單 channel（`RGBLED_LD4=0x02` bit1=G17，`RGBLED_LD5=0x10` bit4=L14）。仍偏亮但在可接受範圍，暫不繼續調整。無需重合成 bitstream。

### 14.4 高 gain + 串接時外接主動式喇叭 crash（板上實測，2026-06-14）

**症狀 A（最初發現）**：sw[0]+sw[1] 同開、distortion 設 high、大力撥弦，KRK Rokit 5（studio monitor）會保護電路跳；EarPods 不會。  
**症狀 B（2026-06-15 追加）**：頻繁快速切換 sw[0] / sw[1]（開關效果），KRK Rokit 5 也會 crash。根因尚未確認，可能原因：switch 瞬間切換時 PS 寫入 AXI-Lite enable 寄存器與 DMA 傳輸競爭，導致 out_buf 短暫輸出未定義值（極大電平脈衝）。  
**根因（症狀 A）**：hard clipping 後 RMS 大幅上升，輸出電平超過 KRK Rokit 5 line in 額定（+4 dBu），觸發保護。EarPods 為被動高阻抗負載，不受影響。  
**優化方向**（依實作成本排序）：

| 選項 | 成本 | 說明 |
|------|------|------|
| 降低 `DIST_GAIN_HIGH` 預設值（12 → 8）| 低（改一個 `#define`，重編譯）| 直接壓低電平，對症狀 A 最快 |
| 在 `audio_loop()` 對 `out_buf` 整體縮放（× 0.5）| 低（純 PS C，不改 HLS）| 全域輸出衰減，對所有效果生效 |
| 調查 sw 切換時序，確認是否有輸出脈衝 | 低（加 log 或示波器量）| 釐清症狀 B 根因 |
| 在 HLS 加 output gain 參數 | 中（需重合成 bitstream）| 最靈活，可即時調整 |

**建議**：先降 `DIST_GAIN_HIGH`（PS 端改一行）解決症狀 A；症狀 B 根因待下次遇到時進一步調查。

### 14.5 codec_init.py + audio_dma 整合（易用性，2026-06-14）

**症狀**：目前需兩步啟動（先 `sudo python3 codec_init.py`，再 `sudo ./audio_dma`），容易忘記，會造成 Bus Error。  
**優化方向**：

| 選項 | 成本 | 說明 |
|------|------|------|
| Shell wrapper script（`start.sh`）| 低 | 一行 `./start.sh` 依序呼叫兩個程式 |
| 將 overlay 載入 + codec init 移入 `audio_dma.c`（C + Python C-API 或 system()）| 中 | 單一二進位，但混 Python/C 複雜 |
| 用 Python 重寫整個 audio loop（pynq + cffi）| 高 | 維護負擔大，不建議 |

**建議**：先做 `start.sh`（最低成本），長期可考慮把 overlay 載入用 C 的 `/dev/mem` 直接燒 bitstream（有 PYNQ precedent）。

### 14.6 Production cleanup（功能性 TODO 全完成後）

**目標**：將 `audio_dma.c`（及相關 PS 程式）整理成部署版本，移除開發期輔助程式碼。  
**包含**：

| 項目 | 說明 |
|------|------|
| 移除 boot 診斷 print | `[diag:]`、`[phys-verify]`、`[boot]`、`[dma]` 等一次性 log |
| 移除 `/dev/mem` cross-verify 區塊 | 驗證 cma phys addr 正確性的程式碼，上線後不需要 |
| 移除 `diag()` / `diag_buf()` 函式 | 純除錯用，production 不呼叫 |
| 保留必要 log | `[ctrl]` preset 切換 log 可保留（demo 時有助確認操作）；或改 `#ifdef DEBUG` 控制 |
| 確認 compile flag | `-O2` 保留；考慮是否加 `-DNDEBUG` |

**時機**：所有功能性 TODO（14.1–14.5）完成、音質驗收通過後執行。  
**注意**：cleanup 前先開新 branch（`release/production` 或 `chore/cleanup`），保留 debug 版 history。

---

### 14.7 Ground loop 雜訊 + 前置 HPF 優化（或許可做）

**現象**：接上 bass 後有些微底噪；手碰 PYNQ 接地腳與 bass 弦（地）時雜訊消失，判斷為 **ground loop hum**（50/60 Hz 及諧波），與 14.2 distortion 放大雜訊有關聯。

**方向 A — 類比接地（最直接）**：拉一條線連接 PYNQ 機殼地與 bass 導線地，消除 ground loop 本身。Ray 已評估可行。

**方向 B — 數位前置 HPF（輔助優化，順手可做）**：在 `process_sample()` 最前端串接一階 IIR 高通濾波器（Fc ≈ 25–30 Hz），截掉 DC offset 與超低頻雜訊，使 distortion 不會把這段雜訊一起放大。
- 只需 2 個乘法器（DSP48E1 資源成本極低）
- bass 最低音（低 E ≈ 41 Hz）基頻影響可忽略
- 若想更精準打 50/60 Hz：改用二階 IIR notch filter

**優先序**：先試方向 A（一條線）；若底噪仍殘留再考慮方向 B。

---

## 16. 參考資料

- UCSD FPGA Guitar Pedal 專案報告(ping-pong delay + overdrive on PYNQ-Z2)。
- PYNQ-Z2 Reference Manual / PYNQ readthedocs(Audio module、base overlay)。
- Xilinx/PYNQ GitHub:`boards/Pynq-Z2/base`。
- Vitis HLS AXI-Lite / AXI-Stream interface 文件。
- DAFX: Digital Audio Effects(濾波器與延遲線理論基礎)。
