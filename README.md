# Bass FX on PYNQ-Z2

即時 bass 數位效果器,效果運算跑在 PYNQ-Z2 的 FPGA(PL),ARM 處理器(PS)負責 codec 設定、參數控制與音訊搬運。

## 專案目標

以 Vitis HLS 合成自訂 Effect IP,在 PL 上實現兩個 bass 效果:

- **Distortion** — hard clipping,AXI-Lite 即時調 threshold / gain
- **Wobble** — 一階 IIR low-pass + LFO 掃頻,AXI-Lite 即時調 lfo_rate / lfo_depth

目標:現場彈奏 JB62 bass,透過板上 switch 與按鈕即時切換、調整效果,能從 amp 聽出差異。

## 硬體

| 項目 | 規格 |
|------|------|
| 開發板 | PYNQ-Z2(Zynq XC7Z020) |
| Codec | ADAU1761,48 kHz,24-bit,I2S |
| 工具 | Vivado 2022.2 / Vitis HLS 2022.2 |
| 輸入 | Fender JB62(被動式 bass,6.3→3.5mm) |
| 輸出 | 3.5→6.3mm → bass amp |

## 架構

```
JB62 → line-in → ADAU1761 codec
                      │
              PS (Cortex-A9 ARM)
              └ DMA + 雙緩衝（Phase 6，C程式）
                      │  AXI-Stream
              Effect IP on PL (HLS, 100 MHz)
              ├ distortion (hard clip, AXI-Lite threshold/gain)
              └ wobble (1st-order IIR + LFO, AXI-Lite lfo_rate/lfo_depth)
                      │
              ADAU1761 codec → HP-out → bass amp
```

效果參數透過 AXI-Lite 暫存器控制；switch / 按鈕透過 AXI GPIO 讀取。

## 開發階段

| Phase | 內容 | 狀態 |
|-------|------|------|
| 0 | 環境 sanity check、bypass 出聲 | ✅ |
| 1 | 最小 passthrough IP | ✅ |
| 2 | Distortion（hard clip + AXI-Lite threshold/gain） | ✅ |
| 3 | Wobble（IIR + LFO + AXI-Lite lfo_rate/lfo_depth） | ✅ |
| 4 | 按鈕切換 + AXI-Lite 調參（MVP） | 🔲 ← 下一步 |
| 5 | 效果串接 | 🔲 |
| 6 | A→B：DMA + 雙緩衝（必要步驟） | ✅ |

## 文件

| 文件 | 說明 |
|------|------|
| `docs/bass_fx_project_plan.md` | 整體架構、Phase 規劃、Exit Criteria |
| `docs/INTERFACE.md` | 介面合約（函式簽章 / AXI-Lite 位址 / GPIO bit） |
| `docs/decisions.md` | 設計決策紀錄 |
| `docs/phase1.md` | Passthrough IP、codec 初始化繞過 libaudio |
| `docs/phase2.md` | Distortion 實作、threshold decode 細節 |
| `docs/phase3.md` | Wobble 實作（IIR + LFO） |
| `docs/phase6.md` | DMA 升級、雙緩衝、AXI-Stream 外殼 |
| `CLAUDE.md` | Claude Code 開發規則與指引 |



## 測試與部署

### 1. 生成 bitstream（Vivado）

Vivado 合成完成後，將 bitstream 與 HWH 複製到 `vivado/`：

```bash
cp vivado/bass_fx/bass_fx.gen/sources_1/bd/bass_fx_bd/hw_handoff/bass_fx_bd.hwh vivado/bass_fx_bd.hwh
cp vivado/bass_fx/bass_fx.runs/impl_1/bass_fx_bd_wrapper.bit vivado/bass_fx_bd.bit
```

> **注意**：`.bit` 與 `.hwh` 須同名成對，不進 Git（另行共享）。

### 2. 傳至板子

```bash
scp vivado/bass_fx_bd.bit vivado/bass_fx_bd.hwh xilinx@192.168.2.99:~/bass-fx/wobble_dma/
scp ps/audio_dma.c ps/codec_init.py ps/wobble_dma_test.py xilinx@192.168.2.99:~/bass-fx/wobble_dma/
```

### 3. SSH 進板子

```bash
ssh xilinx@192.168.2.99
```

### 4. 板上編譯

```bash
cd ~/bass-fx/wobble_dma
gcc audio_dma.c -I/usr/include -L/usr/lib -lcma -lpthread -O2 -o audio_dma
```

### 5. 執行

**IIR 行為驗證**（不需 codec，純 DMA + 數值驗證）：

```bash
sudo python3 wobble_dma_test.py
```

**完整音訊執行**（需先初始化 codec）：

```bash
sudo python3 codec_init.py   # 載入 overlay + 初始化 ADAU1761
sudo ./audio_dma
```

### 6. 即時調整效果參數（另開終端機）

```bash
sudo python3 -c "
from pynq import MMIO; e = MMIO(0x40020000, 0x10000)
e.write(0x18, 1)                     # dist_en=1（開 distortion）
e.write(0x28, int(0.3*(1<<23)))      # threshold=0.3
e.write(0x30, 8)                     # gain=8（1–20）
e.write(0x20, 1)                     # wobble_en=1（開 wobble）
e.write(0x38, 178957)                # lfo_rate≈2 Hz（= freq_hz × 89479）
e.write(0x40, 100)                   # lfo_depth=100（0–100）
"
```


## 開發者

Ray、Claire — 各持一塊 PYNQ-Z2,以 Ray 板作為整合 golden 板。
