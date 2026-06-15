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
- ✅ **Phase 3**：wobble（2nd-order IIR cascade + LFO 掃頻）— 完成（整合於 Phase 6 branch；14.1 升級）
  - Claire 演算法（B_LUT 16 entries Q15，triangle LFO）整合進 Phase 6 AXI-Stream HLS
  - `state_t` 分 `iir_prev_L/R`（stage-1）+ `iir_prev2_L/R`（stage-2，14.1 新增）
  - `apply_wobble` 加 `bool is_l`、`param_t lfo_floor`（14.1 新增，wah depth preset）
  - 14.1 優化（2026-06-15）：B_LUT 換成 10–2000 Hz 對數等比；升 2nd-order IIR（12 dB/oct）；新增 `lfo_floor` AXI-Lite 參數（offset 0x48，btn2 切換 A/B/C preset）
  - 14.1 驗證全 PASS（2026-06-15）：HLS C-sim PASS、synthesis II=1、Vivado rebuild、板上三個 preset（A/B/C）音訊驗聽 PASS
  - **Post-MVP 14.2/14.7（2026-06-15 實作，板上驗聽 PASS）**：60 Hz notch（r=0.9997，BW≈4.6 Hz，r 從 0.9999 降低以縮短 ringing τ 208 ms→69 ms）+ HPF（Fc≈28 Hz）+ noise gate；state_t 擴增至 19 欄（含 gate hysteresis 狀態 bool L/R）；AXI-Lite 介面不變
  - **Post-MVP 14.9（2026-06-16 實作）**：noise gate hysteresis（open=0.001，close=0.0003）；板上驗聽 gate chatter 仍存在，可繼續優化
  - **Post-MVP 14.10/14.11（2026-06-16 記錄）**：13 秒週期茲擦（PS 端原因未查明）；碰弦茲擦（類比前端 EMI，已知限制）
- ✅ **Phase 4 + 5**：GPIO 控制迴路（sw/btn/LED/RGB LD4+LD5）+ 效果串接 — **MVP 完成**（branch: `feat/gpio`）
  - sw[0/1] 即時切換 dist_en/wobble_en；btn[0/1] debounce 切換 low/high preset；btn[2] 循環 wah depth preset A/B/C（14.1 新增）
  - RGB LD4/LD5 顯示 switch 狀態；led[0/1] 顯示 preset 強度
  - axi_gpio_2（0x4003_0000）新增至 BD；hw_cons.xdc 補 6 pin（port: `rgbleds_tri_o_tri_o`）
  - **前置**：每次執行前需先 `sudo python3 codec_init.py`（載入 overlay + 初始化 codec）
  - Post-MVP 待辦：LED 亮度（14.3）、KRK 喇叭 crash（14.4）、啟動整合腳本（14.5）— 見 `docs/bass_fx_project_plan.md`
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
