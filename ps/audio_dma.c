// audio_dma.c — Phase 6 C + DMA audio loop
//
// Pipeline per 256-sample buffer:
//   1. Combined TX/RX loop (one LRCLK per sample, ~5.33 ms):
//      output out_buf[cur] to codec, capture new audio into in_buf[nxt]
//   2. Flush + DMA transfer (~5 μs, negligible)
//   3. Invalidate, swap cur/nxt
//
// Compile on board:
//   gcc audio_dma.c -I/usr/include -L/usr/lib -lcma -O2 -o audio_dma
//   sudo ./audio_dma
//
// Base addresses from INTERFACE.md (Phase 6 BD):
//   Effect IP : 0x40020000
//   AXI DMA   : 0x41E00000
//   Codec ctrl: 0x44A00000

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <libxlnk_cma.h>

// ── Base addresses ────────────────────────────────────────────
#define CODEC_BASE   0x44A00000UL
#define DMA_BASE     0x41E00000UL
#define EFFECT_BASE  0x40020000UL
#define GPIO0_BASE   0x40000000UL  // sw[1:0] ch1, btn[3:0] ch2 (input)
#define GPIO1_BASE   0x40010000UL  // led[3:0] (output)
#define GPIO2_BASE   0x40030000UL  // rgbleds[5:0] LD4+LD5 (output, Phase 4)
#define MAP_SIZE     0x10000UL

// ── audio_codec_ctrl registers (offset from CODEC_BASE) ──────
#define CODEC_RX_L 0x00   // R: left  received sample, bits[23:0] = Q1.23
#define CODEC_RX_R 0x04   // R: right received sample
#define CODEC_TX_L 0x08   // W: left  transmit sample
#define CODEC_TX_R 0x0C   // W: right transmit sample
#define CODEC_STATUS 0x10 // RW: bit0=data_rdy; write any value to clear

// ── AXI DMA Direct Register Mode offsets ─────────────────────
#define DMA_MM2S_CR 0x00     // MM2S Control
#define DMA_MM2S_SR 0x04     // MM2S Status
#define DMA_MM2S_SA 0x18     // MM2S Source Address
#define DMA_MM2S_LEN 0x28    // MM2S Transfer Length (bytes)
#define DMA_S2MM_CR 0x30     // S2MM Control
#define DMA_S2MM_SR 0x34     // S2MM Status
#define DMA_S2MM_DA 0x48     // S2MM Destination Address
#define DMA_S2MM_LEN 0x58    // S2MM Transfer Length (bytes)
#define DMA_CR_RS (1 << 0)   // Run/Stop
#define DMA_SR_IOC (1 << 12) // IOC_Irq: transfer complete

// ── AXI GPIO register offsets (same for gpio0/1/2) ───────────
#define GPIO_DATA  0x000  // ch1 data
#define GPIO_TRI   0x004  // ch1 direction (0=output, 1=input per bit)
#define GPIO_DATA2 0x008  // ch2 data
#define GPIO_TRI2  0x00C  // ch2 direction

// RGB LED bit masks (gpio2)
// bits[2:0] → LD4 (B=L15/G=G17/R=N15); bits[5:3] → LD5 (B=G14/G=L14/R=M15)
// Use green channel only (bit1=LD4G, bit4=LD5G) to reduce brightness
#define RGBLED_LD4 0x02u
#define RGBLED_LD5 0x10u

// ── Effect IP (process_sample) AXI-Lite offsets ──────────────
#define EFFECT_CTRL      0x00 // bit0=AP_START, bit7=AUTO_RESTART
#define EFFECT_N_SAMPLES 0x10
#define EFFECT_DIST_EN   0x18
#define EFFECT_WOBBLE_EN 0x20
#define EFFECT_THRESHOLD 0x28 // Q1.23 int: int(clip_float * (1<<23))
#define EFFECT_GAIN      0x30 // integer 1–20
#define EFFECT_LFO_RATE  0x38 // phase increment per L-sample (2^32 = one full LFO cycle)
#define EFFECT_LFO_DEPTH 0x40 // 0–100 (maps LFO sweep to B_LUT index range)
#define EFFECT_LFO_FLOOR 0x48 // 0–15  (minimum B_LUT index; wah depth preset)

// ── Effect preset parameter tables (Phase 4) ─────────────────
// Distortion low: gentle clip (threshold 0.5, gain 4)
// Distortion high: heavy clip (threshold 0.2, gain 12)
// Wobble slow: 1 Hz sweep; fast: 4 Hz sweep
// Adjust after board listening test; update docs/INTERFACE.md when changed.
#define DIST_THRESHOLD_LOW  ((int)(0.5f * (1 << 23)))  // 4194304
#define DIST_GAIN_LOW       4
#define DIST_THRESHOLD_HIGH ((int)(0.2f * (1 << 23)))  // 1677722
#define DIST_GAIN_HIGH      12
#define WOBBLE_RATE_SLOW    89478   // 1 Hz  (= 1 * 2^32 / 48000)
#define WOBBLE_DEPTH_SLOW   80
#define WOBBLE_RATE_FAST    357914  // 4 Hz  (= 4 * 2^32 / 48000)
#define WOBBLE_DEPTH_FAST   100

// Wah depth presets (btn2 cycles A→B→C, 14.1)
// lfo_floor = minimum B_LUT index; sets the trough cutoff frequency
// A: floor=6 → fc≈83 Hz trough, -6 dB at 80 Hz (audible, default)
// B: floor=4 → fc≈41 Hz trough, -18 dB at 80 Hz (very dark)
// C: floor=0 → fc=10 Hz trough, -36 dB at 80 Hz (near-silent)
#define WAH_FLOOR_A 6
#define WAH_FLOOR_B 4
#define WAH_FLOOR_C 0
static const int wah_floor_values[3] = { WAH_FLOOR_A, WAH_FLOOR_B, WAH_FLOOR_C };
static const char *wah_floor_names[3] = { "A(fc≈83Hz)", "B(fc≈41Hz)", "C(fc=10Hz)" };

// ── Buffer config (non-cacheable = cache coherent with DMA) ──
#define N_SAMPLES 255
#define N_WORDS (N_SAMPLES * 2) // L then R, interleaved
#define BUF_BYTES (N_WORDS * 4) // 2048 bytes per buffer

// ── Globals ───────────────────────────────────────────────────
static volatile uint32_t *codec;
static volatile uint32_t *dma;
static volatile uint32_t *effect;
static volatile uint32_t *gpio0;  // sw + btn input
static volatile uint32_t *gpio1;  // led[3:0] output
static volatile uint32_t *gpio2;  // rgbleds[5:0] output

// ── Control state (Phase 4) ───────────────────────────────────
static int dist_preset      = 0;  // 0=low, 1=high
static int wobble_preset    = 0;  // 0=slow, 1=fast
static int wah_depth_preset = 0;  // 0=A, 1=B, 2=C (btn2 cycles)
static uint32_t prev_sw     = 0xFF; // force first write on boot

#define DEBOUNCE_COUNT 3  // consecutive polls needed ≈ 3 × 5.33 ms = 16 ms
static struct { int count; int fired; } btn_db[3]; // [0]=dist, [1]=wobble, [2]=wah_depth

static int32_t *in_buf[2], *out_buf[2];
static uint32_t in_phys[2], out_phys[2];

// ── MMIO helpers ──────────────────────────────────────────────
#define REG_R(base, off) ((base)[(off) / 4])
#define REG_W(base, off, val) ((base)[(off) / 4] = (uint32_t)(val))

// ── mmap a peripheral ─────────────────────────────────────────
static volatile uint32_t *mmap_periph(int fd, uint32_t base_addr)
{
    void *p = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, (off_t)base_addr);
    if (p == MAP_FAILED)
    {
        perror("mmap");
        exit(1);
    }
    return (volatile uint32_t *)p;
}

// ── CMA buffer allocation ─────────────────────────────────────
static void init_buffers(void)
{
    for (int i = 0; i < 2; i++)
    {
        in_buf[i] = cma_alloc(BUF_BYTES, 0); // cacheable=0: no flush/invalidate needed
        out_buf[i] = cma_alloc(BUF_BYTES, 0);
        if (!in_buf[i] || !out_buf[i])
        {
            fprintf(stderr, "cma_alloc failed\n");
            exit(1);
        }
        in_phys[i] = cma_get_phy_addr(in_buf[i]);
        out_phys[i] = cma_get_phy_addr(out_buf[i]);
        printf("[init] buf[%d]: in_phys=0x%08x  out_phys=0x%08x\n",
               i, in_phys[i], out_phys[i]);
    }
}

// ── Diagnostics ───────────────────────────────────────────────
static void diag(const char *tag)
{
    uint32_t mm2s_sr = REG_R(dma, DMA_MM2S_SR);
    uint32_t s2mm_sr = REG_R(dma, DMA_S2MM_SR);
    uint32_t eff_ctrl = REG_R(effect, EFFECT_CTRL);
    uint32_t eff_nsamp = REG_R(effect, EFFECT_N_SAMPLES);
    printf("[diag:%s]\n", tag);
    printf("  MM2S_SR=0x%08x  (halted=%d idle=%d ioc=%d)\n",
           mm2s_sr, mm2s_sr & 1, (mm2s_sr >> 1) & 1, (mm2s_sr >> 12) & 1);
    printf("  S2MM_SR=0x%08x  (halted=%d idle=%d ioc=%d)\n",
           s2mm_sr, s2mm_sr & 1, (s2mm_sr >> 1) & 1, (s2mm_sr >> 12) & 1);
    printf("  EFFECT_CTRL=0x%08x  n_samples=%u\n", eff_ctrl, eff_nsamp);
}

static void diag_buf(const char *tag, int32_t *buf)
{
    printf("[diag:%s] buf[0..3] = %d %d %d %d\n",
           tag, buf[0], buf[1], buf[2], buf[3]);
}

// ── Phase 4: preset application ──────────────────────────────
static void apply_dist_preset(void)
{
    if (dist_preset) {
        REG_W(effect, EFFECT_THRESHOLD, DIST_THRESHOLD_HIGH);
        REG_W(effect, EFFECT_GAIN,      DIST_GAIN_HIGH);
    } else {
        REG_W(effect, EFFECT_THRESHOLD, DIST_THRESHOLD_LOW);
        REG_W(effect, EFFECT_GAIN,      DIST_GAIN_LOW);
    }
}

static void apply_wobble_preset(void)
{
    if (wobble_preset) {
        REG_W(effect, EFFECT_LFO_RATE,  WOBBLE_RATE_FAST);
        REG_W(effect, EFFECT_LFO_DEPTH, WOBBLE_DEPTH_FAST);
    } else {
        REG_W(effect, EFFECT_LFO_RATE,  WOBBLE_RATE_SLOW);
        REG_W(effect, EFFECT_LFO_DEPTH, WOBBLE_DEPTH_SLOW);
    }
}

static void apply_wah_depth_preset(void)
{
    REG_W(effect, EFFECT_LFO_FLOOR, wah_floor_values[wah_depth_preset]);
}

static void update_leds(uint32_t sw)
{
    // led[0] = dist_preset, led[1] = wobble_preset
    REG_W(gpio1, GPIO_DATA, (uint32_t)((dist_preset & 1) | ((wobble_preset & 1) << 1)));
    // LD4 (bits[2:0]) = sw[0], LD5 (bits[5:3]) = sw[1]
    uint32_t rgb = ((sw & 1) ? RGBLED_LD4 : 0u) | ((sw >> 1) & 1 ? RGBLED_LD5 : 0u);
    REG_W(gpio2, GPIO_DATA, rgb);
}

// Called once per audio buffer (~5.33 ms).
// Reads sw/btn, updates Effect IP and LEDs.
static void control_poll(void)
{
    uint32_t sw  = REG_R(gpio0, GPIO_DATA)  & 0x3u; // sw[1:0]
    uint32_t btn = REG_R(gpio0, GPIO_DATA2) & 0x7u; // btn[2:0]

    // Switches → dist_en / wobble_en (write only on change)
    if (sw != prev_sw) {
        REG_W(effect, EFFECT_DIST_EN,   sw & 1u);
        REG_W(effect, EFFECT_WOBBLE_EN, (sw >> 1) & 1u);
        prev_sw = sw;
    }

    // Button[0]: distortion preset toggle
    if (btn & 1u) {
        if (!btn_db[0].fired && ++btn_db[0].count >= DEBOUNCE_COUNT) {
            dist_preset ^= 1;
            apply_dist_preset();
            btn_db[0].fired = 1;
            printf("[ctrl] dist preset → %s  (thr=%d gain=%d)\n",
                   dist_preset ? "HIGH" : "LOW",
                   dist_preset ? DIST_THRESHOLD_HIGH : DIST_THRESHOLD_LOW,
                   dist_preset ? DIST_GAIN_HIGH : DIST_GAIN_LOW);
        }
    } else {
        btn_db[0].count = 0;
        btn_db[0].fired = 0;
    }

    // Button[1]: wobble preset toggle
    if ((btn >> 1) & 1u) {
        if (!btn_db[1].fired && ++btn_db[1].count >= DEBOUNCE_COUNT) {
            wobble_preset ^= 1;
            apply_wobble_preset();
            btn_db[1].fired = 1;
            printf("[ctrl] wobble preset → %s  (rate=%d depth=%d)\n",
                   wobble_preset ? "FAST" : "SLOW",
                   wobble_preset ? WOBBLE_RATE_FAST  : WOBBLE_RATE_SLOW,
                   wobble_preset ? WOBBLE_DEPTH_FAST : WOBBLE_DEPTH_SLOW);
        }
    } else {
        btn_db[1].count = 0;
        btn_db[1].fired = 0;
    }

    // Button[2]: wah depth preset cycle A→B→C→A
    if ((btn >> 2) & 1u) {
        if (!btn_db[2].fired && ++btn_db[2].count >= DEBOUNCE_COUNT) {
            wah_depth_preset = (wah_depth_preset + 1) % 3;
            apply_wah_depth_preset();
            btn_db[2].fired = 1;
            printf("[ctrl] wah depth → %s  (lfo_floor=%d)\n",
                   wah_floor_names[wah_depth_preset],
                   wah_floor_values[wah_depth_preset]);
        }
    } else {
        btn_db[2].count = 0;
        btn_db[2].fired = 0;
    }

    update_leds(sw);
}

// ── Effect IP init ────────────────────────────────────────────
static void effect_init(void)
{
    // Phase 4: read actual switch state to initialise enables correctly
    uint32_t sw = REG_R(gpio0, GPIO_DATA) & 0x3u;
    prev_sw = sw;

    REG_W(effect, EFFECT_N_SAMPLES, N_SAMPLES);
    REG_W(effect, EFFECT_DIST_EN,   sw & 1u);
    REG_W(effect, EFFECT_WOBBLE_EN, (sw >> 1) & 1u);

    // Write preset parameters (dist_preset=0=low, wobble_preset=0=slow, wah_depth=0=A)
    apply_dist_preset();
    apply_wobble_preset();
    apply_wah_depth_preset();

    // AP_START | AUTO_RESTART: IP restarts after each stream transfer
    REG_W(effect, EFFECT_CTRL, (1 << 7) | (1 << 0));

    // Readback: confirm AXI-Lite writes reached the IP
    uint32_t rb_n   = REG_R(effect, EFFECT_N_SAMPLES);
    uint32_t rb_dis = REG_R(effect, EFFECT_DIST_EN);
    uint32_t rb_ctl = REG_R(effect, EFFECT_CTRL);
    printf("[init] effect IP: dist_en=%u wobble_en=%u "
           "threshold=%d gain=%d lfo_rate=%d lfo_depth=%d\n",
           sw & 1u, (sw >> 1) & 1u,
           DIST_THRESHOLD_LOW, DIST_GAIN_LOW,
           WOBBLE_RATE_SLOW, WOBBLE_DEPTH_SLOW);
    printf("[init] effect readback: n_samples=%u dist_en=%u CTRL=0x%08x"
           "  (ap_start=%d auto_rst=%d)\n",
           rb_n, rb_dis, rb_ctl, rb_ctl & 1, (rb_ctl >> 7) & 1);
    if (rb_n != N_SAMPLES)
        printf("[WARN] n_samples readback mismatch! wrote %d got %u\n",
               N_SAMPLES, rb_n);
}

// ── GPIO init (Phase 4) ───────────────────────────────────────
static void gpio_init(void)
{
    // gpio0: sw(ch1) + btn(ch2) — all input (TRI=1)
    REG_W(gpio0, GPIO_TRI,  0xFFu);
    REG_W(gpio0, GPIO_TRI2, 0xFFu);
    // gpio1: led[3:0] — all output (TRI=0); start with all off
    REG_W(gpio1, GPIO_TRI,  0x0u);
    REG_W(gpio1, GPIO_DATA, 0x0u);
    // gpio2: rgbleds[5:0] — all output (TRI=0); start with all off
    REG_W(gpio2, GPIO_TRI,  0x0u);
    REG_W(gpio2, GPIO_DATA, 0x0u);
    printf("[gpio_init] sw=0x%x btn=0x%x\n",
           REG_R(gpio0, GPIO_DATA) & 0x3u,
           REG_R(gpio0, GPIO_DATA2) & 0xFu);
}

// ── DMA init (hard reset + run both channels) ────────────────
// PYNQ overlay may leave DMA in SG-running state; reset first.
static void dma_init(void)
{
    REG_W(dma, DMA_MM2S_CR, 1 << 2); // MM2S reset
    REG_W(dma, DMA_S2MM_CR, 1 << 2); // S2MM reset
    while (REG_R(dma, DMA_MM2S_CR) & (1 << 2))
        ; // wait auto-clear
    while (REG_R(dma, DMA_S2MM_CR) & (1 << 2))
        ;
    REG_W(dma, DMA_MM2S_CR, DMA_CR_RS);
    REG_W(dma, DMA_S2MM_CR, DMA_CR_RS);
    printf("[dma_init] post-reset: MM2S_SR=0x%08x  S2MM_SR=0x%08x\n",
           REG_R(dma, DMA_MM2S_SR), REG_R(dma, DMA_S2MM_SR));
}

// ── Single DMA transfer (blocking) ───────────────────────────
// S2MM first so receiver is ready before sender fires.
static int dma_first = 1;
static void dma_transfer(uint32_t src_phys, uint32_t dst_phys)
{
    REG_W(dma, DMA_S2MM_DA, dst_phys);
    if (dma_first)
        printf("[dma] S2MM_DA written=0x%08x readback=0x%08x\n",
               dst_phys, REG_R(dma, DMA_S2MM_DA));
    REG_W(dma, DMA_S2MM_LEN, BUF_BYTES); // triggers S2MM

    REG_W(dma, DMA_MM2S_SA, src_phys);
    if (dma_first)
        printf("[dma] MM2S_SA written=0x%08x readback=0x%08x\n",
               src_phys, REG_R(dma, DMA_MM2S_SA));
    REG_W(dma, DMA_MM2S_LEN, BUF_BYTES); // triggers MM2S

    uint32_t mm2s_sr, s2mm_sr;
    while (!((mm2s_sr = REG_R(dma, DMA_MM2S_SR)) & DMA_SR_IOC))
        ;
    if (dma_first)
        printf("[dma] MM2S IOC: SR=0x%08x\n", mm2s_sr);
    REG_W(dma, DMA_MM2S_SR, DMA_SR_IOC);

    while (!((s2mm_sr = REG_R(dma, DMA_S2MM_SR)) & DMA_SR_IOC))
        ;
    if (dma_first)
    {
        printf("[dma] S2MM IOC: SR=0x%08x  (dec=%d slv=%d int=%d)\n",
               s2mm_sr, (s2mm_sr >> 6) & 1, (s2mm_sr >> 5) & 1, (s2mm_sr >> 4) & 1);
        printf("[dma] S2MM_LEN remaining=%u  (0=all xfr'd, 2048=nothing xfr'd)\n",
               REG_R(dma, DMA_S2MM_LEN));
        dma_first = 0;
    }
    REG_W(dma, DMA_S2MM_SR, DMA_SR_IOC);
}

// ── Main audio loop ───────────────────────────────────────────
static void audio_loop(void)
{
    int cur = 0;

    // Bootstrap: fill buf[0] with live audio, process first DMA pass
    // (first 256 output samples will be silence — one buffer latency)
    diag("before-boot");

    printf("[boot] filling first input buffer...\n");
    for (int i = 0; i < N_SAMPLES; i++)
    {
        while (!(REG_R(codec, CODEC_STATUS) & 1))
            ;
        in_buf[0][i * 2] = (int32_t)REG_R(codec, CODEC_RX_L);
        in_buf[0][i * 2 + 1] = (int32_t)REG_R(codec, CODEC_RX_R);
        REG_W(codec, CODEC_STATUS, 0);
    }
    diag_buf("in_buf[0]", in_buf[0]);

    // Sentinel: fill out_buf[0] with 0x12345678 to detect whether S2MM writes to it
    for (int s = 0; s < N_WORDS; s++)
        out_buf[0][s] = (int32_t)0x12345678;
    printf("[boot] sentinel set: out_buf[0][0]=0x%08x\n", (uint32_t)out_buf[0][0]);

    dma_transfer(in_phys[0], out_phys[0]);

    diag("after-boot-dma");
    // Check EFFECT_CTRL AP_IDLE (bit2) and AP_DONE (bit1)
    {
        uint32_t ec = REG_R(effect, EFFECT_CTRL);
        printf("[boot] EFFECT_CTRL=0x%08x  ap_start=%d ap_done=%d ap_idle=%d ap_ready=%d\n",
               ec, ec & 1, (ec >> 1) & 1, (ec >> 2) & 1, (ec >> 3) & 1);
    }
    printf("[boot] out_buf[0][0]=0x%08x  (0x12345678=not written, other=DMA wrote)\n",
           (uint32_t)out_buf[0][0]);
    diag_buf("out_buf[0]", out_buf[0]);

    // Cross-check: read out_phys[0] via /dev/mem to distinguish
    //   "DMA didn't write" vs "DMA wrote but CPU read stale cache"
    {
        int fd3 = open("/dev/mem", O_RDWR | O_SYNC);
        uint32_t poff = out_phys[0] & 0xFFF;
        volatile uint32_t *pm3 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd3, out_phys[0] & ~0xFFF);
        uint32_t via_devmem = pm3[poff / 4];
        printf("[boot] /dev/mem@0x%08x reads 0x%08x  "
               "(0x12345678=DMA truly didn't write; other=cache stale)\n",
               out_phys[0], via_devmem);
        munmap((void *)pm3, 4096);
        close(fd3);
    }

    printf("[boot] entering audio loop (Ctrl+C to stop)\n");

    while (1)
    {
        int nxt = 1 - cur;

        // ── Combined TX + RX: one LRCLK period per iteration ─
        // Output processed audio (out_buf[cur]) while capturing
        // next input audio (in_buf[nxt]).
        // data_rdy fires at 48 kHz; this loop runs ~5.33 ms.
        for (int i = 0; i < N_SAMPLES; i++)
        {
            while (!(REG_R(codec, CODEC_STATUS) & 1))
                ;

            // TX: write processed sample to codec DAC
            REG_W(codec, CODEC_TX_L, (uint32_t)out_buf[cur][i * 2]);
            REG_W(codec, CODEC_TX_R, (uint32_t)out_buf[cur][i * 2 + 1]);

            // RX: capture new sample from codec ADC
            in_buf[nxt][i * 2] = (int32_t)REG_R(codec, CODEC_RX_L);
            in_buf[nxt][i * 2 + 1] = (int32_t)REG_R(codec, CODEC_RX_R);

            REG_W(codec, CODEC_STATUS, 0); // clear data_rdy
        }

        // ── DMA: process in_buf[nxt] → out_buf[nxt] (~5 μs) ─
        dma_transfer(in_phys[nxt], out_phys[nxt]);

        // ── GPIO: read sw/btn, update effect enables + LEDs ──
        control_poll();

        cur = nxt;
    }
}

// ── Entry point ───────────────────────────────────────────────
int main(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
    {
        perror("open /dev/mem");
        return 1;
    }

    codec  = mmap_periph(fd, CODEC_BASE);
    dma    = mmap_periph(fd, DMA_BASE);
    effect = mmap_periph(fd, EFFECT_BASE);
    gpio0  = mmap_periph(fd, GPIO0_BASE);
    gpio1  = mmap_periph(fd, GPIO1_BASE);
    gpio2  = mmap_periph(fd, GPIO2_BASE);
    close(fd);

    printf("[init] peripherals mapped\n");

    init_buffers();
    gpio_init();

    // ── Physical address cross-verify (one-shot) ─────────────────
    // Test: write via virtual addr, read back via /dev/mem at reported phys addr.
    // If they match, cma_get_phy_addr is correct.
    // If they don't match, cma_get_phy_addr is returning the wrong address.
    {
        int fd2 = open("/dev/mem", O_RDWR | O_SYNC);
        uint32_t page_off = out_phys[0] & 0xFFF;
        volatile uint32_t *pm = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd2, out_phys[0] & ~0xFFF);
        out_buf[0][0] = 0xDEADBEEF;
        uint32_t via_phys = pm[page_off / 4];
        pm[page_off / 4] = 0xCAFEBABE;
        uint32_t via_virt = (uint32_t)out_buf[0][0];
        printf("[phys-verify] wrote 0xDEADBEEF via virt → /dev/mem@0x%08x reads 0x%08x\n",
               out_phys[0], via_phys);
        printf("[phys-verify] wrote 0xCAFEBABE via /dev/mem → virt reads 0x%08x\n", via_virt);
        munmap((void *)pm, 4096);
        close(fd2);
        out_buf[0][0] = 0; // clean up
    }

    effect_init();
    dma_init();
    audio_loop();

    // unreachable without signal handler
    for (int i = 0; i < 2; i++)
    {
        cma_free(in_buf[i]);
        cma_free(out_buf[i]);
    }
    return 0;
}
