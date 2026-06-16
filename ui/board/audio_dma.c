// audio_dma.c — Phase 6 C + DMA audio loop (14.6: cleaned, GPIO co-exist)
//
// Pipeline per 256-sample buffer:
//   1. Combined TX/RX loop (one LRCLK per sample, ~5.33 ms):
//      output out_buf[cur] to codec, capture new audio into in_buf[nxt]
//   2. Flush + DMA transfer (~5 μs, negligible)
//   3. Swap cur/nxt
//
// Compile on board:
//   gcc audio_dma.c -lcma -lpthread -O2 -DNDEBUG -o audio_dma
//   sudo ./audio_dma
//
// GPIO co-exist (bidirectional): sw[0/1] always controls dist_en/wobble_en.
// PC UI (ctrl_client.py) can also write dist_en/wobble_en; GPIO is master
// and overrides UI within ~5.33 ms. UI reflects sw state via STATE stdout.
// btn[0/1/2] preset switching always active.
//
// Base addresses (INTERFACE.md, Phase 6 BD):
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
#define GPIO2_BASE   0x40030000UL  // rgbleds[5:0] LD4+LD5 (output)
#define MAP_SIZE     0x10000UL

// ── audio_codec_ctrl registers (offset from CODEC_BASE) ──────
#define CODEC_RX_L   0x00
#define CODEC_RX_R   0x04
#define CODEC_TX_L   0x08
#define CODEC_TX_R   0x0C
#define CODEC_STATUS 0x10

// ── AXI DMA Direct Register Mode offsets ─────────────────────
#define DMA_MM2S_CR  0x00
#define DMA_MM2S_SR  0x04
#define DMA_MM2S_SA  0x18
#define DMA_MM2S_LEN 0x28
#define DMA_S2MM_CR  0x30
#define DMA_S2MM_SR  0x34
#define DMA_S2MM_DA  0x48
#define DMA_S2MM_LEN 0x58
#define DMA_CR_RS    (1 << 0)
#define DMA_SR_IOC   (1 << 12)

// ── AXI GPIO register offsets ─────────────────────────────────
#define GPIO_DATA  0x000
#define GPIO_TRI   0x004
#define GPIO_DATA2 0x008
#define GPIO_TRI2  0x00C

// RGB LED bit masks (gpio2): green channel only
#define RGBLED_LD4 0x02u   // bit1 = LD4 green
#define RGBLED_LD5 0x10u   // bit4 = LD5 green

// ── Effect IP (process_sample) AXI-Lite offsets ──────────────
#define EFFECT_CTRL      0x00
#define EFFECT_N_SAMPLES 0x10
#define EFFECT_DIST_EN   0x18
#define EFFECT_WOBBLE_EN 0x20
#define EFFECT_THRESHOLD 0x28
#define EFFECT_GAIN      0x30
#define EFFECT_LFO_RATE  0x38
#define EFFECT_LFO_DEPTH 0x40
#define EFFECT_LFO_FLOOR 0x48

// ── Effect preset parameter tables ───────────────────────────
#define DIST_THRESHOLD_LOW  ((int)(0.5f * (1 << 23)))  // 4194304
#define DIST_GAIN_LOW       4
#define DIST_THRESHOLD_HIGH ((int)(0.2f * (1 << 23)))  // 1677722
#define DIST_GAIN_HIGH      12
#define WOBBLE_RATE_SLOW    89478   // 1 Hz
#define WOBBLE_DEPTH_SLOW   80
#define WOBBLE_RATE_FAST    357914  // 4 Hz
#define WOBBLE_DEPTH_FAST   100

// Wah depth presets (btn2 cycles A→B→C→A)
#define WAH_FLOOR_A 6
#define WAH_FLOOR_B 4
#define WAH_FLOOR_C 0
static const int         wah_floor_values[3] = { WAH_FLOOR_A, WAH_FLOOR_B, WAH_FLOOR_C };
static const char *const wah_floor_names[3]  = { "A(fc≈83Hz)", "B(fc≈41Hz)", "C(fc=10Hz)" };

// ── Buffer config ─────────────────────────────────────────────
#define N_SAMPLES 255
#define N_WORDS   (N_SAMPLES * 2)
#define BUF_BYTES (N_WORDS * 4)

// ── Globals ───────────────────────────────────────────────────
static volatile uint32_t *codec;
static volatile uint32_t *dma;
static volatile uint32_t *effect;
static volatile uint32_t *gpio0;
static volatile uint32_t *gpio1;
static volatile uint32_t *gpio2;

// ── Control state ─────────────────────────────────────────────
static int      dist_preset      = 0;
static int      wobble_preset    = 0;
static int      wah_depth_preset = 0;
static uint32_t prev_sw          = 0xFF;

#define DEBOUNCE_COUNT 3
static struct { int count; int fired; } btn_db[3];

static int32_t  *in_buf[2],  *out_buf[2];
static uint32_t  in_phys[2],  out_phys[2];

// ── MMIO helpers ──────────────────────────────────────────────
#define REG_R(base, off)       ((base)[(off) / 4])
#define REG_W(base, off, val)  ((base)[(off) / 4] = (uint32_t)(val))

// ── mmap a peripheral ─────────────────────────────────────────
static volatile uint32_t *mmap_periph(int fd, uint32_t base_addr)
{
    void *p = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, (off_t)base_addr);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    return (volatile uint32_t *)p;
}

// ── CMA buffer allocation ─────────────────────────────────────
static void init_buffers(void)
{
    for (int i = 0; i < 2; i++) {
        in_buf[i]   = cma_alloc(BUF_BYTES, 0);
        out_buf[i]  = cma_alloc(BUF_BYTES, 0);
        if (!in_buf[i] || !out_buf[i]) {
            fprintf(stderr, "cma_alloc failed\n");
            exit(1);
        }
        in_phys[i]  = cma_get_phy_addr(in_buf[i]);
        out_phys[i] = cma_get_phy_addr(out_buf[i]);
        printf("[init] buf[%d]: in_phys=0x%08x  out_phys=0x%08x\n",
               i, in_phys[i], out_phys[i]);
    }
}

// ── Preset application ────────────────────────────────────────
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
    REG_W(gpio1, GPIO_DATA,
          (uint32_t)((dist_preset & 1) | ((wobble_preset & 1) << 1)));
    uint32_t rgb = ((sw & 1) ? RGBLED_LD4 : 0u) | (((sw >> 1) & 1) ? RGBLED_LD5 : 0u);
    REG_W(gpio2, GPIO_DATA, rgb);
}

// ── GPIO polling / control (called once per audio buffer ~5.33 ms) ──
//
// sw[0/1] is always applied to dist_en/wobble_en; GPIO is master even
// when bass_ui.py is connected.  PC UI reflects the sw state via the
// STATE lines emitted by ctrl_client.py.
// btn[0/1/2] preset switching is always active.
static void control_poll(void)
{
    uint32_t sw  = REG_R(gpio0, GPIO_DATA)  & 0x3u;
    uint32_t btn = REG_R(gpio0, GPIO_DATA2) & 0x7u;

    // sw[0/1] → dist_en / wobble_en (always, GPIO is master)
    if (sw != prev_sw) {
        REG_W(effect, EFFECT_DIST_EN,   sw & 1u);
        REG_W(effect, EFFECT_WOBBLE_EN, (sw >> 1) & 1u);
        prev_sw = sw;
    }

    // btn[0]: distortion preset toggle
    if (btn & 1u) {
        if (!btn_db[0].fired && ++btn_db[0].count >= DEBOUNCE_COUNT) {
            dist_preset ^= 1;
            apply_dist_preset();
            btn_db[0].fired = 1;
            printf("[ctrl] dist preset → %s  (thr=%d gain=%d)\n",
                   dist_preset ? "HIGH" : "LOW",
                   dist_preset ? DIST_THRESHOLD_HIGH : DIST_THRESHOLD_LOW,
                   dist_preset ? DIST_GAIN_HIGH      : DIST_GAIN_LOW);
        }
    } else {
        btn_db[0].count = 0;
        btn_db[0].fired = 0;
    }

    // btn[1]: wobble preset toggle
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

    // btn[2]: wah depth cycle A→B→C→A
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
    uint32_t sw = REG_R(gpio0, GPIO_DATA) & 0x3u;
    prev_sw = sw;

    REG_W(effect, EFFECT_N_SAMPLES, N_SAMPLES);
    REG_W(effect, EFFECT_DIST_EN,   sw & 1u);
    REG_W(effect, EFFECT_WOBBLE_EN, (sw >> 1) & 1u);

    apply_dist_preset();
    apply_wobble_preset();
    apply_wah_depth_preset();

    REG_W(effect, EFFECT_CTRL, (1 << 7) | (1 << 0)); // AP_START | AUTO_RESTART

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

// ── GPIO init ─────────────────────────────────────────────────
static void gpio_init(void)
{
    REG_W(gpio0, GPIO_TRI,  0xFFu); // ch1 (sw) all input
    REG_W(gpio0, GPIO_TRI2, 0xFFu); // ch2 (btn) all input
    REG_W(gpio1, GPIO_TRI,  0x0u);  // led all output
    REG_W(gpio1, GPIO_DATA, 0x0u);
    REG_W(gpio2, GPIO_TRI,  0x0u);  // rgbled all output
    REG_W(gpio2, GPIO_DATA, 0x0u);
    printf("[init] gpio: sw=0x%x btn=0x%x\n",
           REG_R(gpio0, GPIO_DATA)  & 0x3u,
           REG_R(gpio0, GPIO_DATA2) & 0xFu);
}

// ── DMA init (hard reset then run) ───────────────────────────
static void dma_init(void)
{
    REG_W(dma, DMA_MM2S_CR, 1 << 2);
    REG_W(dma, DMA_S2MM_CR, 1 << 2);
    while (REG_R(dma, DMA_MM2S_CR) & (1 << 2)) ;
    while (REG_R(dma, DMA_S2MM_CR) & (1 << 2)) ;
    REG_W(dma, DMA_MM2S_CR, DMA_CR_RS);
    REG_W(dma, DMA_S2MM_CR, DMA_CR_RS);
    printf("[init] dma: MM2S_SR=0x%08x  S2MM_SR=0x%08x\n",
           REG_R(dma, DMA_MM2S_SR), REG_R(dma, DMA_S2MM_SR));
}

// ── Single DMA transfer (blocking) ───────────────────────────
static void dma_transfer(uint32_t src_phys, uint32_t dst_phys)
{
    REG_W(dma, DMA_S2MM_DA,  dst_phys);
    REG_W(dma, DMA_S2MM_LEN, BUF_BYTES);
    REG_W(dma, DMA_MM2S_SA,  src_phys);
    REG_W(dma, DMA_MM2S_LEN, BUF_BYTES);

    while (!(REG_R(dma, DMA_MM2S_SR) & DMA_SR_IOC)) ;
    REG_W(dma, DMA_MM2S_SR, DMA_SR_IOC);
    while (!(REG_R(dma, DMA_S2MM_SR) & DMA_SR_IOC)) ;
    REG_W(dma, DMA_S2MM_SR, DMA_SR_IOC);
}

// ── Main audio loop ───────────────────────────────────────────
static void audio_loop(void)
{
    int cur = 0;

    // Bootstrap: capture one buffer of live audio to seed the pipeline
    for (int i = 0; i < N_SAMPLES; i++) {
        while (!(REG_R(codec, CODEC_STATUS) & 1)) ;
        in_buf[0][i * 2]     = (int32_t)REG_R(codec, CODEC_RX_L);
        in_buf[0][i * 2 + 1] = (int32_t)REG_R(codec, CODEC_RX_R);
        REG_W(codec, CODEC_STATUS, 0);
    }

    dma_transfer(in_phys[0], out_phys[0]);

    printf("[init] audio loop started (Ctrl+C to stop)\n");
    fflush(stdout);

    while (1) {
        int nxt = 1 - cur;

        // TX current processed output, RX next raw input
        for (int i = 0; i < N_SAMPLES; i++) {
            while (!(REG_R(codec, CODEC_STATUS) & 1)) ;
            REG_W(codec, CODEC_TX_L, (uint32_t)out_buf[cur][i * 2]);
            REG_W(codec, CODEC_TX_R, (uint32_t)out_buf[cur][i * 2 + 1]);
            in_buf[nxt][i * 2]     = (int32_t)REG_R(codec, CODEC_RX_L);
            in_buf[nxt][i * 2 + 1] = (int32_t)REG_R(codec, CODEC_RX_R);
            REG_W(codec, CODEC_STATUS, 0);
        }

        dma_transfer(in_phys[nxt], out_phys[nxt]);
        control_poll();
        cur = nxt;
    }
}

// ── Entry point ───────────────────────────────────────────────
int main(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

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
    effect_init();
    dma_init();
    audio_loop();

    // unreachable without signal handler
    for (int i = 0; i < 2; i++) {
        cma_free(in_buf[i]);
        cma_free(out_buf[i]);
    }
    return 0;
}
