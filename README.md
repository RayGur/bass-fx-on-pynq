# Bass FX on PYNQ-Z2

即時 bass 數位效果器,效果運算跑在 PYNQ-Z2 的 FPGA(PL),ARM 處理器(PS)負責 codec 設定、參數控制與音訊搬運。

## 專案目標

以 Vitis HLS 合成自訂 Effect IP,在 PL 上實現兩個 bass 效果:

- **Distortion** — hard clipping,AXI-Lite 即時調 threshold / gain
- **Wobble** — 2nd-order IIR low-pass cascade + triangle LFO 掃頻,AXI-Lite 即時調 lfo_rate / lfo_depth / lfo_floor

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
              ├ 60 Hz notch (always-on, r=0.9997)
              ├ HPF Fc≈28 Hz (effect-conditional)
              ├ wobble (2nd-order IIR LP sweep + triangle LFO, AXI-Lite lfo_rate/lfo_depth/lfo_floor)
              └ distortion (hard clip + noise gate, AXI-Lite threshold/gain)
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
| 3 | Wobble（2nd-order IIR + LFO + AXI-Lite lfo_rate/lfo_depth/lfo_floor） | ✅ |
| 4 | 按鈕切換 + AXI-Lite 調參（MVP） | ✅ |
| 5 | 效果串接（sw[0]+sw[1] 同開） | ✅ |
| 6 | A→B：DMA + 雙緩衝（必要步驟） | ✅ |
| Post-MVP | 60 Hz notch、HPF、noise gate hysteresis、PC GUI（Tkinter）、串接順序修正 | ✅ |

## 文件

| 文件 | 說明 |
|------|------|
| `docs/bass_fx_project_plan.md` | 整體架構、Phase 規劃、Exit Criteria |
| `docs/INTERFACE.md` | 介面合約（函式簽章 / AXI-Lite 位址 / GPIO bit） |
| `docs/decisions.md` | 設計決策紀錄 |
| `docs/phase1.md` | Passthrough IP、codec 初始化繞過 libaudio |
| `docs/phase2.md` | Distortion 實作、threshold decode 細節 |
| `docs/phase3.md` | Wobble 實作（IIR + LFO） |
| `docs/phase4.md` | GPIO 控制迴路、RGB LED、preset 參數組（Phase 4+5） |
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
scp vivado/bass_fx_bd.bit vivado/bass_fx_bd.hwh xilinx@192.168.2.99:~/bass-fx/ui_dev_v2/
scp ps/audio_dma.c ps/codec_init.py ps/start.sh xilinx@192.168.2.99:~/bass-fx/ui_dev_v2/
scp ui/ctrl_client.py xilinx@192.168.2.99:~/bass-fx/ui_dev_v2/
```

### 3. 板上編譯

```bash
ssh xilinx@192.168.2.99
cd ~/bass-fx/ui_dev_v2
gcc audio_dma.c -lcma -lpthread -O2 -DNDEBUG -o audio_dma
```

### 4a. 啟動（板上獨立模式）

```bash
bash ~/bass-fx/ui_dev_v2/start.sh
```

`start.sh` 自動處理 codec 初始化 + audio_dma 啟動，root-aware 不重複 sudo。

**板上控制**（`audio_dma` 執行中）：
- `sw[0]`（M20）：distortion on/off → LD4（RGB）亮滅
- `sw[1]`（M19）：wobble on/off → LD5（RGB）亮滅
- `btn[0]`（D19）：短按切換 distortion low ↔ high → led[0] 亮滅
- `btn[1]`（D20）：短按切換 wobble slow ↔ fast → led[1] 亮滅
- `btn[2]`（L20）：短按循環 wah depth preset A→B→C
- sw[0]+sw[1] 同開 = 效果串接（wobble → dist）

### 4b. 啟動（PC GUI 模式）

```bash
# PC 端（需 pip install paramiko）
python3 ui/bass_ui.py
```

GUI 透過 SSH 連線板子，支援 stomp 切換、旋鈕即時調參、btn/sw 雙向同步。


## 開發者

Ray、Claire — 各持一塊 PYNQ-Z2,以 Ray 板作為整合 golden 板。
