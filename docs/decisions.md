# decisions.md — 設計決策紀錄

記錄本專案各項設計決策的**選項、理由、參考依據**,供日後撰寫報告與回溯設計脈絡。每筆決策格式:背景 → 選項 → 決定 → 理由 → 參考。

---

## D1. 題目:bass 數位效果器(FPGA / PYNQ-Z2)

- **背景**:期末專案需一個範圍可控、硬體現成、可漸進驗證的 FPGA 題目。
- **決定**:在 PYNQ-Z2 的 PL 上實作即時 bass 數位效果器。
- **理由**:
  - bass 頻率低(基音約 40–400 Hz),對取樣率要求低(48 kHz 綽綽有餘),時序壓力小。
  - PYNQ-Z2 內建 ADAU1761 codec(含 line-in / HP-out),不需外接 ADC/DAC,對 2 人組時間有限的專案是實際優勢。
  - 效果器適合模組化、逐步驗證,契合團隊 module-isolated 的開發習慣。
- **參考**:PYNQ-Z2 Reference Manual(codec 規格);一份 PYNQ-Z2 吉他效果器專案(下稱「參考專案」)證明此平台可行。

---

## D2. 參考專案的取捨:沿用什麼、改掉什麼

- **背景**:找到一份高度相似的 PYNQ-Z2 吉他效果器專案(ping-pong delay + overdrive,HLS IP + memory-mapped IO),可作參考。
- **決定**:選擇性沿用,而非照抄。
- **沿用(板子硬性限制 / 已驗證做法)**:
  - codec 規格與 I2S 資料格式(48 kHz、24-bit pad 32)。
  - 開發 de-risk 順序:base overlay → `pAudio.bypass()` sanity check → 才加自訂 IP。
  - 不碰純 PL I2S 路徑(該專案踩坑證明 `sdata_i`/`sdata_o` 非單純音訊流,純 PL 串接行不通)。
  - mono 輸入餵雙聲道、GPIO 按鈕切換效果。
- **改掉**:
  - 資料搬運:該專案用 per-sample PIO;本專案規劃升級 DMA(見 D5)。
  - 不修改系統 `libaudio.so`(該專案直接改系統庫,風險高)。
- **理由**:沿用已驗證的硬性限制節省時間;改掉的部分是該專案的已知弱點,亦為本專案的工程賣點。
- **參考**:參考專案報告(Challenges 段落明確記錄 I2S 踩坑與繞道過程)。

---

## D3. bass vs 吉他的差異處理

- **背景**:參考專案用吉他,本專案用被動 bass(Fender JB62 規格),低頻 + 被動 pickup 帶來新問題。
- **決定**:正視差異但不為音色過度設計,以「能 demo、聽得出效果」為底線。
- **關鍵差異與對策**:
  - **低頻定點量化**:低頻訊號波形變化緩慢,接近量化精度下限的「小信號」細節易在運算中被截斷。對策:24-bit datapath、wobble 用一階濾波先求穩、係數預先查表。
  - **大動態範圍**:demo 避開 slap、用一般撥弦縮小動態範圍。
  - **被動 pickup 阻抗**:見 D4。
- **參考**:DAFx 文獻(24-bit datapath、濾波器係數預算查表為常見做法);bass 演奏與訊號特性常識。

---

## D4. 阻抗:被動 JB62 直插 line-in 的取捨

- **背景**:JB62 為被動式(高阻抗約 7–10 kΩ 級、低電平約 1 V),PYNQ-Z2 line-in 輸入阻抗僅約 10–20 kΩ,被動樂器理想需 1 MΩ 以上負載。
- **選項**:(A) 直插;(B) 加主動 DI / buffer;(C) 自製 op-amp buffer。
- **決定**:開發階段直插;demo 求音質時再評估加便宜主動 DI。不自製 buffer。
- **理由**:
  - 阻抗不匹配會掉高頻、頻率染色、電平偏低,但**不影響系統運作**(ADC 仍收得到訊號,效果開發/驗證照常)。
  - 本專案不追求音色,直插即可 demo;DI 為可選的音質補救,不值得在開發初期投入。
  - 自製 buffer 會佔用本該投入 FPGA 的時間。
- **參考**:TalkBass / StewMac / Gollihur 等樂器阻抗匹配討論(被動 pickup 需高阻抗負載、低負載造成高頻衰減與低頻「quacky」染色);查證確認 PYNQ-Z2 codec 為 ADAU1761 + 3.5mm line-in。

---

## D5. 資料路徑:PIO 先行,DMA 為必要步驟(A→B 分層)

- **背景**:音訊樣本需在 PS 與 PL 之間搬運。參考專案用 per-sample PIO(CPU 全程被綁、延遲高)。
- **選項**:(A) per-sample PIO;(B) AXI DMA + 雙緩衝 + 中斷。
- **決定**:MVP 用 A 保底,跑通後升級 B。**Phase 6 DMA 升級原列「加分」，已調整為必要步驟**；理由見 D15（Python PIO 音質不可接受、libaudio 路徑在自訂 BD 下無法直接修復）。時間不足時 fallback 為 C PIO（板上編譯 C 程式輪詢 MMIO，可跟上 48 kHz，無需 DMA 硬體改動）。
- **理由**:
  - A 最簡單、確保一定有可 demo 成果;B 延遲低、CPU 解放,且「識別 PIO 瓶頸並改 DMA」是漂亮的工程敘事。
  - 延遲容忍度經確認可放寬(目標 <100 ms,遠寬於演奏舒適區),故 A 的高延遲不擋 demo。
  - 升級成本可控,前提是運算核心解耦(見 D6)。
  - 社群實際做法（Audio-Lab-PYNQ）亦直接採 AXI-DMA，不走 libaudio，印證此方向正確。
- **A→B 成本估**:運算核心 0 改動;IP 介面小改(改 AXI-Stream);block design 中改(加 DMA + 中斷);PS 端大改(DMA 設定 + 中斷 + 雙緩衝)。約 4–6 工作天。
- **參考**:參考專案的 PIO 實作;Zynq DMA / 中斷標準範式;Audio-Lab-PYNQ（https://github.com/cramsay/Audio-Lab-PYNQ）採 AXI-DMA + smbus2 驗證此路線可行。

---

## D6. 運算核心與外殼解耦(process_sample 約定)

- **背景**:A→B 升級若處理不當會導致 IP 重寫。
- **決定**:效果運算寫成獨立純函式 `process_sample()`,不綁定任何 AXI/mmap/stream 細節;A、B 階段共用同一份。
- **理由**:升級 DMA 時運算核心零修改,只換外殼,把 A→B 成本壓在「改外殼」而非「重寫」。亦契合 module-isolated 開發。
- **參考**:團隊既有模組化開發習慣(各模組獨立驗證再整合)。

---

## D7. 效果選擇:distortion + wobble

- **背景**:bass 效果中,不同效果的演算法難度差異大;理論背景偏弱,需選辨識度高且實作可控者。
- **選項**:distortion、wobble、compressor、octaver、reverb、chorus 等。
- **決定**:MVP 做 distortion(全頻 hard clipping)+ wobble(一階 IIR + LFO 掃頻)。
- **理由**:
  - 兩者辨識度高,demo 易聽出差異。
  - distortion 用全頻簡單 clipping(不分頻,接受音色糊),演算法淺。
  - compressor / octaver / 分頻失真等 bass 招牌效果偏難(envelope detection、pitch tracking、crossover),不適合 MVP。
  - wobble 一階先求穩(低頻 IIR 係數量化敏感)。
- **參考**:參考專案的 distortion(gain × 輸入,超過 threshold 即 clip — 與本專案一致);Cal Poly 吉他效果器專案(distortion 的 gain/threshold 兩參數設計);bass 效果難度分布常識。

---

## D8. 控制三通道分離:資料 / 參數 / 開關

- **背景**:IP 需同時處理高吞吐音訊、偶爾改的參數、開關訊號。
- **決定**:三條獨立通道 —— 音訊資料(PIO/DMA)、參數(AXI-Lite)、開關(AXI GPIO)。
- **理由**:三者頻率與性質不同,分開通道符合 Zynq IP 標準設計;AXI-Lite 參數控制與資料路徑獨立,故 A/B 階段共用不需改。
- **參考**:Vitis HLS AXI-Lite / AXI-Stream 介面標準用法;參考專案以 AXI-Lite 傳 delay_samples / wet_mix / feedback_gain。

---

## D9. 定點數格式:Q1.23

- **背景**:需定全專案共用的定點數格式。
- **決定**:音訊 sample 用 `ap_fixed<24,1>`(Q1.23);中間運算用更寬型別(W≥32)留 headroom,運算後 clamp 回 24-bit;參數以 int 經 AXI-Lite 傳入。
- **理由**:對應 codec 24-bit 取樣;小數表示適合效果運算(乘係數、衰減);寬中間型別 + clamp 防溢位。
- **參考**:Cal Poly 專案(24-bit ADC/DAC 精度);DAFx 文獻(I2S 轉 24-bit 並列處理、係數預算查表);參考專案(24-bit datapath、ap_int<24>)。確認此為此類專案的業界/學術標準做法。

---

## D10. 文件結構:CLAUDE.md / project_plan / INTERFACE / phaseN(做到才展開)

- **背景**:專案兼作 Claude Code 起始文件,需區分人讀的藍圖與機器讀的操作指引。
- **決定**:
  - `project_plan.md`:藍圖與決策(人讀)。
  - `CLAUDE.md`:操作指引與規則(Claude Code 每次載入)。
  - `INTERFACE.md`:介面合約(凍結)。
  - `docs/phaseN.md`:各 Phase 細步驟,**做到該 Phase 才展開**。
- **理由**:CLAUDE.md 放反覆遵守的規則與環境細節,不重複藍圖論述以免稀釋重點;phaseN.md 採「做到才生」,因前期結果常影響後期細節,過早寫好易需大改。
- **參考**:團隊既有專案(overlay driver)的 CLAUDE.md + 分 phase 文件結構,已驗證好用。

---

## D11. HLS 檔案結構:同一個 IP,拆成三個 .cpp

- **背景**:distortion 由 Ray 負責,wobble 由 Claire 負責,兩人平行開發。Claire 對 Git 較不熟悉,若兩人改同一個檔案容易產生 merge conflict。
- **選項**:
  - A) 同一個 IP、同一個 `process_sample.cpp`(兩人共改)
  - B) 同一個 IP、拆成 `distortion.cpp` + `wobble.cpp` + `process_sample.cpp` 薄包裝層
  - C) 兩個獨立 IP(各自 HLS 專案、各自 AXI-Lite)
- **決定**:選 B。
- **理由**:
  - Claire 只需 pull/push 自己的 `wobble.cpp`,Git 操作最簡單,幾乎無 conflict。
  - HLS 合成仍以 `process_sample.cpp` 為入口點,export 成單一 IP,Vivado block design 不增加複雜度。
  - 相比 C,block design 維持一條 AXI-Lite、一個 IP,Phase 5 串接邏輯也在 HLS 內部完成,不需在 block design 繞線。
- **檔案責任歸屬**:
  - `hls/effect_ip/process_sample.cpp` — Ray 維護(薄薄的整合層,呼叫兩個子效果)
  - `hls/effect_ip/distortion.cpp` — Ray 全責
  - `hls/effect_ip/wobble.cpp` — Claire 全責
- **參考**:C 語言多檔編譯慣例(main.c 呼叫子模組函式,編譯成一個 binary);分工設計討論(2026-06-06)。

---

---

## D12. MCLK 到 IO 的佈線方式：ODDR

- **背景**：clk_wiz 產生 10 MHz MCLK 給 ADAU1761（接腳 U5）。FPGA 的 clock network 不能直接驅動 IO pin，Vivado 會報 `IO Clock Placer failed`。
- **選項**：
  - A) ODDR primitive：把 clock 訊號轉成 data output 再驅動 IO（標準做法）
  - B) 直接接 BD port（`-type clk`）+ `CLOCK_DEDICATED_ROUTE FALSE`
  - C) 在 BD 中直接拉外部 port，不加任何 constraint（可能 build 失敗）
- **決定**：選 A，實作獨立模組 `rtl/clk_oddr.v`，在 BD 中以 module reference 加入。
- **理由**：
  - B 需要找到 post-synthesis net 名稱才能在 XDC 生效（hierarchical path 複雜，容易出錯）；
  - A 為 Xilinx 7 系列官方推薦做法，明確且不依賴 net 名稱。
  - 實測：`audio_clk_10MHz` 成功 place 到 U5（Bank 13, IOB_X0Y11），Bitstream 生成 OK。
- **附帶發現**：`audio_codec_ctrl` BD 產生的 wrapper port 名稱帶 `_0` 後綴（`bclk_0`, `lrclk_0` 等），XDC 必須對齊此命名。
- **參考**：Xilinx UG471（ODDR primitive）；參考專案（Audio-Lab-PYNQ）直接接 BD port 但未遇相同錯誤（可能版本差異或時序容忍）。

---

## D13. ADAU1761 I2C 路徑：需 AXI IIC IP 在 PL

- **背景**：`AudioADAU1761.configure()` 呼叫 `libaudio.so` 的 `config_audio_pll(iic_index=1)`，預期透過 I2C 送 PLL 設定給 codec，但該函式無限 hang。
- **診斷**：
  - `i2cdetect -y 0` 與 `-y 1` 均找不到 codec（I2S 位址 0x3B）。
  - UIO device `audio-codec-ctrl` 有找到（uio_index=0）。
  - libaudio.so 的 I2C 實作（`i2cps.c`）直接存取 Zynq PS IIC 控制器 MMIO；但 ADAU1761 的 SDA/SCL 實際上接到 **PL 腳 U9/T9**（非 PS MIO），需 AXI IIC IP 在 PL 中橋接。
  - 確認依據：參考專案（Audio-Lab-PYNQ）XDC 明確標示 `IIC_1_scl_io @ U9`、`IIC_1_sda_io @ T9`，且 BD 中有 `axi_iic:2.0` IP。
- **決定**：在 `bass_fx_bd` block design 中加入 `axi_iic:2.0`，連接 PS AXI Interconnect，IIC port 接外部腳 U9/T9；XDC 加 PULLUP 與 IOSTANDARD 設定。
- **待補**：AXI IIC base address（Vivado Address Editor 重新 build 後確認）→ 填入 `docs/INTERFACE.md`。
- **影響**：需重新 Generate Bitstream；`pio_loop.py` 的 `init_audio()` 使用 `iic_index=1` 或 AXI IIC 對應 bus index（待板上確認）。

---

---

## D14. ADAU1761 I2C 初始化：AxiIIC MMIO 直接操作

- **背景**：BD 加入 `axi_iic:2.0`，XDC 接 U9/T9，硬體正確。但 overlay 載入後 `i2cdetect -l` 看不到新的 bus，`libaudio.so` 的 I2C 路徑（預期 `/dev/i2c-X`）無法使用。
- **原因（查證後修正）**：`AxiIIC` **從未依賴 device tree 或 kernel driver**。任何版本的 PYNQ 都不為 `axi_iic` 產生 DT entry；`AxiIIC` 從 2018 年加入起就是純 userspace MMIO 操作，透過 PYNQ 的 `bindto` 機制自動綁定，完全繞過 Linux i2c subsystem。`/dev/i2c-X` 不出現是預期行為，不是缺陷。
  - 真正的問題是：`libaudio.so` 的 I2C 實作（`i2cps.c`）預期透過 Linux `/dev/i2c-X` 送指令，但 ADAU1761 的 SDA/SCL 在 PL 腳（U9/T9），走的是 AXI IIC，不是 PS IIC。`libaudio` 這條路根本行不通。
  - 次要問題：Vivado 2022.2 產生的 `axi_iic:2.1`，但 PYNQ 2.5 的 `iic.py` `bindto = ['xilinx.com:ip:axi_iic:2.0']`，版本不符導致 `ol.axi_iic_0` 靜默變成 `DefaultIP`，需 `ignore_version=True`。
- **決定**：
  1. `Overlay(..., ignore_version=True)`
  2. `AudioADAU1761.configure()` 在 Overlay 載入前 monkey-patch，跳過 libaudio I2C 呼叫
  3. 新增 `init_codec_via_axiic(ol)`：用 `ol.axi_iic_0.send()` 直接操作 AXI IIC MMIO 暫存器，重現完整初始化序列
  4. 預先開啟 HP output mixer
- **技術原理**：AxiIIC 直接寫 AXI IIC IP 的 TX FIFO 暫存器（MMIO），IP 產生 I2C 波形驅動 U9/T9，完全繞過 Linux i2c subsystem。這是 PYNQ 對 AXI IIC 的標準用法，非 workaround。
- **AXI IIC base address**：`0x40800000`（已填入 `docs/INTERFACE.md`）
- **版本升級注意**：若未來升級至 PYNQ master / 3.x，`iic.py` 的 `bindto` 已改為 `axi_iic:2.1`，屆時可移除 `ignore_version=True`。
- **參考**：PYNQ `iic.py` 原始碼（https://github.com/Xilinx/PYNQ/blob/master/pynq/lib/iic.py）；PYNQ `hwh_parser.py` 原始碼確認無 axi_iic 特殊處理（https://github.com/Xilinx/PYNQ/blob/master/pynq/pl_server/hwh_parser.py）。

---

## D15. libaudio.record()/play() 導致 kernel crash：音訊搬運路線決策

- **背景**：codec 初始化後呼叫 `audio.record()`，不到 3 秒 kernel 重開機。
- **根本原因**：`libaudio.so` 是針對 PYNQ base overlay 的特定記憶體佈局編譯的。它呼叫 `mmap(/dev/uio0, audio_mmap_size)`，其中 `audio_mmap_size` 來自 HWH 的 IP 位址範圍，UIO driver 依 base image DT 宣告決定允許的最大 mmap size；自訂 BD 改變了記憶體佈局，兩者不符 → 存取超出範圍 → ARM bus error → kernel panic。本質上是 **libaudio 與自訂 BD 的記憶體佈局不相容**，不是 Effect IP 的問題。
- **為何不修 libaudio**：開發規則禁止修改系統函式庫（Rule #4）。對齊 base overlay 位址理論上可行，但需取得 base overlay source Tcl 並確認 `audio_codec_ctrl` 固定位址，工作量與 DMA 相當，且最終仍受 libaudio 本身效能限制。社群其他 PYNQ-Z2 音訊專案（如 Audio-Lab-PYNQ）亦選擇完全捨棄 libaudio 走 DMA，佐證此路線。
- **當前暫解（Python MMIO polling）**：完全繞過 libaudio，改用純 Python 直接存取 `audio_codec_ctrl` 暫存器：
  - `py_record()`：polling `I2S_STATUS_REG`，每 frame 讀 RX_L / RX_R
  - `py_play()`：polling `I2S_STATUS_REG`，每 frame 寫 TX_L / TX_R
- **已知限制**：Python loop ~20–30 μs/次，I2S frame 每 20.8 μs 一個，Python 跟不上 48 kHz → 掉 sample → 音質差（破碎）。
- **升級路線**：
  - **Fallback（時間不足）**：C PIO — 板上編譯 C 程式做同樣的 MMIO polling，C loop < 1 μs/次，可跟上 48 kHz，無需更動 HLS IP 或 BD。
  - **正式解（Phase 6，必要）**：AXI DMA + 雙緩衝 + 中斷，音訊搬運改由 DMA 硬體負責，CPU 解放，根本解決。Phase 6 已從「加分」升為必要步驟（見 D5）。
- **未影響**：HLS Effect IP、AXI-Lite 控制路徑完全不受影響，Phase 2–5 照計畫進行。
- **參考**：Audio-Lab-PYNQ（https://github.com/cramsay/Audio-Lab-PYNQ）採 AXI-DMA + smbus2 完全繞過 libaudio；PYNQ discuss 論壇 UIO device 討論（https://discuss.pynq.io/t/uio-device/1801）說明 UIO mmap 機制。

---

## D16. Distortion 參數編碼：threshold Q1.23 + gain 純整數

- **背景**：`threshold` 與 `gain` 均以 `param_t = int`（32-bit）經 AXI-Lite 傳入，IP 內部需決定如何解讀。
- **選項**：
  - A) threshold Q1.23 整數 + gain 純整數倍數
  - B) threshold 百分比整數（0–100）+ gain 純整數
  - C) 兩者皆 Q1.23（gain 上限 <1，無法放大）
- **決定**：選 A。
- **理由**：
  - threshold 用 Q1.23（`int(clip_float * (1<<23))`）與音訊 sample 型別(`ap_fixed<24,1>`) 格式一致，HLS 端轉換最自然（`ap_fixed<24,1>(ap_int<24>(threshold))`）；是 Xilinx HLS 音訊專案慣例。
  - gain 用純整數（1–20）PS 端最直觀，對應商用 pedal（Boss DS-1 ≈ 21x），無需格式轉換。
- **參考**：MicroZed Chronicles HLS Interfacing；ElectroSmash Boss DS-1 Analysis（Boss DS-1 最大增益 26.5 dB ≈ 21x）；WebSearch 查詢結果（2026-06-08）。

---

## D17. Distortion 中間運算型別：ap_fixed<32,6>

- **背景**：`ap_fixed<24,1>` × `int gain`（最大 20）乘積可達 20，超出 Q1.23 範圍，需更寬中間型別承載乘積再 clamp。
- **選項**：
  - A) `ap_fixed<32,6>`：Q6.26，範圍 [−32, +32)，gain ≤ 20 夠用，資源消耗合理
  - B) `ap_fixed<48,6>`：更多 headroom，對此場景 overkill
  - C) `ap_fixed<40,9>`：折衷
- **決定**：選 A（`ap_fixed<32,6>`）。
- **理由**：gain 最大 20，sample 最大 ≈ 1，乘積最大 20 < 32，Q6.26 的 6 整數位完全夠用；32-bit 整體寬度比 24-bit sample 寬 8 bit，有充足精度；PYNQ-Z2 48 kHz 時序壓力極小，DSP block 27-bit 限制不構成影響。
- **影響**：clamp 後必須回到 `sample_t`（ap_fixed<24,1>）；定案後回填 `docs/INTERFACE.md` 第 1 節。
- **參考**：WPI Fixed-Point Arithmetic Lecture；Xilinx HLS ap_fixed Multiplication Forum；WebSearch 查詢結果（2026-06-08）。

---

## D18. Phase 6 傳輸架構：C + DMA

- **背景**：Python PIO 音質不可接受（D15）；Phase 6 需決定升級路線。
- **選項**：
  - A) C PIO：PS C 程式輪詢 `audio_codec_ctrl`，逐 sample 透過 AXI-Lite 送 Effect IP。C loop < 1 μs/次，48kHz 完全跟得上。但沒有展示 DMA 技術，CPU 仍被佔用。
  - B) C + DMA：PS C 程式輪詢 codec 填 buffer，DMA 批次送 Effect IP（AXI-Stream），DMA 收回結果，PS 寫回 codec。CPU 解放；展示 DMA 技術。
  - C) Full hardware stream：換 I2S IP（有 AXI-Stream 輸出），codec → AXI-Stream → Effect IP → AXI-Stream → codec，PS 完全不碰音訊資料。
- **決定**：選 B（C + DMA）。
- **理由**：
  - A 可行但無法展示 DMA（專案技術亮點之一，project_plan.md 第 6.3 節）。
  - B 對既有 `audio_codec_ctrl` 零改動（已驗證可用），codec 側 C 輪詢足夠快，DMA 只負責 Effect IP 側的批次搬運。
  - C 需更換 `audio_codec_ctrl`，I2S timing 在 Phase 1 已踩坑（D12），風險過高；Vivado 2022.2 IP Catalog 中對應 IP 未經確認。
  - 板上已驗證：`pynq.allocate()` 可用（paddr=0x18049000，64-byte 對齊），`flush()`/`invalidate()` 方法存在，gcc 7.3.0 可用。
- **架構圖**：
  ```
  codec ←I2S→ audio_codec_ctrl ←MMIO→ PS C 輪詢
                                            ↕ DRAM buffer (ping-pong)
                                        AXI DMA (MM2S / S2MM)
                                            ↕ AXI-Stream
                                    [AXI Stream FIFO]（解耦時序）
                                            ↕ AXI-Stream
                                        Effect IP (HLS)
                                            ↕ AXI-Lite（參數，不動）
  ```
- **參考**：
  - PYNQ DMA 官方文件：https://pynq.readthedocs.io/en/v2.5/pynq_libraries/dma.html
  - cathalmccabe PYNQ DMA Tutorial Part 1：https://discuss.pynq.io/t/tutorial-pynq-dma-part-1-hardware-design/3133
  - Audio-Lab-PYNQ（PYNQ-Z2 音訊 DMA 參考專案）：https://github.com/cramsay/Audio-Lab-PYNQ

---

## D19. Phase 6 DMA buffer 大小：256 samples

- **背景**：buffer 越小延遲越低，但 DMA overhead 比例越高；越大則反之。
- **決定**：256 samples/buffer（每個 stereo frame，含 L+R 各 256 words，共 512 × 4 = 2048 bytes per DMA）。
- **理由**：
  - 延遲 = 256 / 48000 ≈ 5.3 ms，遠低於 100 ms 目標（project_plan.md 第 7.1 節）。
  - 音訊 DSP 社群對 48kHz 系統的推薦折衷點為 256–512 samples。
  - 2048 bytes 遠大於 DMA descriptor 最小粒度，burst 效率高。
- **可調整**：實測若出現 underrun 可增至 512（10.7 ms），仍在目標內。
- **參考**：
  - Audio DSP buffer sizing practice：https://audiodsplab.wordpress.com/ping-pong-buffer-audio-stream/
  - PYNQ Workshop Session 4 DMA：https://github.com/Xilinx/PYNQ_Workshop/blob/master/Session_4/6_dma_tutorial.ipynb

---

## D20. Phase 6 跨 buffer 狀態：HLS static 變數

- **背景**：IIR 前一輸出（wobble）和 LFO 相位（wobble）需要跨 DMA buffer 保留。Phase 6 前這些在 `state_t` 裡由 AXI-Lite 從 PS 傳入，DMA 模式下每 buffer 讀寫一次 AXI-Lite 的成本可接受，但 HLS static 更乾淨。
- **選項**：
  - A) HLS `static state_t state = {0}`：在 HLS top function 內宣告，自動跨 call 保留，合成成 register。
  - B) PS 每次 buffer 前後透過 AXI-Lite 讀回 / 寫入 state。
- **決定**：選 A（static）。
- **理由**：static 合成成暫存器（非 BRAM），HLS 已知支援此模式，且 IIR/LFO 狀態本就屬於 IP 內部狀態，不需 PS 介入。Phase 6 後 AXI-Lite `state` port 可移除。
- **注意**：static 在 HLS 模擬中初始化一次；RTL 中 reset 行為需確認（通常 ap_rst 時清零）。
- **參考**：
  - Hackaday IIR Audio Processing with static state：https://hackaday.io/project/166515-audio-processing-with-the-snickerdoodle/details
  - Xilinx HLS UG1399（static variable synthesis）：https://docs.amd.com/r/en-US/ug1399-vitis-hls/How-AXI4-Stream-is-Implemented

---

## D21. Phase 6 兩個已知關鍵陷阱

- **陷阱 1：TLAST 未設 → DMA recvchannel.wait() 永遠 hang**
  - 根本原因：AXI DMA S2MM channel 等待 TLAST 訊號才認定 packet 結束；HLS IP 若沒在最後一個 sample 設 `pkt.last = 1`，DMA 永遠不知道 packet 結束，程式卡死。
  - 解法：HLS stream loop 在 `i == n_samples - 1` 時設 `pkt.last = 1`，其餘設 0。
  - 來源：Element14 Vitis HLS + DMA Training Part 2：https://community.element14.com/technologies/fpga-group/b/blog/posts/pynq-and-zynq-the-vitis-hls-accelerator-with-dma-training---part-2-add-the-accelerated-ip-to-a-vivado-design

- **陷阱 2：HP port 無 cache coherency → 資料讀寫錯亂**
  - 根本原因：Zynq-7000 HP port 不自動同步 CPU cache，PS 寫入 buffer 後 PL 可能讀到 stale cache，PL 寫完 PS 可能讀到 stale cache。
  - 解法：
    - PS 寫完 input buffer 後、DMA 讀之前：`in_buf.flush()`
    - DMA 寫完 output buffer 後、PS 讀之前：`out_buf.invalidate()`
  - `pynq.allocate()` 回傳的 `PynqBuffer` 物件有這兩個方法，直接呼叫。
  - 來源：PYNQ discuss cache coherency：https://discuss.pynq.io/t/cache-coherency-in-pynq-image-using-pynq-z2-board-ethernet-bring-up/4078

---

## D22. Phase 6 PS 端 CMA buffer 分配與 cache 管理：`libcma.so` ✅

- **背景**：C + DMA 架構下，PS C 程式需要（1）分配 DMA-safe（physically contiguous）buffer，（2）在 DMA 傳輸前後做 cache flush / invalidate（HP port 無 hardware coherency，見 D21）。Python 透過 `pynq.allocate()` 解決，但 C 程式不能直接呼叫 Python API。
- **決定**：使用板上已有的 **`libcma.so`**（xlnk 的 C userspace wrapper）。
- **調查結論**：
  - **XRT**：**不支援 PYNQ-Z2（Zynq-7000）**。XRT 的 Zynq 支援指 ZynqMP（UltraScale+, Cortex-A53），與 Zynq-7000（Cortex-A9）無關。官方 maintainer Cathal McCabe 明確確認（https://discuss.pynq.io/t/xrt-on-pynq-z2/2950）。「xlnk 被 XRT 取代」在 Z2 上**不適用**。
  - **xlnk / libcma.so**：xlnk kernel driver 在 PYNQ 2.5 仍然存在且可用（Python 層面 deprecated，但 kernel driver 未移除）。Xilinx 提供 `libcma.so` 作為 C userspace wrapper，**已預裝於 PYNQ 2.5**（`/usr/lib/libcma.so`、`/usr/include/libxlnk_cma.h`）。在 Cortex-A9 上，`cma_flush_cache` / `cma_invalidate_cache` 底層呼叫 xlnk kernel driver 的 `xlnkFlushCache` / `xlnkInvalidateCache`，提供完整 cache coherency。PYNQ 2.5 + PYNQ-Z2 上社群確認可用（需 sudo）。
  - **udmabuf**：需 cross-compile（板上缺 `modpost` binary，`/bin/sh: scripts/mod/modpost: not found`），沒有 `4.19.0-xilinx-v2019.1` 的預編譯 `.ko`，且 `libcma.so` 已提供等效功能，**不值得繼續追**。
  - **ACP port**：hardware coherent，不需 software cache ops，但需 Vivado BD 額外設定 AxCACHE（接 Constant IP = `0b0011`），改動量較大，保留為備選。
- **C API**（編譯：`gcc ... -I/usr/include -L/usr/lib -lcma -lpthread`，需 sudo）：
  ```c
  #include <libxlnk_cma.h>
  void     *buf  = cma_alloc(2048, 1);           // 分配 CMA buffer（cacheable）
  uint32_t  phys = cma_get_phy_addr(buf);        // 取實體位址（給 DMA 暫存器用）
  cma_flush_cache(buf, phys, 2048);              // DMA TX 前：PS→PL flush
  cma_invalidate_cache(buf, phys, 2048);         // DMA RX 後：PL→PS invalidate
  cma_free(buf);
  ```
- **待確認（板上）**：`ls -la /usr/lib/libcma.so /usr/include/libxlnk_cma.h`
- **參考**：
  - libxlnk_cma.h：https://github.com/Xilinx/PYNQ/blob/master/sdbuild/packages/libsds/libcma/libxlnk_cma.h
  - pynqlib.c（flush/invalidate 實作）：https://github.com/Xilinx/PYNQ/blob/master/sdbuild/packages/libsds/libcma/pynqlib.c
  - XRT on PYNQ-Z2（maintainer 確認不支援）：https://discuss.pynq.io/t/xrt-on-pynq-z2/2950
  - libcma.so in C++ on PYNQ-Z2（社群確認）：https://discuss.pynq.io/t/a-problem-on-libcma-so-in-c/959
  - udmabuf modpost 問題：https://github.com/ikwzm/udmabuf/issues/19

---

## 待補決策(後續 Phase 產生)

- `process_sample()` 跨 sample 狀態結構完整定案（Phase 3，wobble 實作後）。
- low/high 參數的實際數值(Phase 4,調出好聽範圍)。
- wobble 中間運算型別寬度（Phase 3）。
- wobble 濾波器係數查表的頻率範圍與量化精度(Phase 3)。
- Phase 6 DMA base address（Vivado Address Editor 重新 build 後確認）。
- Phase 6 HLS 合成後 AXI-Lite parameter offsets 是否有變（重新 synthesis 後確認 `xprocess_sample_hw.h`）。
