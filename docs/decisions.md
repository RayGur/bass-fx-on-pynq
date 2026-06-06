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

## D5. 資料路徑:PIO 先行,DMA 為加分(A→B 分層)

- **背景**:音訊樣本需在 PS 與 PL 之間搬運。參考專案用 per-sample PIO(CPU 全程被綁、延遲高)。
- **選項**:(A) per-sample PIO;(B) AXI DMA + 雙緩衝 + 中斷。
- **決定**:MVP 用 A 保底,跑通後升級 B 為加分。
- **理由**:
  - A 最簡單、確保一定有可 demo 成果;B 延遲低、CPU 解放,且「識別 PIO 瓶頸並改 DMA」是漂亮的工程敘事。
  - 延遲容忍度經確認可放寬(目標 <100 ms,遠寬於演奏舒適區),故 A 的高延遲不擋 demo。
  - 升級成本可控,前提是運算核心解耦(見 D6)。
- **A→B 成本估**:運算核心 0 改動;IP 介面小改(改 AXI-Stream);block design 中改(加 DMA + 中斷);PS 端大改(DMA 設定 + 中斷 + 雙緩衝)。約 4–6 工作天。
- **參考**:參考專案的 PIO 實作;Zynq DMA / 中斷標準範式;團隊既有 DMA(MM2S/S2MM)與 IRQ/GIC 經驗。

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

## 待補決策(後續 Phase 產生)

- AXI IIC bus index（確認 AXI IIC 在 /dev/i2c-X 的編號，Phase 1 修復後）。
- 中間運算型別寬度(Phase 2/3)。
- `process_sample()` 跨 sample 狀態結構(Phase 2/3)。
- low/high 參數的實際數值(Phase 4,調出好聽範圍)。
- wobble 濾波器係數查表的頻率範圍與量化精度(Phase 3)。
