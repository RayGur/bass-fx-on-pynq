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

## D23. Phase 6 除錯：TKEEP=0 → S2MM 不寫 DDR

- **背景**：Phase 6 DMA 接線完成後，S2MM 呈現「IOC 正常 fire、S2MM_SR LEN=0、無 AXI 錯誤，但 `out_buf` 內容永遠是初始化哨兵值（0x12345678）」。透過 `/dev/mem` 直讀實體位址確認是 DDR 本身未被寫入，排除 cache 問題。
- **排除過的根因**：
  - T1 cache（`cma_alloc(size, 0)` non-cacheable；`/dev/mem` 直讀仍是哨兵）
  - T2 實體位址錯誤（雙向驗證通過）
  - T3 HP0 寬度不匹配（從 64-bit 改 32-bit，結果相同）
  - T4 BD 連線遺漏（重新 export TCL 確認 `axis_data_fifo_1` 連線正確）
  - T5 AXI-Lite offset 錯誤（對照 `xprocess_sample_hw.h` 確認全部正確）
  - T6 Effect IP 未運行（若 `s_out` 空，S2MM 會 hang 而非完成；IOC 能 fire 代表 IP 有輸出）
- **根本原因**：`process_sample.cpp` 的輸出 packet 建構：
  ```cpp
  audio_pkt_t out_l_pkt;
  out_l_pkt.data = 0;
  out_l_pkt.data.range(23, 0) = out_l.range(23, 0);
  // out_l_pkt.keep 未設 → HLS 合成後為 0
  out_l_pkt.last = 0;
  ```
  `ap_axis<32,0,0,0>` 的 `.keep`（4-bit）若不顯式設值，Vitis HLS 合成後在輸出 TKEEP 線預設驅動 0。AXI DMA 內部的 DataMover IP 將 TKEEP 映射到 AXI4 WSTRB；WSTRB=0 代表所有 byte enable 無效 → Zynq HP0 接受交易（BRESP=OKAY，S2MM 認為成功，IOC fire），但不寫入 DDR 任何位元組。
- **決定**：在每個輸出 packet 建構後立即設 `out_l_pkt.keep = ~0`（`out_r_pkt.keep = ~0`），確保 TKEEP=0xF（32-bit stream 的 4 個 byte lane 全有效）。
- **修正位置**：`hls/effect_ip/process_sample.cpp`，`process_sample()` top function 的 stream loop 內。
- **注意**：此 bug 無法被以下機制偵測：
  - HLS C Sim（sim 直接讀 `.data`，不走 DMA 硬體）
  - `validate_bd_design`（只驗連線，不驗 HLS 內部）
  - DMA 錯誤旗標（S2MM_SR 無 error bits，AXI 有正常 BRESP）
- **影響**：需重新 HLS 合成 → Vivado Refresh IP → Generate Bitstream → 重新上板。
- **參考**：
  - Xilinx AXI DataMover PG022：TKEEP → WSTRB 映射說明
  - UG1399 ap_axis 說明：.keep 欄位預設行為

---

## D24. Phase 6 除錯：HLS loop II=2 → S2MM 跳寫，R channel 不寫 DDR

- **背景**：D23 修復 TKEEP 後，`out_buf` 開始被寫入，但呈現**完美交錯 pattern**：偶數 index（L samples）寫入正確，奇數 index（R samples）永遠保持哨兵值。左耳可聽到 passthrough，右耳爆音。
- **排除過的根因**：
  - T1 DMA/FIFO/HP0 data width（hwh 確認全部 32-bit）
  - T2 TSTRB=0（加 `.strb = ~0` 後重建 bitstream，pattern 不變）
  - T3 AXI Interconnect 64-bit upsize（XBAR_DATA_WIDTH=32，排除）
  - T4 cache（`/dev/mem` 直讀仍為哨兵，確認 DDR 本身未寫入）
- **根本原因**：`hls::stream` 是**單端口 FIFO**，每個 clock 最多一次 read 或一次 write。原 loop body：
  ```cpp
  for (int i = 0; i < n_samples; i++) {
  #pragma HLS PIPELINE II=1
      s_in.read();   // read 1
      s_in.read();   // read 2  ← 同一 FIFO 衝突
      ...
      s_out.write(out_l_pkt);  // write 1
      s_out.write(out_r_pkt);  // write 2  ← 同一 FIFO 衝突
  }
  ```
  HLS 強制 II=2（csynth.rpt `Issue Type: II, Interval: 2`）。AXI DMA S2MM store-and-forward 模式下，II=2 的輸出讓每隔一個 valid slot 才對應到 R write，R samples 的時序不被 S2MM 接收，造成 R 全數不寫 DDR。  
  （已驗：Xilinx UG1448 定義此為 "interface contention (intra-iteration)"；UG1399 確認 hls::stream 單端口限制。）
- **決定**：loop 改為 `n_samples * 2` 次迭代，每次 1 read + 1 write，L/R 以 `i % 2` 區分：
  ```cpp
  for (int i = 0; i < n_samples * 2; i++) {
  #pragma HLS PIPELINE II=1   // 現在可達 II=1
      audio_pkt_t pkt = s_in.read();
      // process one sample...
      s_out.write(out_pkt);
  }
  ```
  `n_samples` 語義不變（stereo pairs），PS 仍寫 256，DMA buffer size 不變（2048 bytes）。
- **修正位置**：`hls/effect_ip/process_sample.cpp`，`process_sample()` top function。
- **Phase 3 連動**：`apply_wobble` 需加 `bool is_l` 參數（LFO phase 只在 L 推進）；`state_t.iir_prev` 需分成 `iir_prev_l` / `iir_prev_r`。見 `docs/phase3.md §11`。
- **注意**：此 bug 無法被 HLS C Sim 偵測（sim 不走 DMA 硬體）。
- **影響**：需重新 HLS 合成 → Vivado Refresh IP → Generate Bitstream → 重新上板。

---

## D25. Phase 6 除錯：HP0 data width 32-bit → 交錯寫入 pattern（S2MM 每隔一個 word 不寫 DDR）

- **背景**：D24 修復 per-sample loop（II=1）後，`dma_test.py` 獨立測試（N=256，in_buf=i×100，sentinel=0xAAAAAAAA）仍然出現：
  ```
  out_buf[:8] = [0, 0xAAAAAAAA, 200, 0xAAAAAAAA, 400, ...]
  total written=256  sentinel=256  (expected written=512)
  ```
  偶數 index 寫入正確（且值等於 `in_buf[even]`，不是 `in_buf[odd]`），奇數 index 永遠是 sentinel。
- **關鍵觀察**：
  - written 的值是 `in_buf[0], in_buf[2], in_buf[4]`...（**8-byte stride**，不是 4-byte），代表 S2MM 每次以 64-bit beat 寫入，但只有低 32-bit 有 WSTRB，高 32-bit 全跳過。
  - MM2S 側亦相同：從 DDR 讀進 HLS 的是 `in_buf[0], in_buf[2]...`，所以輸出也是每隔一個值。
  - 用 Python `dma_test.py`（非 `audio_dma.c`）獨立驗證，排除 PS C 程式問題。
- **排除的根因**：
  - HLS RTL 已讀過：`TKEEP` 硬接線 `4'd15`，`TVALID` 對全部 510 次 iteration 皆 fire。RTL 清潔，無 HLS 層問題。
  - D23（TKEEP=0）：已修正並驗證（synthesis report 確認 TKEEP hardwired）。
  - D24（II=2）：已修正，Final II=1 在 csynth.rpt 確認；但硬體仍失敗，排除為充分條件。
  - cache 問題：non-cacheable CMA 和 explicit invalidate 兩種方式皆出現相同 pattern。
  - FIFO 溢出：N=255 < FIFO_DEPTH=512，同樣 pattern。
  - AXI-Lite 位址或參數設定：各項 readback 驗證正確。
- **根本原因**：Zynq-7000 的 S_AXI_HP port 內部匯流排**實際上是 64-bit**，即使 `PCW_S_AXI_HP0_DATA_WIDTH=32` 的參數也不改變底層硬體寬度。  
  當 HWH/PS 設 32-bit 時，AXI Interconnect 與 HP0 之間的接口以 64-bit 運作，每個 64-bit beat 只有低 32-bit 有效 WSTRB（=0x0F），高 32-bit WSTRB=0x0（不寫 DDR）。S2MM 每 64-bit beat 只寫一個 32-bit word，造成 8-byte stride、每隔一個 slot 空白的 pattern。MM2S 讀取亦相同（每次讀取 64-bit 但只傳 32-bit 給 DMA，跳過另一個 word）。
- **決定**：在 Vivado PS7 Customization 將 `S AXI HP0 Interface Data Width` 從 32 改為 **64**，重新 Validate Design + Generate Bitstream。
- **修改位置**：Vivado Block Design → double-click `processing_system7_0` → PS-PL Configuration → HP Slave AXI Interface → S AXI HP0 Interface → Data Width = 64 → Validate（F6）→ Generate Bitstream。
- **驗證**（`dma_test.py` 重跑）：
  ```
  out_buf[:8] = [0, 100, 200, 300, 400, 500, 600, 700]
  total written=512  sentinel=0  (expected written=512)
  ```
  全部 512 個 word 正確寫入。
- **附注 1**：`LEN_rem=2048`（S2MM 完成後 LENGTH 暫存器讀回 2048）是 Xilinx AXI DMA direct-register mode 的正常行為；LEN 暫存器讀回 programmed value，不倒數，可忽略。
- **附注 2**：先前已有 `bass_fx_bd_hp64.bit`（舊版 build，未含 D24 fix），測試時顯示 `total written=0`。懷疑是舊 BD 有其他 interconnect 配置問題。**正確做法是從乾淨的 bass_fx_bd BD 出發，在 PS7 改 HP0 width 後重新 Generate Bitstream**，而非直接使用舊 hp64 檔案。
- **影響範圍**：DMA S2MM + MM2S 兩條路徑皆受惠；`audio_dma.c` 和 `dma_test.py` 不需改動。
- **參考**：Zynq-7000 TRM（UG585）§11.2 AXI Slave Port（HP）說明；Xilinx forum 討論 HP port 實際寬度 vs. PCW 參數的差異。

---

## D26. 板上實測：wobble 效果深度不足（Post-MVP 優化待辦）

- **背景**：Phase 3 wobble 整合進 Phase 6 DMA 架構後（2026-06-14），接 ADAU1761 codec + bass 實測，wobble 掃動效果太細微，與 distortion 串接時尤為明顯。
- **根本原因**：
  1. **一階 IIR（6 dB/oct）rolloff 太緩**：低通濾波器頻率響應不夠陡，截止頻率前後音量差異不明顯，bass 低頻基音（40–400 Hz）幾乎不受影響。
  2. **B_LUT 掃動範圍**（b ≈ 0.026–0.704，約 200 Hz–10 kHz）集中在中高頻，對 bass 基音效果有限。
  3. **與 distortion 串接時**：distortion 產生的諧波被 wobble 低通削減，但 wobble 本身對基音的調變已很弱，整體效果不明顯。
- **後續優化選項（Post-MVP）**：
  - A）升 2nd-order IIR（12 dB/oct）：效果更明顯，但需增加 HLS 資源（loop-carried state 距離加深，需確認 II 仍可達 1）。
  - B）調整 B_LUT 範圍：將掃動下緣推低（如 20–2000 Hz），使 bass 基音也進入掃動區。
  - C）加諧振（Q factor）：在截止頻率附近加 boost，wah 感更強，但設計複雜度大增。
  - **建議先試 B**：修改 B_LUT 成本最低（純軟體），無需改 HLS 架構，不影響 II。
- **影響範圍**：`hls/effect_ip/wobble.cpp`（B_LUT 調整）或 `process_sample.cpp`（2nd-order state 擴充）。
- **處置（2026-06-15，14.1 fix）**：同時執行 A + B，理由如下：
  - 實測確認問題本質是「震幅不足」而非頻率偏移：低頻確有波動，但開合幅度太小。
  - 根因一（B_LUT 低端 fc≈200 Hz 高於 bass 基音 41–98 Hz）為主因：當 LFO 在最低值時，bass 基音仍在 passband，未被衰減，所以「有波動但不夠深」。
  - 同時採用 2nd-order cascade 增加斜率，讓開合幅度更明顯（36 dB vs 18 dB）。
  - **B_LUT 新範圍**：10–2000 Hz 對數等比（公式 `b = 1 - exp(-2π·fc/48000)`），16 點。
  - **state_t 擴充**：新增 `iir_prev2_L` / `iir_prev2_R`（純 HLS 內部 static，不影響 AXI-Lite 介面合約）。
  - **修改檔案**：`wobble.cpp`（B_LUT + 2nd-order IIR）、`effect_ip.h`（state_t）、`process_sample.cpp`（初始化）、`tb_process_sample.cpp`（testbench 初始化）。
  - **後續追加（同日）**：實測波谷無聲 → 新增 `lfo_floor` AXI-Lite 參數（見 D28）；btn2 循環三種 wah depth preset。
  - **驗證（2026-06-15）**：HLS C-sim PASS、synthesis II=1（lfo_floor offset 0x48 確認）、Vivado rebuild PASS、板上三個 preset（A/B/C）音訊驗聽全 PASS。

---

## D28. wah depth preset runtime 切換：lfo_floor 參數設計（2026-06-15）

- **背景**：14.1 fix 後 B_LUT 低端 fc=10 Hz，2nd-order 在波谷時把 bass 基音壓到 ~−36 dB（近無聲）。實測偏好保留波谷聲音，需能在不同深度間切換。
- **選項比較**：
  - A）多張 B_LUT + `wah_mode` 選擇器：每個 preset 有獨立 LUT，切換乾淨，但 HLS 資源用多一倍（16×N 個 ROM entry），且需再加一個 AXI-Lite 參數。
  - B）`lfo_floor` 參數（最小 LUT index）：保留現有單張 10–2000 Hz 寬範圍 B_LUT，靠 `lut_idx = max(lfo_floor, computed_idx)` 抬高波谷；只需一個新參數，HLS 資源不增。
- **決定**：選 B（`lfo_floor`）。
- **理由**：HLS 資源省一半；B_LUT 保留完整範圍方便未來調整；PS 端可用任意整數（0–15）設定波谷，不限三段。
- **Preset 對應**（btn2 循環 A→B→C）：
  - A（default）：floor=6，fc≈83 Hz，波谷 −6 dB，bass 可聽
  - B：floor=4，fc≈41 Hz，波谷 −18 dB，很暗
  - C：floor=0，fc=10 Hz，波谷 −36 dB，近無聲
- **AXI-Lite offset**：0x48（HLS 按參數順序分配，synthesis 後由 `csynth.rpt` 確認）。
- **影響範圍**：`wobble.cpp`（floor clamping）、`effect_ip.h`（所有簽章）、`process_sample.cpp`（pragma + 傳遞）、`tb_process_sample.cpp`（call sites）、`ps/audio_dma.c`（offset + preset + btn2）、`docs/INTERFACE.md`（AXI-Lite 表、btn[2]）。

---

## D27. 板上實測：distortion 高 gain 雜訊放大（Post-MVP 優化待辦）

- **背景**：Phase 6 DMA 架構接 codec 實測（2026-06-14），distortion gain 調高時，底噪（noise floor）被明顯放大，與 passthrough 和 wobble 模式相比差異顯著。
- **根本原因**：hard clipping 在 clip 之前對全頻訊號（含底噪）先做 `gain` 倍放大。底噪從原本不可聞（codec ADC 量化噪 ≈ −144 dBFS @ 24-bit）經 ×8–20 放大後提升 18–26 dB，進入可聞範圍。被動 bass 直插 line-in 的阻抗不匹配（D1 §5.3）也使底噪比例偏高，加劇問題。
- **後續優化選項（Post-MVP）**：
  - A）**加 noise gate**（建議首選）：在 gain 放大前判斷 `|in| < noise_threshold`，若成立則輸出 0（靜音）。noise_threshold 可設為獨立的 AXI-Lite 參數，讓 PS 動態調整。實作成本低，HLS 一個 `if` 即可，不影響 II。
  - B）**soft knee clipping**：把硬切換成平滑過渡，減少截波產生的高諧波，但對底噪本身無幫助。
  - C）主動 DI（硬體）：改善阻抗匹配，降低底噪源頭，但這是 demo 佈置問題，非 IP 問題。
  - **建議先試 A**：noise gate 實作最簡單，直接在 `apply_distortion()` 加判斷，不需改介面合約，可搭配 B 使用。
- **影響範圍**：`hls/effect_ip/distortion.cpp`、`effect_ip.h`（若新增 noise_threshold 參數則需更新 AXI-Lite 位址表並通知 Claire）。

---

## D32. 60 Hz Notch r 值選 0.9997（2026-06-15 初選 0.9999，2026-06-16 改 0.9997）

- **背景**：板上實測接地線只略改善 hum，數位方案需精準打 60 Hz。選 r 值需在 notch 帶寬與 bass 音色之間取平衡。
- **初版決定（2026-06-15）**：r = 0.9999，帶寬（−3 dB）≈ 1.5 Hz（= (1−r) × 48000 / π）。理由：r=0.999 → BW 15 Hz → −3 dB 點落 52.4 Hz，A 弦（55 Hz）被衰減 ~3 dB（不可接受）；r=0.9999 → −3 dB 點 58.5–61.5 Hz，55 Hz 幾乎無影響。
- **修正（2026-06-16）**：板上實測 pick attack 後約 0.2 s 出現滋擦聲，確認為 notch 高 Q 暫態 ringing（τ = 1/(1−r) = 10,000 samples ≈ 208 ms）被 distortion 放大所致。降 r 至 0.9997 → τ = 3,333 samples ≈ 69 ms（縮短 3×），ringing 明顯減輕。
- **最終決定**：r = 0.9997，BW ≈ 4.6 Hz，−3 dB 點 57.7–62.3 Hz。A 弦（55 Hz）衰減 ≈ 0.82 dB（<1 dB，distortion 情境下幾乎不可感知）。60 Hz 精確零點與 r 無關，hum 消除效果不受影響。
- **影響**：`hls/effect_ip/notch.cpp` — NOTCH_A1/A2 係數（B1 不變）。

---

## D33. Notch always-on vs conditional（2026-06-15）

- **背景**：HPF 在 bypass 時若 always-on，第一個 sample output ≠ input，破壞 passthrough test 的精確等值比較（out == in）。Notch 是否有同樣問題？
- **決定**：Notch **always-on**（無條件啟動）；HPF **conditional on dist_en || wobble_en**。
- **理由**：Direct Form I biquad 在所有 state 為 0 的 fresh state 下，第一個 sample：`y[0] = x[0] + b1×0 + 0 − a1×0 − a2×0 = x[0]`，即輸出等於輸入。passthrough test 只呼叫一次（fresh state），因此 notch 不影響 passthrough 結果。HPF 則是 `y[0] = alpha × x[0] ≠ x[0]`，需要條件限制。
- **影響**：`process_sample.cpp` — notch 在 loop 最前端無條件呼叫；HPF 包在 `if (dist_en || wobble_en)` 內。

---

## D34. HPF conditional on dist_en||wobble_en（2026-06-15）

- **背景**：HPF 的 first-sample output = alpha × x ≠ x（alpha ≈ 0.9963），若 always-on 會使 bypass 路徑出現微小衰減，破壞 testbench passthrough test。
- **決定**：HPF 僅在 `dist_en || wobble_en` 時啟動。
- **語意優勢**：true bypass（兩個效果皆關）確實等於輸入，符合使用者預期的「乾聲監聽」。
- **影響**：`process_sample.cpp` `hpf.cpp`。

---

## D35. Noise gate 0.001 hardcode（2026-06-15）

- **背景**：板上接地線略改善後，hum 仍在 ≈ −60 dBFS 量級；distortion gain 放大時靜音期 hum 變得明顯（14.2）。是否需要 AXI-Lite 動態調整？
- **決定**：hardcode 0.001（= −60 dBFS，Q1.23 = 8389/2^23）。
- **理由**：新增 AXI-Lite 參數需更新 INTERFACE.md、通知 Claire、PS 端加 preset 值，成本超過效益。板上 hum 量級穩定在此範圍，固定值直接有效。若日後需要調整，屆時再升 AXI-Lite 參數（影響範圍小）。
- **影響**：`hls/effect_ip/distortion.cpp`。

---

## D36. Noise gate hysteresis：open/close 雙 threshold（2026-06-16）

- **背景**：14.2 加入 hard gate（單一 threshold 0.001）後，板上實測 sustain 音符衰減時音色「裂開」（滋渣、越來越碎）。
- **根因**：弦振動振幅衰減到 0.001 附近時，正弦波峰值 > 0.001（gate 開）而零點附近 < 0.001（gate 關），每週期開關兩次，把波形切成碎片（gate chatter），產生大量諧波。
- **決定**：改為 hysteresis gate，open threshold = 0.001（同前），close threshold = 0.0003（≈ −70 dBFS）。gate 開啟後需振幅降到 0.0003 才關閉，音符可自然衰減通過原 0.001 門檻而不被切斷。gate 狀態存於 `state_t.dist_gate_open_L/R`（bool per channel）。
- **影響**：`distortion.cpp`（logic）、`effect_ip.h`（簽章 + state_t）、`process_sample.cpp`（呼叫帶入 state/is_l）、`tb_process_sample.cpp`（state init 17→19 欄）。
- **板上驗聽結果（2026-06-16）**：gate chatter 情況與前版相近，改善不明顯；問題仍存在，可繼續優化（如更長的 hold-off timer、envelope follower、或提高 close threshold 至 0.0005）。

---

## 待補決策(後續 Phase 產生)
