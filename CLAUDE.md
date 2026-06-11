# CLAUDE.md

PYNQ-Z2 上的即時 bass 數位效果器。效果運算(distortion / wobble)以 HLS 合成跑在 PL(FPGA);PS(ARM)負責 codec 設定、參數控制與音訊搬運。詳見 `docs/project_plan.md`。

---

## 回答品質要求

- **不確定時明說**：技術細節若不確定，直接說「不確定」或「需要查證」，不要猜測後裝作確定。
- **主動查資料**：對可以用 WebSearch 驗證的問題，查完再回答，不要只靠訓練資料。
- **引用來源**：查到的外部資訊附 URL；引用專案內文件時標明檔名與段落。

---

## Claude Code 的角色與邊界

**Claude Code 只負責程式碼層**,以下由 Ray 手動執行,Claude Code 不碰:

- ❌ 不執行 Vivado / Vitis HLS(build、合成、bitstream 生成由 Ray 手動跑 GUI/tcl)
- ❌ 不做實機音訊測試

**Claude Code 可以**:

- ✅ SSH 上板執行指令(`ssh xilinx@192.168.2.99`)

**Claude Code 負責**:寫/改 HLS C source、PS 端 C / Python、HLS testbench、文件;必要時 SSH 上板執行與驗證。

---

## 硬體環境

| 項目 | 內容 |
|------|------|
| 板子 | PYNQ-Z2 ×2(Ray、Claire 各一張,學校提供) |
| 晶片 | Zynq XC7Z020(Cortex-A9, 32-bit) |
| 整合 golden 板 | Ray 的板子 |
| 軟體棧 | 標準 PYNQ image(需有 `pAudio` / base overlay) |
| codec | ADAU1761(48 kHz, 24-bit pad 32, I2S, I2C 設定) |

- **Ray 板 SSH**:`192.168.2.99`(user: xilinx)
  - 註:已確認為標準 PYNQ 2.5 image,**無需重刷**。
- **Claire 板 SSH**:`192.168.2.99`(user: xilinx)
- **PYNQ image 版本**:`2.5 (Glasgow)`,基於 Ubuntu 18.04
- **同步方式**:本機寫檔 → scp 同步到板子 → build / 測試在 Ray 端手動執行

## 工具版本(凍結)

| 工具 | 版本 |
|------|------|
| Vivado | 2022.2 |
| Vitis HLS | 2022.2(須與 Vivado 同版) |

> 兩塊板子與兩人開發機的工具版本必須一致(FPGA 專案對版本極度敏感)。

## 本機路徑

- Ray 本機:`EE\bass-fx-on-pynq`
- 板上路徑:`<TODO>`

---

## 開發規則

1. **每個模組獨立可測**,未驗證前不整合進上層。符合分層驗證。
2. **`process_sample()` 必須與外殼解耦**:運算核心不可寫入任何 AXI / mmap / stream 細節。A(PIO)與 B(DMA)共用同一份運算核心,這是 A→B 低成本升級的前提。
3. **改介面前先更新 `docs/INTERFACE.md`**:`process_sample()` 簽章、AXI-Lite 位址表、AXI GPIO bit 對應為凍結合約;任何變動先改文件並通知對方。
4. **不修改 PYNQ 系統 `libaudio.so`**:於自有 userspace / notebook 操作,不動系統函式庫。
5. **HLS 禁用 `#pragma HLS DATAFLOW` 與非必要的 `UNROLL`**:已知資源過用主因。避免衝突 pragma(如 UNROLL factor 與 PIPELINE II=1 並用)。
6. **每一級運算後做 clamp 防溢位**:單效果與串接皆需,呼應 distortion/wobble 的 clipping。
7. **定點數型別統一**:依 `docs/INTERFACE.md` 約定的 Q 格式,不隨意混用寬度。
8. **每個 Step 開始前先說實作計畫,確認後再動手。**
9. **遇到設計決策列出選項問 Ray,不要自己決定。**
10. **每個 Phase 開始前主動列出前提條件**(工具鏈、image、sysfs 路徑、權限、上一 Phase 是否通過 Exit Criteria);未確認項整理成 checklist 交 Ray 去板子上跑。
11. **未通過 Exit Criteria 不進下一 Phase**(見 `docs/project_plan.md` 第 9 節)。
12. **token 快用完時主動提醒**,先更新本檔「目前進度」與 `docs/decisions.md`。

---

## 版本控制

- Repo:https://github.com/RayGur/bass-fx-on-pynq.git
- 功能開新 branch,完成一小塊就 commit。
- merge 開 PR,push 前告訴 Ray。
- **進 Git**:HLS C source、tcl、PS 程式、docs。
- **不進 Git**:`.bit` / `.hwh` 等 binary(另行共享;`.bit` 與 `.hwh` 須同名成對)。

---

## 目前進度

- ✅ **Phase 0**:環境與硬體 sanity check — 完成
  - ✅ PYNQ 2.5 image 確認(無需重刷)
  - ✅ base overlay 載入正常
  - ✅ line-in 收訊正常(touch 測試)
  - ✅ HP-out 輸出正常(bypass + EarPods 有聲,需 `select_line_in()`)
  - ✅ stereo 兩聲道確認
  - ✅ JB62 實機接線驗證完成
- ✅ **Phase 1**:最小 IP(passthrough)跑通自訂路徑 — **完成**
  - ✅ HLS source、C Sim、synthesis、export IP
  - ✅ Vivado BD（含 clk_oddr、axi_iic_0）、Bitstream 生成
  - ✅ Effect IP sanity check PASS（`run_effect(0.5, -0.5)` 正確）
  - ✅ Codec 初始化：`init_codec_via_axiic(ol)` 繞過 libaudio I2C，用 `ol.axi_iic_0`（AxiIIC）直接設定 ADAU1761
  - ✅ 音訊 record/play：`py_record` / `py_play` 用 MMIO 直讀 audio_codec_ctrl，繞過 libaudio UIO（避免 kernel crash）
  - ✅ **Phase 1 Exit Criteria PASS**：聲音穿透自訂 Effect IP（batch mode，音質差為 PIO 限制，非 bug）
  - 技術債：`docs/decisions.md` 待補 D14（AXI IIC PYNQ DT 問題與 AxiIIC 繞法）
- ✅ **Phase 2**:distortion(hard clipping + AXI-Lite threshold/gain) — **完成**
  - ✅ `distortion.cpp` 實作（hard clipping，`ap_fixed<32,6>` 中間型別，threshold Q1.23 raw bit decode）
  - ✅ `process_sample.cpp` 啟用 distortion block
  - ✅ `tb_process_sample.cpp` Phase 2 測試（13 cases）
  - ✅ C Simulation PASS（全 13 case）
  - ✅ RTL Synthesis PASS（DSP×2，LUT 1%，timing 6.57 ns < 10 ns）
  - ✅ Export IP → Vivado Refresh IP → Generate Bitstream
  - ✅ 上板 AXI-Lite 控制驗證 PASS（6/6 sanity check）
  - ✅ **Phase 2 Exit Criteria PASS**：threshold=0.1, gain=20 失真明顯可辨（batch mode，音質差為 Python PIO 掉 frame，非 distortion bug）
  - **技術債**：threshold decode 須用 `.range()` raw bit assign（值轉換會 overflow），已記錄於 `docs/phase2.md`
- 🔲 Phase 3:wobble(一階 IIR + LFO 掃頻 + AXI-Lite lfo_rate/lfo_depth)
- 🔲 Phase 4:按鈕單選切換 + AXI-Lite 調參 → **MVP 完成**
- 🔲 Phase 5:效果串接(2 switch 同開,需 P2/P3 各自通過)
- 🔄 **Phase 6**:A→B 升級(C + DMA + 雙緩衝 + 中斷)— **進行中**（branch: `phase6/dma-upgrade`）
  - 架構方向定案：C + DMA（見 D18–D21，docs/phase6.md）
  - 已確認板上：`pynq.allocate()` 可用、flush/invalidate 存在、gcc 7.3.0 可用
  - 下一步：HLS top function 改 AXI-Stream 外殼（Step A）

> 進度隨開發更新。

---

## 關鍵文件索引

| 需要了解… | 看這裡 |
|-----------|--------|
| 整體架構、所有 Phase、Exit Criteria | `docs/project_plan.md` |
| 介面合約(函式簽章 / AXI-Lite 位址 / GPIO bit) | `docs/INTERFACE.md` |
| Phase 1 細步驟・checklist | `docs/phase1.md` |
| 各 Phase 細步驟 | `docs/phaseN.md`(做到該 Phase 才生成) |
| 設計決策紀錄 | `docs/decisions.md` |

> phase.md 採「做到該 Phase 才展開」,避免遠期細節因前期結果而需大改。
