# CLAUDE.md

PYNQ-Z2 上的即時 bass 數位效果器。效果運算(distortion / wobble)以 HLS 合成跑在 PL(FPGA)；PS(ARM)負責 codec 設定、參數控制與音訊搬運。詳見 `docs/project_plan.md`。

---

## Claude Code 的角色與邊界

**不碰（Ray 手動執行）：**
- ❌ Vivado / Vitis HLS（build、合成、bitstream 生成）
- ❌ 實機音訊測試

**可以：**
- ✅ SSH 上板執行指令（`ssh xilinx@192.168.2.99`）
- ✅ 寫/改 HLS C source、PS 端 C / Python、HLS testbench、文件

---

## 硬體環境

| 項目 | 內容 |
|------|------|
| 板子 | PYNQ-Z2 ×2（Ray、Claire 各一張） |
| 晶片 | Zynq XC7Z020（Cortex-A9, 32-bit） |
| Golden 板 | Ray 的板子 |
| codec | ADAU1761（48 kHz, 24-bit pad 32, I2S, I2C） |
| PYNQ image | 2.5 (Glasgow)，Ubuntu 18.04，**無需重刷** |

- **Ray 板 SSH**：`ssh xilinx@192.168.2.99`
- **Claire 板 SSH**：`192.168.2.99`（user: xilinx）
- **同步方式**：本機寫檔 → scp 上板 → Ray 手動 build / 測試

## 工具版本（凍結）

| 工具 | 版本 |
|------|------|
| Vivado | 2022.2 |
| Vitis HLS | 2022.2 |

> 兩塊板子與兩人開發機版本必須一致。

## 本機路徑

- Ray 本機：`EE\bass-fx-on-pynq`
- 板上路徑：`<TODO>`

---

## 開發規則（專案特定）

1. **每個模組獨立可測**，未驗證前不整合進上層。
2. **`process_sample()` 與外殼解耦**：運算核心不可寫入任何 AXI / mmap / stream 細節。A(PIO) 與 B(DMA) 共用同一份核心，這是 A→B 低成本升級的前提。
3. **改介面前先更新 `docs/INTERFACE.md`**：`process_sample()` 簽章、AXI-Lite 位址表、AXI GPIO bit 對應為凍結合約；任何變動先改文件並通知對方。
4. **不修改 PYNQ 系統 `libaudio.so`**：於自有 userspace / notebook 操作。
5. **HLS 禁用 `#pragma HLS DATAFLOW` 與非必要的 `UNROLL`**：已知資源過用主因；避免衝突 pragma。
6. **每一級運算後做 clamp 防溢位**：單效果與串接皆需。
7. **定點數型別統一**：依 `docs/INTERFACE.md` 約定的 Q 格式，不隨意混用寬度。

> 開發流程習慣（計畫確認、決策問 Ray、Phase 前提、token 提醒）見全域 skill `dev-workflow`。

---

## 版本控制

- Repo：`https://github.com/RayGur/bass-fx-on-pynq.git`
- **進 Git**：HLS C source、tcl、PS 程式、docs
- **不進 Git**：`.bit` / `.hwh` binary（另行共享；兩者須同名成對）

> Commit 格式與 branch 規範見全域 skill `commit`。

---

## 目前進度

- ✅ **Phase 0**：環境與硬體 sanity check — 完成
- ✅ **Phase 1**：最小 IP（passthrough）跑通自訂路徑 — 完成
  - Codec 初始化繞過 libaudio，用 `ol.axi_iic_0`（AxiIIC）直接設 ADAU1761
  - 音訊 record/play 用 MMIO 直讀，繞過 libaudio UIO
  - 技術債：`docs/decisions.md` 待補 D14
- ✅ **Phase 2**：distortion（hard clipping + AXI-Lite threshold/gain）— 完成
  - `ap_fixed<32,6>` 中間型別；threshold Q1.23 raw bit decode（需 `.range()`）
  - C Sim PASS（13 cases）、RTL Synthesis PASS（DSP×2, LUT 1%, 6.57 ns）
  - 技術債：threshold decode 記錄於 `docs/phase2.md`
- ✅ **Phase 3**：wobble（一階 IIR + LFO 掃頻）— 完成（整合於 Phase 6 branch）
  - Claire 演算法（B_LUT 16 entries Q15，triangle LFO，一階 IIR）整合進 Phase 6 AXI-Stream HLS
  - `state_t` 分 `iir_prev_L` / `iir_prev_R`（ap_fixed<32,2>）；`apply_wobble` 加 `bool is_l`
  - HLS 合成：II=1 達成；timing violation -3.08 ns 在 AXI-Lite wrapper（Vivado P&R 可解）
  - `wobble_dma_test.py` 板上驗證 PASS（L[0]=b×in 精確，state 跨 DMA 保留）
  - 實際音訊測試 PASS（`audio_dma.c` + codec，lfo_rate/depth 可熱改）
  - **Post-MVP 優化待辦**：wobble 深度不足（D26）、distortion 底噪放大（D27）— 見 `docs/decisions.md`
- 🔲 **Phase 4**：按鈕單選切換 + AXI-Lite 調參 → **MVP 完成**（**下一步**）
- 🔲 Phase 5：效果串接（2 switch 同開，需 P2/P3 各自通過）
- ✅ **Phase 6**：A→B 升級（C + DMA + 雙緩衝）— **完成**（branch: `phase6/wobble`）
  - 架構定案：C + DMA（見 D18–D21，`docs/phase6.md`）
  - 板上確認：`pynq.allocate()` 可用、gcc 7.3.0 可用
  - Step A（HLS AXI-Stream 外殼）✅；Step B（Vivado BD + PS `audio_dma.c`）✅
  - BUG 修復：D23（TKEEP=0）、D24（II=2 → R channel 不寫）、D25（HP0 64-bit 交錯寫）
  - DMA pipeline + codec 音訊驗證全部 PASS
  - 板上工作目錄：`~/bass-fx/wobble_dma/`；compile：`gcc audio_dma.c -lcma -lpthread -O2`

> 進度隨開發更新。

---

## 關鍵文件索引

| 需要了解… | 看這裡 |
|-----------|--------|
| 整體架構、所有 Phase、Exit Criteria | `docs/project_plan.md` |
| 介面合約（函式簽章 / AXI-Lite 位址 / GPIO bit） | `docs/INTERFACE.md` |
| 各 Phase 細步驟 | `docs/phaseN.md`（做到該 Phase 才生成） |
| 設計決策紀錄 | `docs/decisions.md` |
