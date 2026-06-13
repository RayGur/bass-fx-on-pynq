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
              ├ Mode A: per-sample PIO（MVP）
              └ Mode B: DMA + 雙緩衝（Phase 6）
                      │
              Effect IP on PL (HLS)
              ├ distortion (hard clip)
              └ wobble (IIR + LFO)
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
| 3 | Wobble（IIR + LFO + AXI-Lite lfo_rate/lfo_depth） | 🔲 |
| 4 | 按鈕切換 + AXI-Lite 調參（MVP） | 🔲 |
| 5 | 效果串接 | 🔲 |
| 6 | A→B：DMA + 雙緩衝 + 中斷 | 🔄 |

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



## 測試

生成完.bit和 .hwh以及ps 端driver
`cp vivado\bass_fx\bass_fx.gen\sources_1\bd\bass_fx_bd\hw_handoff\bass_fx_bd.hwh vivado/bass_fx_bd.hwh`

`cp vivado\bass_fx\bass_fx.runs\impl_1\bass_fx_bd_wrapper.bit vivado/bass_fx_bd.bit`

SCP: 
`scp vivado/bass_fx_bd.bit vivado/bass_fx_bd.hwh xilinx@192.168.2.99:~/`
`scp ps/audio_dma.c xilinx@192.168.2.99:~/`

ssh進板子
`ssh pynq-z2`

先build
`gcc audio_dma.c -I/usr/include -L/usr/lib -lcma -lpthread -O2 -o audio_dma`

再跑
`sudo python3 ~/dma_test.py` (僅部分測試用)
`sudo python3 codec_init.py && sudo ./audio_dma`


## 開發者

Ray、Claire — 各持一塊 PYNQ-Z2,以 Ray 板作為整合 golden 板。
