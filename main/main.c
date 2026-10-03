#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_async_memcpy.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "tinyusb.h"

#define DMA_LEN    (32 * 1024)   // per GDMA M2M transfer
#define SRAM_LEN   (64 * 1024)   // workload buffer in internal SRAM
#define PSRAM_LEN  (256 * 1024)  // > 32 KB dcache, so every pass misses
#define RUNS       50
#define MIN_WINDOW (100 * 1000)  // us; long enough for many DMA completions
#define DMA_INFLIGHT 2

enum { DMA_OFF, DMA_SRAM_TO_SRAM, DMA_SRAM_TO_PSRAM, DMA_MODES };
static const char *dma_names[] = { "idle", "sram->sram", "sram->psram" };

static async_memcpy_handle_t mcp;
static SemaphoreHandle_t dma_slots;
static uint8_t *dma_src, *dma_dst_sram, *dma_dst_psram;
static volatile int dma_mode = DMA_OFF;

static volatile uint32_t dma_bytes;  // bumped in the done ISR

static bool on_dma_done(async_memcpy_handle_t h, async_memcpy_event_t *e, void *arg)
{
    BaseType_t woken = pdFALSE;
    dma_bytes += DMA_LEN;
    xSemaphoreGiveFromISR(dma_slots, &woken);
    return woken == pdTRUE;
}

// Core 0: keep DMA_INFLIGHT GDMA copies queued while dma_mode != DMA_OFF, so the
// channel never idles between transfers.
static void dma_pump(void *arg)
{
    for (;;) {
        xSemaphoreTake(dma_slots, portMAX_DELAY);
        while (dma_mode == DMA_OFF) vTaskDelay(1);
        void *dst = dma_mode == DMA_SRAM_TO_SRAM ? dma_dst_sram : dma_dst_psram;
        ESP_ERROR_CHECK(esp_async_memcpy(mcp, dst, dma_src, DMA_LEN, on_dma_done, NULL));
    }
}

// 128-bit PIE kernels, all sweeping `len` bytes of address space so cycles/KB compare.
// len must be a multiple of 256.
static void __attribute__((noinline)) k_read(void *buf, size_t len)
{
    asm volatile(
        "loopnez %1, 1f\n"
        "ee.vld.128.ip q0, %0, 16\n"
        "ee.vld.128.ip q1, %0, 16\n"
        "ee.vld.128.ip q2, %0, 16\n"
        "ee.vld.128.ip q3, %0, 16\n"
        "1:\n"
        : "+r"(buf) : "r"(len / 64) : "memory");
}

static void __attribute__((noinline)) k_write(void *buf, size_t len)
{
    asm volatile(
        "loopnez %1, 1f\n"
        "ee.vst.128.ip q0, %0, 16\n"
        "ee.vst.128.ip q1, %0, 16\n"
        "ee.vst.128.ip q2, %0, 16\n"
        "ee.vst.128.ip q3, %0, 16\n"
        "1:\n"
        : "+r"(buf) : "r"(len / 64) : "memory");
}

// First half -> second half.
static void __attribute__((noinline)) k_copy(void *buf, size_t len)
{
    void *dst = (uint8_t *)buf + len / 2;
    asm volatile(
        "loopnez %2, 1f\n"
        "ee.vld.128.ip q0, %0, 16\n"
        "ee.vld.128.ip q1, %0, 16\n"
        "ee.vst.128.ip q0, %1, 16\n"
        "ee.vst.128.ip q1, %1, 16\n"
        "1:\n"
        : "+r"(buf), "+r"(dst) : "r"(len / 64) : "memory");
}

// One 16 B load per 32 B dcache line: on PSRAM every load is a line miss.
static void __attribute__((noinline)) k_stride(void *buf, size_t len)
{
    asm volatile(
        "loopnez %1, 1f\n"
        "ee.vld.128.ip q0, %0, 32\n"
        "ee.vld.128.ip q1, %0, 32\n"
        "ee.vld.128.ip q2, %0, 32\n"
        "ee.vld.128.ip q3, %0, 32\n"
        "1:\n"
        : "+r"(buf) : "r"(len / 128) : "memory");
}

enum { K_NONE, K_READ, K_WRITE, K_COPY, K_STRIDE, K_KINDS };
static const char *k_names[] = { "none", "read", "write", "copy", "stride" };
static void (*const kernels[])(void *, size_t) = { NULL, k_read, k_write, k_copy, k_stride };

static uint8_t *sram, *psram;  // Core 1 workload buffers

enum { MEM_SRAM, MEM_SRAM_DMA_SRC, MEM_PSRAM };
static const char *mem_names[] = { "SRAM", "SRAM*", "PSRAM" };

// One profile cell as sent to the host: raw counters, the host derives the rates.
typedef struct {
    uint8_t mem, kernel, gdma, pad;
    uint32_t len, runs, best, elapsed, dma_bytes;  // cycles, except len/dma_bytes
    uint64_t sum;                                  // cycles over all runs
} cell_t;
_Static_assert(sizeof(cell_t) == 32, "host parses 32 B cells");

static void run_cell(cell_t *c, int where, int k, void *buf, size_t len, int mode)
{
    dma_mode = mode;
    vTaskDelay(pdMS_TO_TICKS(20));  // let the pump reach steady state

    uint32_t best = UINT32_MAX, n = 0;
    uint64_t sum = 0;
    uint32_t b0 = dma_bytes, t0 = esp_cpu_get_cycle_count();
    uint32_t window = MIN_WINDOW * esp_rom_get_cpu_ticks_per_us();
    while (n < RUNS || esp_cpu_get_cycle_count() - t0 < window) {
        uint32_t s = esp_cpu_get_cycle_count();
        kernels[k](buf, len);
        uint32_t dt = esp_cpu_get_cycle_count() - s;
        sum += dt;
        if (dt < best) best = dt;
        n++;
    }
    uint32_t elapsed = esp_cpu_get_cycle_count() - t0, moved = dma_bytes - b0;
    dma_mode = DMA_OFF;

    *c = (cell_t){ where, k, mode, 0, len, n, best, elapsed, moved, sum };
    float mhz = esp_rom_get_cpu_ticks_per_us();
    printf("%-6s %-7s %-12s %8.2f %8.2f %10.1f\n", mem_names[where], k_names[k], dma_names[mode],
           (float)best * 1024 / len, (float)sum / n * 1024 / len,
           mode == DMA_OFF ? 0.0f : moved * mhz / elapsed);
}

// Ping-pong stream: GDMA fills one block while USB drains the other.
// Host sends {u32 blocks, u32 interval_us, u32 load} on bulk OUT and gets `blocks` blocks of
// BLK bytes on bulk IN: {u32 seq, u32 overruns} header + a fixed pattern from dma_src.
//   interval_us == 0: backpressure, the producer waits for a free buffer, nothing is lost.
//   interval_us  > 0: a timer produces one block per tick, like a sampling peripheral;
//                     a tick that finds its buffer still owned by USB is an overrun
//                     (dropped block, seq still advances so the host sees the gap).
//   load: Core 1 kernel to run for the whole stream, kernel | 0x10 for PSRAM (0 = none).
#define BLK CFG_TUD_VENDOR_TX_EPSIZE
#define HDR 8
enum { PP_FREE, PP_FILLING, PP_READY };

static uint8_t *pp[2];
static volatile uint8_t pp_state[2];
static portMUX_TYPE pp_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t prod_next, prod_seq, overruns, blocks_left, interval_us;
static esp_timer_handle_t pace_timer;
static TaskHandle_t usb_task;
static volatile uint32_t stream_load, profile_req;

static bool on_fill_done(async_memcpy_handle_t h, async_memcpy_event_t *e, void *arg)
{
    BaseType_t woken = pdFALSE;
    pp_state[(int)arg] = PP_READY;
    vTaskNotifyGiveFromISR(usb_task, &woken);
    return woken == pdTRUE;
}

// Claim the next buffer in order and start GDMA into it. Called from the pace
// timer (paced), the USB task after freeing a buffer, and the start request.
static void produce(bool paced)
{
    taskENTER_CRITICAL(&pp_lock);
    int i = prod_next;
    if (!blocks_left || pp_state[i] != PP_FREE) {
        if (blocks_left && paced) { prod_seq++; overruns++; }
        taskEXIT_CRITICAL(&pp_lock);
        return;
    }
    pp_state[i] = PP_FILLING;
    ((uint32_t *)pp[i])[0] = prod_seq++;
    ((uint32_t *)pp[i])[1] = overruns;
    prod_next ^= 1;
    bool last = !--blocks_left;
    taskEXIT_CRITICAL(&pp_lock);
    if (last && paced) esp_timer_stop(pace_timer);
    ESP_ERROR_CHECK(esp_async_memcpy(mcp, pp[i] + HDR, dma_src, BLK - HDR, on_fill_done, (void *)i));
}

static void pace_tick(void *arg) { produce(true); }

void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buf, uint16_t len)
{
    // ponytail: requests during a running stream or profile are ignored, no abort command.
    if (profile_req) return;
    bool idle = !blocks_left && pp_state[0] == PP_FREE && pp_state[1] == PP_FREE;
    if (len == 4 && !memcmp(buf, "PROF", 4)) {
        if (idle) profile_req = 1;
        return;
    }
    if (len < 12 || !idle || pp_state[0] != PP_FREE || pp_state[1] != PP_FREE) return;
    memcpy(&interval_us, buf + 4, 4);
    prod_seq = overruns = 0;
    memcpy((void *)&stream_load, buf + 8, 4);
    memcpy(&blocks_left, buf, 4);
    if (interval_us) {
        ESP_ERROR_CHECK(esp_timer_start_periodic(pace_timer, interval_us));
    } else {
        produce(false);
        produce(false);
    }
}

void tud_vendor_tx_cb(uint8_t idx, uint32_t sent_bytes) { xTaskNotifyGive(usb_task); }

// Blocks on notifications from the GDMA ISR (block ready) and TinyUSB (IN transfer done).
// No busy-poll: at idle priority, yielding let IDLE0 waiti until the next 10 ms tick.
// tud_vendor_write copies the block into TinyUSB's endpoint buffer, so the ping-pong
// buffer is free again as soon as it returns.
static void usb_stream(void *arg)
{
    int c = 0;  // consumes in the same order produce() fills
    for (;;) {
        if (pp_state[c] != PP_READY || !tud_vendor_write_available()) {
            ulTaskNotifyTake(pdTRUE, 1);  // 1 tick timeout backs up a missed wakeup
            continue;
        }
        tud_vendor_write(pp[c], BLK);  // BLK == endpoint transfer size, one call
        pp_state[c] = PP_FREE;
        c ^= 1;
        if (!interval_us) produce(false);
        if (!blocks_left && pp_state[0] == PP_FREE && pp_state[1] == PP_FREE) stream_load = 0;
    }
}

// Host sends "PROF" on bulk OUT and gets one BLK-byte reply on bulk IN:
// {u32 'PROF', u32 cells, u32 cpu_mhz, u32 sizeof(cell_t)} then the cells, zero padded.
// A full-size transfer needs no short packet or ZLP to end the host's read.
static void profile(void)
{
    static uint32_t reply[BLK / 4];
    cell_t *c = (cell_t *)&reply[4];

    printf("\n%-6s %-7s %-12s %8s %8s %10s\n", "data", "kernel", "gdma", "min c/KB", "avg c/KB", "dma MB/s");
    for (int k = K_READ; k < K_KINDS; k++)
        for (int m = 0; m < DMA_MODES; m++) run_cell(c++, MEM_SRAM, k, sram, SRAM_LEN, m);
    for (int m = 0; m < DMA_MODES; m++) run_cell(c++, MEM_SRAM_DMA_SRC, K_READ, dma_src, DMA_LEN, m);
    for (int k = K_READ; k < K_KINDS; k++)
        for (int m = 0; m < DMA_MODES; m++) run_cell(c++, MEM_PSRAM, k, psram, PSRAM_LEN, m);
    printf("SRAM* = workload reads the GDMA source buffer (same SRAM bank)\n");

    memcpy(reply, "PROF", 4);
    reply[1] = c - (cell_t *)&reply[4];
    reply[2] = esp_rom_get_cpu_ticks_per_us();
    reply[3] = sizeof(cell_t);
    while (!tud_vendor_write_available()) vTaskDelay(1);
    tud_vendor_write(reply, BLK);  // no stream is running, so usb_stream isn't writing
}

// Core 1: profile on request; otherwise serve as the stream's load generator.
static void profiler(void *arg)
{
    for (;;) {
        uint32_t load = stream_load, k = load & 0xf;
        if (profile_req) { profile(); profile_req = 0; continue; }
        if (k == K_NONE || k >= K_KINDS) { vTaskDelay(1); continue; }
        bool ps = load & 0x10;
        void *buf = ps ? psram : sram;
        size_t len = ps ? PSRAM_LEN : SRAM_LEN;
        uint32_t passes = 0, t0 = esp_cpu_get_cycle_count();
        while (stream_load == load) { kernels[k](buf, len); passes++; }
        uint32_t dt = esp_cpu_get_cycle_count() - t0;
        printf("stream load %s %s: %" PRIu32 " passes, %.0f c/KB\n", ps ? "PSRAM" : "SRAM",
               k_names[k], passes, (float)dt * 1024 / ((uint64_t)passes * len));
    }
}

void app_main(void)
{
    printf("\n--- ESP32-S3 GDMA bus contention profiler @ %" PRIu32 " MHz ---\n",
           esp_rom_get_cpu_ticks_per_us());

    dma_src = heap_caps_aligned_alloc(16, DMA_LEN, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    dma_dst_sram = heap_caps_aligned_alloc(16, DMA_LEN, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    dma_dst_psram = heap_caps_aligned_alloc(64, DMA_LEN, MALLOC_CAP_SPIRAM);
    sram = heap_caps_aligned_alloc(16, SRAM_LEN, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    psram = heap_caps_aligned_alloc(16, PSRAM_LEN, MALLOC_CAP_SPIRAM);
    assert(dma_src && dma_dst_sram && dma_dst_psram && sram && psram);

    for (uint32_t i = 0; i < DMA_LEN / 4; i++) ((uint32_t *)dma_src)[i] = i;  // stream pattern
    for (int i = 0; i < 2; i++) {
        pp[i] = heap_caps_aligned_alloc(16, BLK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(pp[i]);
    }

    async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_async_memcpy_install_gdma_ahb(&cfg, &mcp));
    dma_slots = xSemaphoreCreateCounting(DMA_INFLIGHT, DMA_INFLIGHT);
    const esp_timer_create_args_t targs = { .callback = pace_tick, .name = "pace" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &pace_timer));

    xTaskCreatePinnedToCore(usb_stream, "usb_stream", 4096, NULL, 6, &usb_task, 0);
    const tinyusb_config_t tusb_cfg = {0};  // default descriptors: one vendor bulk IN/OUT pair
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    xTaskCreatePinnedToCore(dma_pump, "dma_pump", 4096, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(profiler, "profiler", 4096, NULL, 5, NULL, 1);
}
