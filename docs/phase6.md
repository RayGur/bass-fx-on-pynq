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

### 3.3 PIPELINE II=1 限制（已觸發，已修正）

`hls::stream` 是單端口 FIFO，每個 clock 最多一次 read 或 write。  
原設計每次 iteration 做 2×read + 2×write → HLS 強制 II=2 → **R channel 完全不寫 DDR**（見 D24）。  
**已改為 per-sample loop**（`n_samples * 2` 次，每次 1 read + 1 write）：

```cpp
// per-sample loop：n_samples*2 iters，每次 1 read + 1 write → II=1
for (int i = 0; i < n_samples * 2; i++) {
#pragma HLS PIPELINE II=1
    audio_pkt_t pkt = s_in.read();
    sample_t in_s, out_s;
    in_s.range(23, 0) = pkt.data.range(23, 0);
    // ...處理後寫出
    audio_pkt_t out_pkt;
    out_pkt.data.range(23, 0) = out_s.range(23, 0);
    out_pkt.keep = ~0;  out_pkt.strb = ~0;
    out_pkt.last = (i == n_samples * 2 - 1) ? 1 : 0;
    s_out.write(out_pkt);
}
```

> **已驗（D24）**：原 2×read+2×write per iter → HLS 強制 II=2 → R channel 所有 sample 不寫 DDR。  
> Per-sample loop 為 Xilinx HLS + DMA 的 canonical pattern，詳見 §10 D24 節。

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
| **TKEEP=0（本次根因）** | S2MM IOC 正常 fire、LEN=0，但 out_buf 永遠不更新，無 AXI error | HLS `ap_axis` 輸出 packet 必須顯式設 `.keep = ~0`；見 §10 |

---

## 10. 上板除錯紀錄（Step C / D 期間）

> 本節記錄 2026-06-13 的完整試錯過程，供日後參考。

### 10.1 症狀

執行 `sudo ./audio_dma` 後：
- DMA MM2S IOC 正常觸發（`MM2S_SR=0x00001002`，LEN remaining=0）
- DMA S2MM IOC 正常觸發（`S2MM_SR=0x00001002`，dec=0 slv=0 int=0，LEN remaining=0）
- Effect IP `EFFECT_CTRL=0x81`（ap_start=1 auto_restart=1，ap_idle=0，ap_done=0）
- **但 `out_buf[0][0]` 從頭到尾都等於 sentinel 值，DMA 沒有寫入 DDR**
- 耳機接上時爆音（codec TX 收到未初始化的 out_buf）

### 10.2 試錯過程與排除

#### T1：Cache coherency（cacheable=1 + flush/invalidate）

**假設**：out_buf 是 cacheable，DMA 寫入 DDR 但 CPU 讀到 cache stale value。  
**測試**：改用 `cma_alloc(size, 0)`（non-cacheable），移除 flush/invalidate。  
**結果**：symptom 不變；後來加入 `/dev/mem` cross-check 確認 DDR 內容本身就等於 sentinel → 不是 cache 問題。  
**排除**：確認。

#### T2：Physical address 錯誤（cma_get_phy_addr 回傳不對）

**假設**：`cma_get_phy_addr()` 回傳的 phys addr 與 CPU virt addr 對應的不是同一塊 DDR。  
**測試**：加入雙向 phys-verify：
```
[phys-verify] wrote 0xDEADBEEF via virt → /dev/mem@0x1804a000 reads 0xdeadbeef  ✓
[phys-verify] wrote 0xCAFEBABE via /dev/mem → virt reads 0xcafebabe  ✓
```
**結果**：兩個方向都對得上。  
**排除**：確認。

#### T3：HP0 data width 不匹配（64-bit vs 32-bit）

**假設**：DMA M_AXI 是 32-bit，HP0 設定 64-bit，axi_mem_intercon 做 width conversion 時 WSTRB 位移導致寫入失敗。  
**測試**：在 Vivado PS7 customization 把 S_AXI_HP0_DATA_WIDTH 從 64 改為 32，重新 Generate Bitstream。  
**結果**：symptom 完全相同，無改善。  
**排除**：確認（HP0 32-bit 改法是對的，但不是根因）。

#### T4：BD 未包含 axis_data_fifo_1（S2MM 路徑斷線）

**假設**：DMA S_AXIS_S2MM 沒有接到 process_sample_0/s_out，S2MM 永遠收不到資料。  
**調查方法**：從 Vivado TCL Console 執行 `write_bd_tcl ~/bass_fx_bd_export.tcl`，比對連線。  
**第一次 TCL（舊版）**：的確沒有 axis_data_fifo_1、也沒有 s_out 的接線——但那是舊版匯出，不反映當時的 Vivado 專案狀態。  
**第二次 TCL（重新匯出）**：確認 `axis_data_fifo_1` 存在，`s_out → fifo_1 → S_AXIS_S2MM` 完整接線。  
**排除**：BD 連線本身正確，不是根因。  
**副作用**：第一次 TCL 也讓人懷疑 `c_include_sg` 可能是 1（SG mode），因為舊 TCL 有 `M_AXI_SG` 連線。重新匯出後確認 `c_include_sg {0}`，SG mode 理論排除。

#### T5：AXI-Lite 位址不符（n_samples 沒寫到正確 offset）

**假設**：`EFFECT_N_SAMPLES = 0x10` 等 define 和 HLS 產出的實際 offset 不符，IP 用到錯誤的 n_samples。  
**調查**：讀取 `hls/effect_ip/.../xprocess_sample_hw.h`，確認 HLS 產出的 offset 完全與 audio_dma.c 相符。  
**加入 readback 診斷**：
```
[init] effect readback: n_samples=256 dist_en=0 CTRL=0x00000081
```
n_samples 確認正確。  
**排除**：確認。

#### T6：Effect IP 從未執行（永遠卡在 s_in.read()）

**假設**：AP_START 設了但 IP 沒有處理任何資料，s_out/fifo_1 空的，S2MM 等不到 TVALID 永遠不完成。  
**矛盾**：S2MM IOC 已 fire 且 LEN=0，若 fifo_1 空的 S2MM 應該 hang，不可能在有限時間內完成。  
**結論**：IP 確實有在產出資料到 fifo_1（否則 S2MM 不會完成），但這些資料寫進 DDR 時沒有效果。  
**排除（部分）**：IP 有輸出，但輸出到 DDR 的過程出問題。

### 10.3 根因：TKEEP = 0（ap_axis .keep 未初始化）

**觸發因子**：`process_sample.cpp` 的輸出 packet 只設了 `.data` 和 `.last`，**沒有設 `.keep`**：

```cpp
// 原始有問題的程式碼
audio_pkt_t out_l_pkt, out_r_pkt;
out_l_pkt.data = 0;
out_l_pkt.data.range(23, 0) = out_l.range(23, 0);
out_l_pkt.last = 0;   // .keep 沒設 → HLS 合成預設 0
```

**因果鏈**：
```
.keep 未設 → HLS RTL 合成 TKEEP = 0
  → AXI DMA S2MM 收到 TKEEP=0
  → S2MM 把 TKEEP 對應到 AXI4 WSTRB=0（Byte Enable 全關）
  → HP0 收到 WSTRB=0 的寫入事務 → 接受事務（BRESP=OKAY，無 error）但不更新 DDR
  → S2MM IOC fire（事務完成），LEN=0，dec=0 slv=0 int=0（全部正常）
  → 但 DDR 內容不變（sentinel 永遠是 0x12345678）
```

**確認依據**：
- `/dev/mem` cross-check：`/dev/mem@out_phys reads 0x12345678`（DDR 本身就沒被寫）
- AXI 協議：WSTRB=0 是合法事務，HP0 正確回覆 OKAY 但不寫 byte
- 無 AXI error（dec/slv/int 全 0）完全符合此場景
- C sim testbench 不會發現此 bug（simulation 直接讀 `.data`，不過 DMA 硬體邏輯）

### 10.4 修正

**`process_sample.cpp`**（已更新）：

```cpp
// 修正後
out_l_pkt.keep = ~0;   // 全部 byte enable 有效（TKEEP=0xF for 32-bit）
out_r_pkt.keep = ~0;
```

### 10.5 根因 2：II=2 → R channel 不寫 DDR（D24）

**觸發因子**：TKEEP fix（D23）後板測，左聲道 passthrough 正常，右聲道仍爆音。  
`out_buf` 模式：`[L0_correct, 0x12345678, L1_correct, 0x12345678, ...]`（奇數索引永遠是 sentinel）。

**根因**：`hls::stream` 是單端口 FIFO（UG1448），每個 clock cycle 最多 1 次 read **或** 1 次 write。  
原 loop 每個 iteration 做 `s_in.read()×2 + s_out.write()×2` → HLS 強制 II=2 → S2MM 在 II=2 的節奏下只看到 L sample 佔用的寫入槽，R sample 的 write 被 schedule 到 S2MM 不接受的 cycle → R words 完全不落 DDR。

**因果鏈**：
```
原 loop: [read_L, read_R, write_L, write_R] per iter
  → hls::stream 單端口 FIFO 衝突 → HLS 強制 II=2
  → 每 2 cycles 只完成 1 word 有效寫入
  → S2MM store-and-forward：R channel write 被 AXI throttle 跳過
  → out_buf 奇數位置 = sentinel（未被寫入）
```

**修正（已入 `process_sample.cpp`）**：
```cpp
// n_samples*2 iterations，每次 1 read + 1 write → II=1
for (int i = 0; i < n_samples * 2; i++) {
#pragma HLS PIPELINE II=1
    audio_pkt_t pkt = s_in.read();
    // ... process ...
    s_out.write(out_pkt);
}
```

**n_samples 語義不變**：PS 仍寫 256（stereo pairs），DMA buffer 仍 2048 bytes。  
`process_sample_core()` 未動（only used by testbench，testbench 不過 DMA 硬體路徑）。

**待驗**：HLS 重新合成 → Vivado Refresh IP → Generate Bitstream → 板上確認 R channel 正常。

### 10.6 根因 3：HP0 data width 32-bit → S2MM 8-byte stride，每隔一個 word 不寫 DDR（D25）

**觸發因子**：D24 確認 II=1 後，`dma_test.py` 獨立驗證仍出現：
```
out_buf[:8] = [0, 0xAAAAAAAA, 200, 0xAAAAAAAA, 400, ...]
total written=256  sentinel=256  (expected written=512)
```

**關鍵線索**：written 值是 `in_buf[0], in_buf[2], in_buf[4]`...（8-byte stride，不是 4-byte），確認 DMA 每次以 64-bit beat 寫入 HP0，WSTRB 只蓋低 32-bit，高 32-bit 跳過。

**調查過程**：
- 讀 HLS RTL（`process_sample_Pipeline_VITIS_LOOP_62_1.v`、`process_sample.v`）：TKEEP hardwired `4'd15`，TVALID 對全部 iteration 皆 fire。RTL 清潔，排除 HLS 層問題。
- D23 / D24 皆已驗證修正，排除。
- cache / FIFO / 位址：各方向驗證均排除。

**根本原因**：Zynq HP0 內部匯流排實際上是 64-bit，`PCW_S_AXI_HP0_DATA_WIDTH=32` 僅改變 HWH 記錄，不改變底層硬體寬度。AXI Interconnect → HP0 以 64-bit beat 傳輸，WSTRB=0x0F（只有低 4 bytes），造成每 8 bytes 只寫一個 32-bit slot。

**修正**：Vivado PS7 Customization → HP Slave AXI Interface → S AXI HP0 Interface Data Width 改為 **64** → Validate Design → Generate Bitstream（重新 build，不沿用舊 hp64 bitstream）。

**驗證（板上 dma_test.py 輸出）**：
```
effect CTRL=0x81  n_samples=256
in  phys=0x18075000  out phys=0x18076000
post-reset SR: MM2S=0x0 S2MM=0x0
S2MM SR=0x1002  LEN_rem=2048
  halted=0 idle=1 int_err=0 slv_err=0 dec_err=0 ioc=1
in_buf [:8] = [0, 100, 200, 300, 400, 500, 600, 700]
out_buf[:8] = [0, 100, 200, 300, 400, 500, 600, 700]
even positions match input (L-ch): True
odd  positions all sentinel  (R-ch): False
total written=512  sentinel=0  (expected written=512)
```

全部 512 個 word 正確寫入，DMA pipeline **完全通過**。`LEN_rem=2048` 是 DMA direct-register mode 正常行為（LEN register 讀回 programmed value），可忽略。

### 10.7 診斷工具（本次開發，保留在 audio_dma.c）

目前 `ps/audio_dma.c` 包含下列診斷，待 DMA 驗證通過後可選擇移除：
- `[phys-verify]`：雙向 virt↔/dev/mem 驗證實體位址
- `[init] effect readback`：確認 AXI-Lite 寫入有效
- `[diag:before-boot]` / `[diag:after-boot-dma]`：DMA SR + Effect CTRL 狀態
- `[dma] S2MM_DA readback` / `MM2S_SA readback`：確認 DA/SA 暫存器正確
- `[boot] /dev/mem@...`：確認 DDR 實際寫入狀態（T1/T6 關鍵診斷）

---

## 8. 實作 Checklist

### Step A — HLS 修改
- [x] `process_sample.cpp` 改成 stream top function（外殼 loop）
- [x] `effect_ip.h` 新增 `process_sample_core()` 宣告
- [x] 原 `process_sample()` 邏輯移入 `process_sample_core()`
- [x] HLS C Simulation PASS（stream testbench，驗 TLAST + output 正確）
- [x] RTL Synthesis PASS（確認 II=1，LUT/DSP 用量合理）
- [x] 確認 synthesis report 中 static state 保留
- [x] Export IP
- [x] **BUG FIX D23：output packet `.keep = ~0`（TKEEP=0 導致 S2MM 寫入 DDR 失敗，見 §10.3）**
- [x] **BUG FIX D24：per-sample loop（n_samples×2 iters，1 read+1 write）→ II=1，修正 R channel 不寫 DDR（見 §10.5）**
- [x] **重新 HLS C Sim + RTL Synthesis（含 D24 loop 修正）→ 確認 II=1（Final II=1, Depth=5）→ Export IP**

### Step B — Vivado Block Design
- [x] 加入 AXI DMA IP（MM2S + S2MM，c_include_sg=0）
- [x] 加入 2 × AXI Stream FIFO（axis_data_fifo_0 / _1，depth=512）
- [x] 加入 Concat IP，接 IRQ_F2P[0]
- [x] **BUG FIX D25：HP0 data width 改為 64-bit**（Zynq HP0 內部 64-bit，設 32-bit 導致 WSTRB=0x0F per 64-bit beat，每隔一個 word 不寫 DDR；見 §10.6）
- [x] axi_mem_intercon NUM_SI=2（MM2S + S2MM，無 SG port）
- [x] Address Editor 確認（DMA=0x41E00000，Effect=0x40020000）
- [x] **Generate Bitstream 完成（含 D24 + D25 fix）**

### Step C — PS C 程式
- [x] 確認板上 `libcma.so` 存在
- [x] 實作 `audio_dma.c`（Combined TX/RX codec loop + DMA ping-pong，non-cacheable buffer）
- [x] 板上編譯：`gcc audio_dma.c -I/usr/include -L/usr/lib -lcma -O2 -o audio_dma`
- [x] 加入完整診斷（phys-verify、effect readback、DMA SR、/dev/mem cross-check）
- [x] **DMA pipeline 驗證通過（dma_test.py：total written=512，sentinel=0，見 §10.6）**
- [ ] Ping-pong 連續音訊迴圈測試（確認無 glitch）

### Step D — 上板整合驗證
- [x] 確認 passthrough 音訊正常（dist_en=0）
- [x] 確認 distortion 效果正常（dist_en=1，threshold/gain 調整）
- [x] 確認無 click / glitch（IIR state 跨 buffer 正常）
- [x] 與 Phase 2 Python PIO 聽感比較，確認音質改善
- [x] **Phase 6 Exit Criteria 確認**

### 後續優化（wobble merge + MMIO 串接完畢後處理）
- [ ] **Distortion 高 gain 雜訊問題**：gain=20 時底噪被放大明顯，演奏時可聽到背景雜訊。原因：hard clipping 在放大後才 clip，低電平的底噪也被等比放大。對策選項：(A) 加入 noise gate（signal < 門檻時靜音）；(B) 限制 max gain（改為 12 或 15）；(C) 先 clip 再 gain（soft clipping 架構）。優先考量 A 或 B，成本低。

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
