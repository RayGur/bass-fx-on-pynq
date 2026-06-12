# Phase 6 — A→B 升級：C + DMA + 雙緩衝 + 中斷

> **目標**：以 AXI DMA 取代 Python PIO，解決 D15 音質問題，解放 CPU，作為技術亮點。  
> **負責人**：Ray  
> **前置條件**：
> - Phase 2 Exit Criteria 通過（distortion 上板驗證 PASS）✅
> - 板上確認：`pynq.allocate()` 可用（paddr 64-byte 對齊）✅、gcc 7.3.0 可用 ✅
> - Phase 3（wobble）尚未完成，但 Phase 6 只動傳輸層，`process_sample()` 不改，與 wobble 無依賴
>
> **Exit Criteria**：
> - DMA 搬運下音訊正常，distortion 效果不變
> - 無明顯音質劣化（與 Phase 2 C PIO 基準比較）
> - 延遲較 Python PIO 明顯改善（目標 < 20 ms，原 Python PIO 無法保證）
> - `process_sample()` 未修改（零改動，由 diff 確認）
> - HLS C Simulation 通過（新 top function stream 版本）

---

## 1. 架構設計（D18）

### 資料流

```
ADAU1761 codec (I2S, 48kHz)
       ↕ I2S
audio_codec_ctrl (AXI-Lite MMIO)
       ↕ MMIO polling（C 程式，< 1 μs/次）
   [input_buf_A / input_buf_B]   ← ping-pong，pynq.allocate，256 samples × 2 ch
       ↕
   AXI DMA MM2S ──flush()──→ AXI-Stream ──→ [AXI Stream FIFO] ──→
                                                                    Effect IP (HLS)
                                            [AXI Stream FIFO] ←── ↑（process_sample() 不動）
       ↕                                                           AXI-Lite（參數）
   AXI DMA S2MM ←──invalidate()──← AXI-Stream ←──────────────────
       ↕
   [output_buf_A / output_buf_B]  ← ping-pong
       ↕ MMIO write（C 程式）
audio_codec_ctrl (AXI-Lite MMIO)
```

### Ping-pong 時序

```
時間 →   [    Buffer A    ]  [    Buffer A    ]  ...
         [    Buffer B    ]  [    Buffer B    ]  ...

PS C:    fill A from codec   fill B from codec
DMA:                    process A → write A out
PS C:    write A out to codec
```

PS 填 buffer A 的同時，DMA 在硬體上處理前一個 buffer B，兩者並行。

---

## 2. 改動地圖

| 層 | Phase 5（現狀）| Phase 6（目標）| 改動量 |
|---|---|---|---|
| `process_sample()` | 不動 | 不動 | **0** |
| HLS top function | AXI-Lite per-sample | AXI-Stream loop + TLAST | 小（新包裝層） |
| Block design | 無 DMA | 加 AXI DMA + 2× AXI Stream FIFO + Concat | 中 |
| PS 音訊搬運 | Python MMIO polling | C + DMA transfer + 中斷/雙緩衝 | 大 |
| PS 參數控制 | Python MMIO（不變）| Python MMIO（不變）| **0** |

---

## 3. Step 1 — HLS：新 top function（AXI-Stream 外殼）

### 3.1 `process_sample.cpp` 修改

移除 in_l / in_r / out_l / out_r 的 AXI-Lite port，改成 AXI-Stream；`process_sample()` 本體不動，外面包 stream loop。

```cpp
#include "effect_ip.h"
#include "ap_axi_sdata.h"
#include "hls_stream.h"

typedef ap_axis<32, 0, 0, 0> audio_pkt_t;  // 32-bit data + last

void process_sample(
    hls::stream<audio_pkt_t> &s_in,
    hls::stream<audio_pkt_t> &s_out,
    int         n_samples,     // 每次 DMA 傳的 stereo pair 數
    bool        dist_en,
    bool        wobble_en,
    param_t     threshold,
    param_t     gain,
    param_t     lfo_rate,
    param_t     lfo_depth
) {
#pragma HLS INTERFACE axis      port=s_in       depth=512
#pragma HLS INTERFACE axis      port=s_out      depth=512
#pragma HLS INTERFACE s_axilite port=n_samples  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=dist_en    bundle=ctrl
#pragma HLS INTERFACE s_axilite port=wobble_en  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=threshold  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=gain       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=lfo_rate   bundle=ctrl
#pragma HLS INTERFACE s_axilite port=lfo_depth  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return     bundle=ctrl

    // 跨 buffer 保留的狀態（D20）
    static state_t state = {0, sample_t(0)};

    // 每次處理 n_samples 個 stereo pair（L/R 交錯）
    // stream 總長度 = n_samples × 2 words
    int total = n_samples * 2;

    for (int i = 0; i < total; i += 2) {
#pragma HLS PIPELINE II=1

        // 讀 L / R
        audio_pkt_t pkt_l = s_in.read();
        audio_pkt_t pkt_r = s_in.read();

        sample_t in_l, in_r, out_l, out_r;
        in_l.range(23, 0) = ap_int<24>(pkt_l.data);
        in_r.range(23, 0) = ap_int<24>(pkt_r.data);

        // 呼叫運算核心（不動）
        process_sample_core(in_l, in_r, &out_l, &out_r,
                            dist_en, wobble_en,
                            threshold, gain,
                            lfo_rate, lfo_depth,
                            &state);

        // 寫 L / R，最後一 pair 設 TLAST（D21 陷阱）
        bool is_last = (i == total - 2);
        audio_pkt_t out_pkt_l, out_pkt_r;
        out_pkt_l.data = (ap_int<32>)out_l;
        out_pkt_l.last = 0;
        out_pkt_r.data = (ap_int<32>)out_r;
        out_pkt_r.last = is_last ? 1 : 0;  // TLAST 在最後一個 word

        s_out.write(out_pkt_l);
        s_out.write(out_pkt_r);
    }
}
```

> **重要**：原 `process_sample()` 邏輯搬到 `process_sample_core()` 內部函式，top function 只做 stream loop。這樣 testbench 還可以直接測 core。

### 3.2 `effect_ip.h` 新增

```cpp
// Phase 6：運算核心（解耦自 stream top function）
void process_sample_core(
    sample_t  in_l,  sample_t  in_r,
    sample_t *out_l, sample_t *out_r,
    bool dist_en, bool wobble_en,
    param_t threshold, param_t gain,
    param_t lfo_rate,  param_t lfo_depth,
    state_t *state
);
```

### 3.3 PIPELINE II=1 限制

Loop 內有兩次 `s_in.read()`，HLS 可能把這兩個 read 展開成兩個 pipeline stage（II=2）。  
**若 II=1 達不到**，改成每次只讀一個 word，用奇偶 index 區分 L/R：

```cpp
for (int i = 0; i < total; i++) {
#pragma HLS PIPELINE II=1
    audio_pkt_t pkt = s_in.read();
    sample_t sample;
    sample.range(23,0) = ap_int<24>(pkt.data);
    bool is_L = (i % 2 == 0);
    // ...累積 L，下一個 cycle 有 R 時一起處理
}
```

先試原版，synthesis report 確認 II 後再決定。

---

## 4. Step 2 — HLS C Simulation（stream 版 testbench）

在 `tb_process_sample.cpp` 新增 stream 測試：

```cpp
// Phase 6 stream testbench 骨架
hls::stream<audio_pkt_t> s_in, s_out;
const int N = 16;  // 測試 16 個 stereo pair

// 填入測試資料（distortion 輸入）
for (int i = 0; i < N*2; i++) {
    audio_pkt_t p;
    p.data = int(0.5 * (1<<23));  // 0.5 full scale
    p.last = (i == N*2-1) ? 1 : 0;
    s_in.write(p);
}

// 執行 DUT
process_sample(s_in, s_out, N, /*dist_en=*/1, 0,
               int(0.3*(1<<23)), 8, 0, 0);

// 驗證輸出
for (int i = 0; i < N*2; i++) {
    audio_pkt_t p = s_out.read();
    // distortion with threshold=0.3, gain=8: 0.5*8=4.0 → clipped to 0.3
    if (i % 2 == 1 && i == N*2-1)
        assert(p.last == 1);  // TLAST 在最後
}
```

---

## 5. Step 3 — Vivado Block Design 修改

### 5.1 新增 IP

1. **AXI Direct Memory Access**（`axi_dma:7.1`）
   - Enable both MM2S and S2MM
   - Enable interrupts（兩個 channel）
   - Width of buffer length register: 23（支援最大 8MB，2048 bytes 綽綽有餘）
   - Burst size: 256（或讓 Vivado 自動）

2. **AXI4-Stream Data FIFO**（`axis_data_fifo:2.0`）×2
   - 一個在 DMA MM2S → Effect IP 之間
   - 一個在 Effect IP → DMA S2MM 之間
   - FIFO depth: 512（> 2 × n_samples = 512 words）

3. **Concat**（`xlconcat:2.1`）
   - 2 inputs：接 DMA MM2S IRQ + S2MM IRQ
   - 輸出接 PS `IRQ_F2P[0:0]`

### 5.2 接線

```
PS M_AXI_GP0 → AXI Interconnect → axi_dma_0 (S_AXI_LITE)
                                 → Effect IP (S_AXI_CTRL)

PS S_AXI_HP0 ← AXI DMA (M_AXI_MM2S + M_AXI_S2MM)

axi_dma_0 M_AXIS_MM2S → axis_fifo_in → Effect IP s_in
Effect IP s_out → axis_fifo_out → axi_dma_0 S_AXIS_S2MM

axi_dma_0 mm2s_introut → xlconcat → PS IRQ_F2P[0]
axi_dma_0 s2mm_introut ↗
```

### 5.3 Address Editor

確認後填入 `INTERFACE.md`：
- Effect IP base address（預期 `0x4002_0000`，但 re-layout 後可能改變）
- AXI DMA base address（TBD）

---

## 6. Step 4 — PS C 程式（`audio_dma.c`）

buffer 分配與 cache 管理方案已定案（D22）：使用板上預裝的 **`libcma.so`**（xlnk userspace wrapper）。  
編譯：`gcc audio_dma.c -I/usr/include -L/usr/lib -lcma -lpthread -O2 -o audio_dma`（需 sudo 執行）

> **板上確認**（執行前先驗）：`ls -la /usr/lib/libcma.so /usr/include/libxlnk_cma.h`

### 6.1 整體結構

```c
// audio_dma.c — Phase 6 C + DMA 音訊搬運
#include <stdint.h>
#include <stdlib.h>
#include <libxlnk_cma.h>   // cma_alloc / cma_flush_cache / cma_invalidate_cache

#define N_SAMPLES     256
#define N_WORDS       (N_SAMPLES * 2)   // L/R interleaved
#define BUF_BYTES     (N_WORDS * 4)     // 2048 bytes

// ping-pong buffer（CMA，physically contiguous）
int32_t  *in_buf[2],  *out_buf[2];
uint32_t  in_phys[2],  out_phys[2];

void init_buffers() {
    for (int i = 0; i < 2; i++) {
        in_buf[i]   = cma_alloc(BUF_BYTES, 1);   // cacheable=1
        out_buf[i]  = cma_alloc(BUF_BYTES, 1);
        in_phys[i]  = cma_get_phy_addr(in_buf[i]);
        out_phys[i] = cma_get_phy_addr(out_buf[i]);
    }
}

void free_buffers() {
    for (int i = 0; i < 2; i++) {
        cma_free(in_buf[i]);
        cma_free(out_buf[i]);
    }
}

void fill_input_buf(int32_t *buf) {
    for (int i = 0; i < N_SAMPLES; i++) {
        while (!(mmio_read(I2S_STATUS_REG) & RX_VALID));
        buf[i*2]   = mmio_read(I2S_DATA_RX_L_REG);
        buf[i*2+1] = mmio_read(I2S_DATA_RX_R_REG);
    }
}

void drain_output_buf(int32_t *buf) {
    for (int i = 0; i < N_SAMPLES; i++) {
        while (!(mmio_read(I2S_STATUS_REG) & TX_READY));
        mmio_write(I2S_DATA_TX_L_REG, buf[i*2]);
        mmio_write(I2S_DATA_TX_R_REG, buf[i*2+1]);
    }
}

void audio_loop() {
    int cur = 0;

    // 填第一個 buffer（prime）
    fill_input_buf(in_buf[0]);

    while (1) {
        int nxt = 1 - cur;

        // 1. Flush：PS cache → DRAM，確保 DMA 讀到最新資料（D21）
        cma_flush_cache(in_buf[cur], in_phys[cur], BUF_BYTES);

        // 2. 啟動 DMA（non-blocking）
        dma_start_send(in_phys[cur], BUF_BYTES);
        dma_start_recv(out_phys[cur], BUF_BYTES);

        // 3. PS 填下一個 input buffer（與 DMA 並行）
        fill_input_buf(in_buf[nxt]);

        // 4. 等 DMA 完成（polling DMA status reg 或 IRQ）
        dma_wait();

        // 5. Invalidate：清除 PS cache stale 內容，確保讀到 DMA 寫入的新資料（D21）
        cma_invalidate_cache(out_buf[cur], out_phys[cur], BUF_BYTES);

        // 6. 寫回 codec
        drain_output_buf(out_buf[cur]);

        cur = nxt;
    }
}
```

### 6.2 Python 端（控制用）

參數控制繼續用 Python MMIO，不需改：

```python
# Phase 6 後，參數 offset 以新 synthesis 結果為準
effect.write(ADDR_DIST_EN,   1)
effect.write(ADDR_THRESHOLD, int(0.3 * (1<<23)))
effect.write(ADDR_GAIN,      8)
# DMA 啟動由 C 程式負責，Python 只管參數
```

---

## 7. 已知踩坑（D21）

| 陷阱 | 症狀 | 對策 |
|------|------|------|
| TLAST 未設 | `dma.recvchannel.wait()` 永遠不返回，程式 hang | stream loop 最後一個 word 設 `.last = 1` |
| Cache flush 漏做 | PL 讀到 stale data，輸出是舊的 sample | PS 寫完 buffer 後立即 `flush()` 再 start DMA |
| Cache invalidate 漏做 | PS 讀到 stale output，輸出無效 | DMA 結束後 `invalidate()` 再讀 out_buf |
| IIR state 跨 buffer 重置 | 每個 buffer 開頭有咔噠聲 | HLS `static state_t state` 確認有在 synthesis 保留 |
| II 不等於 1 | throughput 不足（256 samples @ 100MHz 需 512 cycles）| 避免 loop 內 double-read，改奇偶 index 版本 |
| AXI Stream FIFO 深度不足 | S2MM backpressure 導致 DMA stall | FIFO depth ≥ 2 × n_samples |

---

## 8. 實作 Checklist

### Step A — HLS 修改
- [ ] `process_sample.cpp` 改成 stream top function（外殼 loop）
- [ ] `effect_ip.h` 新增 `process_sample_core()` 宣告
- [ ] 原 `process_sample()` 邏輯移入 `process_sample_core()`
- [ ] HLS C Simulation PASS（stream testbench，驗 TLAST + output 正確）
- [ ] RTL Synthesis：確認 II=1，LUT/DSP 用量合理
- [ ] 確認 synthesis report 中 static state 保留（非 ROM）
- [ ] Export IP

### Step B — Vivado Block Design
- [ ] 加入 AXI DMA IP（MM2S + S2MM，開 interrupt）
- [ ] 加入 2 × AXI Stream FIFO（depth ≥ 512）
- [ ] 加入 Concat IP，接 IRQ_F2P[0]
- [ ] Effect IP Refresh IP
- [ ] Address Editor 確認，填入 `INTERFACE.md`
- [ ] Generate Bitstream

### Step C — PS C 程式
- [ ] 確認板上 `libcma.so` 存在：`ls -la /usr/lib/libcma.so /usr/include/libxlnk_cma.h`
- [ ] 實作 `audio_dma.c`（fill → `cma_flush_cache` → DMA start → fill next → DMA wait → `cma_invalidate_cache` → drain）
- [ ] 板上編譯：`gcc -O2 audio_dma.c -I/usr/include -L/usr/lib -lcma -lpthread -o audio_dma`
- [ ] 單次 DMA transfer 測試（不 ping-pong，只跑一次 send + recv，驗輸出正確）
- [ ] Ping-pong 迴圈測試（連續跑 5 秒，用示波器或錄音確認無 glitch）

### Step D — 上板整合驗證
- [ ] 接 JB62 + amp，啟動 `audio_dma`
- [ ] 確認 distortion 效果正常
- [ ] 確認無 click / glitch（IIR state 跨 buffer 正常）
- [ ] 與 Phase 2 Python PIO 聽感比較，確認音質改善
- [ ] **Phase 6 Exit Criteria 確認**

---

## 9. 參考資料

| 資料 | URL |
|------|-----|
| PYNQ DMA 官方文件（v2.5）| https://pynq.readthedocs.io/en/v2.5/pynq_libraries/dma.html |
| PYNQ DMA Tutorial Part 1（BD 設計）| https://discuss.pynq.io/t/tutorial-pynq-dma-part-1-hardware-design/3133 |
| Element14 Vitis HLS + DMA Training Part 2（TLAST 陷阱）| https://community.element14.com/technologies/fpga-group/b/blog/posts/pynq-and-zynq-the-vitis-hls-accelerator-with-dma-training---part-2-add-the-accelerated-ip-to-a-vivado-design |
| Xilinx UG1399 AXI4-Stream 介面說明 | https://docs.amd.com/r/en-US/ug1399-vitis-hls/How-AXI4-Stream-is-Implemented |
| PYNQ cache coherency 討論 | https://discuss.pynq.io/t/cache-coherency-in-pynq-image-using-pynq-z2-board-ethernet-bring-up/4078 |
| Audio-Lab-PYNQ（PYNQ-Z2 音訊 DMA 參考）| https://github.com/cramsay/Audio-Lab-PYNQ |
| cathalmccabe PYNQ DMA Tutorial | https://github.com/cathalmccabe/PYNQ_tutorials |
| PYNQ Workshop Session 4 DMA Notebook | https://github.com/Xilinx/PYNQ_Workshop/blob/master/Session_4/6_dma_tutorial.ipynb |
| Ping-pong buffer 說明 | https://audiodsplab.wordpress.com/ping-pong-buffer-audio-stream/ |
| PYNQ libxlnk_cma.h（C API）| https://github.com/Xilinx/PYNQ/blob/master/sdbuild/packages/libsds/libcma/libxlnk_cma.h |
| PYNQ pynqlib.c（flush/invalidate 實作）| https://github.com/Xilinx/PYNQ/blob/master/sdbuild/packages/libsds/libcma/pynqlib.c |
| XRT on PYNQ-Z2（maintainer 確認不支援）| https://discuss.pynq.io/t/xrt-on-pynq-z2/2950 |
| libcma.so in C on PYNQ-Z2（社群確認）| https://discuss.pynq.io/t/a-problem-on-libcma-so-in-c/959 |
