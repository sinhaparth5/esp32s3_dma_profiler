# ESP32-S3 DMA Profiler: Roadmap

Board: ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM), ESP-IDF v5.3.
Flash/console: `idf.py -p /dev/ttyACM1 flash monitor` (COM port, CH343).
Native USB port: TinyUSB vendor device `303a:4020`.

## Plan corrections (hardware limits)

- GDMA cannot feed USB-OTG. USB-OTG has its own DMA. GDMA serves SPI2/3, UHCI, I2S, LCD/CAM, AES, SHA, ADC, RMT and M2M.
- PIE SIMD is integer only, so there are no 128-bit float vectors.
- USB is full-speed only (12 Mbit/s), so bulk tops out at 1.216 MB/s (measured 1.18).

## Phase 0: Bus contention baseline ✅

The GDMA M2M pump runs on Core 0 with 2 transfers queued. The PIE `ee.vld.128.ip` read workload runs on Core 1. The profiler prints a table on the console at boot.

| Workload | GDMA | Result |
|---|---|---|
| SRAM | idle | 64 cycles/KB (16 B/cycle) |
| SRAM | sram->sram | about 1.5% slower (avg) |
| SRAM* (reads the GDMA source buffer) | sram->sram | about 9% slower (avg) |
| PSRAM | idle | 3936 cycles/KB (about 62 MB/s) |
| PSRAM | sram->psram | 6652 cycles/KB (+69%). GDMA itself drops from 36.4 to 18.0 MB/s |
| GDMA sram->sram | | 61.6 MB/s (was 59 with 1 transfer in flight) |

## Phase 1: USB vendor bulk streaming ✅

- [x] TinyUSB vendor class with bulk IN and OUT endpoints
- [x] Fixed a zero-length packet after every packet: unbuffered vendor mode with 4 KB transfers (top-level `CMakeLists.txt`)
- [x] Full-speed ceiling reached in Phase 2 (1.18 MB/s, see below)

## Phase 2: Ping-pong DMA buffers ✅

GDMA fills one 4 KB block while USB drains the other. Each block is `{u32 seq, u32 overruns}` followed by a fixed pattern.

- [x] Two DMA-capable SRAM buffers with FREE → FILLING → READY handoff (`produce()`, `on_fill_done`, `usb_stream`)
- [x] Backpressure mode (interval 0): lossless and USB-bound
- [x] Paced mode: an `esp_timer` produces one block per tick, like a sampling peripheral. Overruns are counted, and the host checks seq gaps == device overruns
- [x] 2 GDMA transfers in flight in `dma_pump`
- [x] Fixed a 10 ms/block stall: the idle-priority busy-poll let IDLE0 `waiti` until the next tick. The USB task now blocks on notifications from the GDMA ISR and `tud_vendor_tx_cb`
- [ ] Optional: a real peripheral source (I2S RX / ADC continuous) instead of M2M + timer

Run: `uv run --with pyusb host/stream.py [blocks] [interval_us]`

| Mode | Offered | Delivered | Dropped |
|---|---|---|---|
| backpressure | – | 1.182 MB/s (9.46 Mbit/s) | 0 |
| 5000 us | 0.819 MB/s | 0.818 MB/s | 0 |
| 4000 us | 1.024 MB/s | 1.022 MB/s | 0 |
| 3000 us | 1.365 MB/s | 1.180 MB/s | 76/576 (13.2%), seq gaps == overruns |

## Phase 3: SIMD contention engine on Core 1 ✅

- [x] PIE kernels `k_read`, `k_write`, `k_copy` (first half → second half) and `k_stride` (one 16 B load per 32 B dcache line)
- [x] Boot matrix: kernel × memory × GDMA mode (console)
- [x] Stream load: third request word picks the Core 1 kernel for the whole stream (`stream.py 500 3500 psram:write`)
- [x] Fixed: TinyUSB task was pinned to Core 1 at prio 5, so a busy workload time-sliced it to one block per 10 ms. It is now pinned to Core 0 (`sdkconfig.defaults`)

Boot matrix, avg cycles/KB (GDMA MB/s in brackets):

| Memory | Kernel | idle | GDMA sram->sram | GDMA sram->psram |
|---|---|---|---|---|
| SRAM | read | 64.3 | 66.7 (61.6) | 64.5 (36.4) |
| SRAM | write | 64.3 | 66.5 | 64.4 |
| SRAM | copy | 64.4 | 68.3 | 64.7 |
| SRAM | stride | 32.3 | 33.4 | 32.3 |
| PSRAM | read | 3940 | 3940 | 6660 (18.1) |
| PSRAM | write | 7005 | 7013 | 9854 (12.3) |
| PSRAM | copy | 5528 | 5524 | 8260 (14.7) |
| PSRAM | stride | 3943 | 3939 | 6658 (18.0) |

Takeaways:
- SRAM costs 1 cycle per 16 B however it is accessed. GDMA on the same SRAM adds 2–6%.
- PSRAM is line-fill bound: stride (1 load per line) costs the same as reading every byte. Writes cost 1.8× reads (write-back evictions plus fills).
- GDMA into PSRAM is the big contender: +40–70% for the CPU, and the GDMA rate halves (36 → 12–18 MB/s).

Stream under load: 1.18 MB/s backpressure and 0 drops at 3500 us paced, for **every** kernel on SRAM and PSRAM. Core 1 rates were unchanged too.
The stream path (GDMA SRAM→SRAM, CPU copy into the USB FIFO on Core 0) uses about 2% of SRAM bandwidth, and USB full speed is the bottleneck, so contention can't show up there.

## Phase 4: Analytics ✅

- [x] Profiling runs on request over USB, not at boot: the host sends `PROF`, and Core 1 runs the 27-cell matrix (it still logs to the console)
- [x] Reply is one fixed 4 KB block of raw counters (`cell_t`: runs, best/sum cycles, elapsed, GDMA bytes). The host derives every rate
- [x] `host/profile.py` prints a table with slowdown vs idle and writes `<prefix>.csv` and `<prefix>.png`
- [x] Shared device open/drain code in `host/usbdev.py`
- [ ] Skipped: a "USB on" dimension in the matrix. Phase 3 measured no effect from the stream (about 2% of SRAM bandwidth)

Run: `uv run --with pyusb --with matplotlib host/profile.py results/profile`
Note: `cpu_mb_s` is address span per second, so stride (half the bytes) shows 2× read.

Result: `results/profile.png`. GDMA writing into PSRAM costs the CPU +41% to +69% on any PSRAM workload. Everything else stays under 8%.

## Ideas beyond the plan

- A real peripheral producer (I2S RX / ADC continuous) instead of M2M + timer
- Ping-pong buffers in PSRAM, so the stream itself competes with the PSRAM workloads
- Sweep GDMA burst / transfer size and the PSRAM clock (80 vs 120 MHz)
