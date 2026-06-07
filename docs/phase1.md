# Phase 1 — 最小 IP（passthrough）實作計畫書

> **目標**：打通自訂 Effect IP 的完整資料路徑，尚無效果。  
> **Exit Criteria**：聲音能穿過自訂 IP 從 amp 出來，與 `bypass()` 聽感一致。  
> **前置條件**：Phase 0 Exit Criteria 通過（JB62 直插 `bypass()` 出聲）。

---

## 1. 產出檔案總覽

```
hls/effect_ip/
├── effect_ip.h              # 型別定義、函式宣告（共用 header）
├── process_sample.cpp       # HLS top function + AXI-Lite pragma
├── distortion.cpp           # distortion 骨架（Phase 1 直接 passthrough）
├── wobble.cpp               # wobble 骨架（Phase 1 直接 passthrough，Claire 的起點）
├── tb_process_sample.cpp    # HLS C simulation testbench
└── run_hls.tcl              # Vitis HLS 建 project + 合成 + export 一鍵腳本

ps/
└── pio_loop.py              # PS 端 PIO 音訊搬運迴圈
```

---

## 2. HLS 檔案設計

### 2.1 `effect_ip.h` — 共用型別與函式宣告

定義三個型別：

| 型別 | 定義 | 說明 |
|------|------|------|
| `sample_t` | `ap_fixed<24,1>` | Q1.23，對應 ADAU1761 24-bit，範圍 [−1, +1) |
| `param_t` | `int` | PS 傳入的參數，IP 內部解讀為對應定點值 |
| `state_t` | struct | 跨 sample 狀態；Phase 1 放 placeholder，Phase 3 由 Claire 填 LFO 相位與 IIR 歷史值 |

宣告三個函式：`process_sample()`、`apply_distortion()`、`apply_wobble()`。

---

### 2.2 `process_sample.cpp` — HLS Top Function

**責任**：HLS 合成入口，宣告所有 AXI-Lite pragma，呼叫效果子函式。

函式簽章：

```c
void process_sample(
    sample_t  in_l,       sample_t  in_r,      // 輸入（mono 來源已於 IP 入口複製為 L/R）
    sample_t *out_l,      sample_t *out_r,     // 輸出
    bool      dist_en,    bool      wobble_en, // 效果開關（來自 sw[0]/sw[1]）
    param_t   threshold,  param_t   gain,      // distortion 參數
    param_t   lfo_rate,   param_t   lfo_depth, // wobble 參數
    state_t  *state                            // 跨 sample 狀態
);
```

**AXI-Lite pragma**（每個控制參數都需要）：

```c
#pragma HLS INTERFACE s_axilite port=dist_en    bundle=ctrl
#pragma HLS INTERFACE s_axilite port=wobble_en  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=threshold  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=gain       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=lfo_rate   bundle=ctrl
#pragma HLS INTERFACE s_axilite port=lfo_depth  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return     bundle=ctrl
```

**Phase 1 邏輯**：直接 passthrough，不呼叫任何效果：

```c
*out_l = in_l;
*out_r = in_r;
```

---

### 2.3 `distortion.cpp` — Distortion 骨架

Phase 1 直接回傳輸入：

```c
sample_t apply_distortion(sample_t in, param_t threshold, param_t gain) {
    return in;  // Phase 2 填入 hard clipping 邏輯
}
```

---

### 2.4 `wobble.cpp` — Wobble 骨架（Claire 的起點）

Phase 1 直接回傳輸入：

```c
sample_t apply_wobble(sample_t in, param_t lfo_rate, param_t lfo_depth, state_t *state) {
    return in;  // Phase 3 由 Claire 填入 IIR + LFO 邏輯
}
```

> **Claire 注意**：Phase 3 開始前，先與 Ray 確認 `state_t` 欄位（LFO 相位、IIR 歷史值），再修改 `effect_ip.h` 中的 `state_t` 定義，並通知 Ray 更新 `process_sample.cpp`。

---

### 2.5 `tb_process_sample.cpp` — HLS Testbench

在 Vitis HLS 跑 C Simulation，驗證 passthrough 正確性：

- 餵已知輸入（如 `0.5`、`-0.5`、`0.0`、最大值、最小值）
- 斷言 `out_l == in_l`、`out_r == in_r`
- C Sim pass 才進合成

---

### 2.6 `run_hls.tcl` — Vitis HLS 自動化腳本

Ray 在 Vitis HLS Tcl Console 執行，一鍵完成：

1. 建 project，設定 part `xc7z020clg400-1`，clock 10 ns（100 MHz）
2. 加入所有 source 檔與 testbench
3. 執行 C Simulation
4. 執行 `csynth_design`（合成）
5. 執行 `export_rtl`（輸出 Vivado IP）

---

## 3. PS 端：`pio_loop.py`

載入 bitstream，以 PIO 方式逐 sample 搬運音訊：

```python
from pynq import Overlay
from pynq.lib.audio import AudioDirect  # 或 pAudio，依板上 API

overlay = Overlay("bass_fx.bit")
# 設定 line-in、開始 PIO 迴圈
# while True:
#     sample = read_sample_from_codec()
#     overlay.effect_ip.write(INPUT_OFFSET, sample)
#     out = overlay.effect_ip.read(OUTPUT_OFFSET)
#     write_sample_to_codec(out)
```

> 實際 API 依 PYNQ 2.5 上的 `pAudio` 模組確認。offset 位址於 Ray 完成 Step A 後填入。

---

## 4. Ray 手動執行 Checklist

以下步驟**按順序執行**，每步通過再往下。

### Step A — Vitis HLS 合成
- [ ] 在 Vitis HLS 開啟 Tcl Console，執行 `run_hls.tcl`
- [ ] C Simulation pass（testbench 無 assertion error）
- [ ] Synthesis 完成，無 error，確認 II = 1
- [ ] Export RTL 完成，產出 IP 目錄
- [ ] 找到 `xprocess_sample_hw.h`（或類似名稱），回報各參數的 `ADDR_*` 定義給 Ray → 填進 `docs/INTERFACE.md`

### Step B — Vivado Block Design
- [x] 新建 Vivado project，匯入 HLS 產出的 IP
- [x] 保留 `audio_codec_ctrl`（I2S master：bclk/lrclk/sdata）
- [x] 加入 Effect IP（AXI-Lite port 接 PS M_AXI_GP0）
- [x] 加入 AXI GPIO（sw[0:1]、btn[0:3]、led[0:3]）
- [x] 加入 `clk_oddr`（RTL module reference，解決 MCLK → IO 的 clock placer 問題）
- [x] Address Editor 取得 Effect IP base address，更新 `docs/INTERFACE.md`
- [x] Validate Design 通過
- [x] 加入 `axi_iic:2.1`，連接 U9(SCL)/T9(SDA)，XDC 加 PULLUP（見 D14）
- [x] axi_iic base address `0x40800000` 填入 `docs/INTERFACE.md`

### Step C — 產生 Bitstream
- [x] Generate Bitstream 成功（含 ODDR + 修正後的 XDC port 名稱 `bclk_0` 等）
- [x] 加入 AXI IIC 後重新 Generate Bitstream 成功
- [x] `bass_fx.bit` + `bass_fx.hwh` 同名成對，位於板上 `/home/xilinx/jupyter_notebooks/bass_fx/`

### Step D — 上板確認
- [x] Overlay 載入正常，ip_dict 含全部 4 個 IP（含 axi_iic_0）
- [x] Effect IP sanity check PASS：`run_effect(0.5, -0.5)` → `(0.5000, -0.5000)`
- [x] codec configure() hang 已解（monkey-patch + AxiIIC 直接初始化；詳見 D14）
- [x] JB62 直插 line-in，接 amp，聲音穿透確認（純 Python MMIO，有 dropout，MVP 可接受；詳見 D15）
- [x] 主觀聽感確認（有效果 passthrough，音質 MVP 等級；Phase 6 DMA 升級後修）
- [x] **Phase 1 Exit Criteria 通過** → Claire 可開始 Phase 3

---

## 7. 已發現問題與解法

### P1. IO Clock Placer failed（MCLK 無法直接驅動 IO）
- **症狀**：`[Place 30-99] IO Clock Placer failed`
- **原因**：MMCM/clk_wiz 的 clock output 不能直接驅動 IO pin
- **解法**：加入 `rtl/clk_oddr.v`（ODDR primitive），在 BD 以 module reference 連入，轉換為 data output 再驅動 U5
- **狀態**：✅ 已解決

### P2. BD wrapper port 名稱帶 `_0` 後綴
- **症狀**：XDC `get_ports bclk` → `No ports matched`
- **原因**：`audio_codec_ctrl_0` 的 port 在 BD wrapper 中命名為 `bclk_0`、`lrclk_0`、`sdata_o_0` 等
- **解法**：XDC 全部改為 `_0` 後綴版本
- **狀態**：✅ 已解決

### P3. AudioADAU1761.configure() 無限 hang
- **症狀**：`init_audio()` 執行超過 3 分鐘不返回
- **原因**：ADAU1761 I2C 走 PL 腳（U9/T9），需 AXI IIC IP；`libaudio.so` 的 `config_audio_pll()` 在等 codec PLL lock，但因缺少 AXI IIC，I2C 訊號送不到 codec
- **解法（實際採用）**：
  1. BD 加入 `axi_iic:2.1`，接 U9/T9，XDC 加 PULLUP，rebuild bitstream
  2. PYNQ 2.5 HWH parser 不建 DT entry → `/dev/i2c-X` 不出現 → 改用 `pynq.lib.iic.AxiIIC` 直接操作 MMIO
  3. `AudioADAU1761.configure()` 在 `__init__` 中自動呼叫且會 hang → 在 `Overlay()` 前 monkey-patch 為空實作，略過 libaudio I2C 路徑
  4. 自行撰寫 `init_codec_via_axiic()` 完成 ADAU1761 初始化（PLL + 暫存器 + select_line_in）
- **狀態**：✅ **已解決（詳見 D14）**

### P4. libaudio.record() 導致 kernel crash 重開機
- **症狀**：呼叫 `audio.record()` 後約 3 秒，ARM 總線錯誤，板子重啟
- **原因**：`libaudio.record()` 對 `/dev/uio0` 執行 `mmap(size=audio_mmap_size)` 但 size 與實際 UIO 裝置不符 → bus error → kernel panic
- **解法**：棄用 libaudio record/play，改用純 Python 直接輪詢 `audio_ip.mmio.array`（numpy mmap）實作 `py_record()` / `py_play()`
- **技術債**：Python 輪詢 ~20-30μs/iter，I2S frame 20.8μs，會掉 sample，音質差。這是 MVP 暫時方案；Phase 6 AXI DMA 升級後根本解決
- **狀態**：✅ **已解決（詳見 D15）**

---

## 5. 介面更新（Phase 1 完成後）

Step A 完成後，Ray 將以下資訊填入 `docs/INTERFACE.md`：

- Effect IP base address（從 Vivado Address Editor）
- 各參數 byte offset（從 HLS 產出的 `*_hw.h`）

填完後凍結，通知 Claire。

---

## 6. 注意事項

- **不使用 `#pragma HLS DATAFLOW`**：已知資源過用主因，Phase 1 不需要。
- **不使用非必要 `UNROLL`**：passthrough 邏輯極簡，不需要展開。
- `wobble.cpp` 保持最小骨架，介面改動等 Claire Phase 3 開始前再協調。
- bitstream 與 hwh **不進 Git**，另行以 scp 傳板。
