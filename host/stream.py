"""Phase 2 ping-pong stream test.

usage: uv run --with pyusb host/stream.py [blocks] [interval_us] [load]
  interval_us = 0 (default): backpressure, expect zero drops, measures max throughput
  interval_us > 0: device produces one 4 KB block per tick; drops show up as seq gaps
  load: Core 1 PIE kernel during the stream, [sram|psram:]read|write|copy|stride (default none)
"""
import struct
import sys
import time
from array import array

from usbdev import open_device

BLK, HDR = 4096, 8
blocks = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
interval = int(sys.argv[2]) if len(sys.argv) > 2 else 0
KERNELS = ["none", "read", "write", "copy", "stride"]
mem, _, kernel = (sys.argv[3] if len(sys.argv) > 3 else "none").rpartition(":")
load = KERNELS.index(kernel) | (0x10 if mem == "psram" else 0)

ep_out, ep_in = open_device()

pattern = array("I", range((BLK - HDR) // 4)).tobytes()
buf = bytearray()
got = bad = gaps = expect = dev_overruns = 0

ep_out.write(struct.pack("<III", blocks, interval, load))
t0 = time.perf_counter()
while got < blocks:
    # read no more than what's left: there is no short packet to end a larger read
    want = min(64 * 1024, (blocks - got) * BLK - len(buf))
    buf += ep_in.read(want, 5000)
    while len(buf) >= BLK:
        seq, dev_overruns = struct.unpack_from("<II", buf)
        if buf[HDR:BLK] != pattern:
            bad += 1
        gaps += seq - expect
        expect = seq + 1
        del buf[:BLK]
        got += 1
dt = time.perf_counter() - t0

mb = blocks * BLK / 1e6
print(f"{blocks} blocks, {mb:.2f} MB in {dt:.2f} s = {mb / dt:.3f} MB/s ({mb * 8 / dt:.2f} Mbit/s)")
if interval:
    print(f"offered {BLK / interval:.3f} MB/s, dropped {gaps} blocks ({gaps / (blocks + gaps):.1%})")
print(f"seq gaps {gaps}, device overruns {dev_overruns}, corrupt {bad}")
print("data OK" if bad == 0 and gaps == dev_overruns else "MISMATCH")
