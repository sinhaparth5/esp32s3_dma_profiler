# ESP32-S3 DMA Profiler

Measures how GDMA traffic slows down 128-bit PIE SIMD workloads on SRAM and PSRAM. It also streams data over native USB through GDMA ping-pong buffers.

- **Core 0:** GDMA load generator, ping-pong producer and USB streaming
- **Core 1:** PIE kernels (read / write / copy / stride) and the profiler matrix
- **Host:** Python scripts talk to the board over a USB vendor bulk interface (`303a:4020`)

See `ROADMAP.md` for the phases, design notes and measured results.

## Hardware

- An ESP32-S3 N16R8 board (16 MB flash, 8 MB octal PSRAM) with **two USB ports**, both connected to the PC:
  - **COM / UART port** (CH343, `/dev/ttyACM1`): flashing and console
  - **native USB port**: the profiler's vendor bulk device
- ESP-IDF v5.3 at `~/esp/esp-idf`
- [uv](https://docs.astral.sh/uv/) for the host scripts

## One-time setup

```bash
# serial port access (log out and back in afterwards)
sudo usermod -aG dialout $USER

# USB access to the profiler device for plugdev users
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="4020", MODE="0660", GROUP="plugdev"' \
  | sudo tee /etc/udev/rules.d/99-esp32s3-profiler.rules
sudo udevadm control --reload-rules
```

## Build and flash

```bash
. ~/esp/esp-idf/export.sh
idf.py -p /dev/ttyACM1 flash monitor   # Ctrl+] exits the monitor
```

The first build downloads TinyUSB into `managed_components/` and generates `sdkconfig` from `sdkconfig.defaults`.
If the COM port shows up under another name, check `ls /dev/ttyACM*`.

## Run the tests

Every test prints `data OK` when it passes.

### 1. Max USB throughput (lossless)

```bash
uv run --with pyusb host/stream.py 1000
```
Expect about 1.18 MB/s (the full-speed bulk ceiling is 1.216 MB/s), with 0 seq gaps.

### 2. Paced stream with overrun detection

```bash
uv run --with pyusb host/stream.py 500 4000   # 1.02 MB/s offered: expect 0 dropped
uv run --with pyusb host/stream.py 500 3000   # 1.37 MB/s offered: expect ~13% dropped
```
The second argument is the producer period in µs. Dropped blocks must equal the device overrun count.

### 3. Stream while Core 1 hammers memory

```bash
uv run --with pyusb host/stream.py 500 3500 psram:write
```
The load can be `sram:` or `psram:` followed by `read`, `write`, `copy` or `stride`. Expect no throughput loss and 0 drops. The Core 1 rate is printed on the console.

### 4. Contention profile (CSV and plot)

```bash
uv run --with pyusb --with matplotlib host/profile.py results/profile
```
This takes about 10 s. It prints the matrix and writes `results/profile.csv` and `results/profile.png`.
To redraw the plot from the saved CSV without the board, add `--replot`.

Run all four in one go:

```bash
uv run --with pyusb host/stream.py 1000 && \
uv run --with pyusb host/stream.py 500 3000 && \
uv run --with pyusb host/stream.py 500 3500 psram:write && \
uv run --with pyusb --with matplotlib host/profile.py results/profile
```

## Troubleshooting

| Symptom | Fix |
|---|---|
| `device 303a:4020 not found` | Plug in the native USB port. Check `lsusb \| grep 303a` |
| `Access denied` from pyusb | The udev rule only applies when the device enumerates. Replug or press RST |
| Serial `Permission denied` | You're not in `dialout` yet, so log out and back in |
| Stream stuck at 0.41 MB/s | The TinyUSB task is on Core 1. Delete `sdkconfig` and rebuild so `sdkconfig.defaults` applies |

## Layout

```
main/main.c          firmware: GDMA pump, PIE kernels, ping-pong stream, USB protocol
host/stream.py       stream test: throughput, seq gaps, overruns, data check
host/profile.py      contention matrix over USB, writes CSV and PNG
host/usbdev.py       shared device open and stale-data drain
CMakeLists.txt       TinyUSB unbuffered vendor mode (avoids a ZLP per packet)
sdkconfig.defaults   target, PSRAM, CPU 240 MHz, TinyUSB on Core 0
results/             latest profile output
docs/report.tex      full write-up; docs/report.pdf is the compiled version
```
