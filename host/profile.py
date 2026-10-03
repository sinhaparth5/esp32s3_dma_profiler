"""Phase 4: run the device's contention matrix over USB, save CSV and a plot.

usage: uv run --with pyusb --with matplotlib host/profile.py [out_prefix]
writes <out_prefix>.csv and <out_prefix>.png (default: results)
"""
import csv
import struct
import sys

from usbdev import open_device

MEMS = ["SRAM", "SRAM*", "PSRAM"]
KERNELS = ["none", "read", "write", "copy", "stride"]
GDMA = ["idle", "sram->sram", "sram->psram"]
out = sys.argv[1] if len(sys.argv) > 1 else "results"

ep_out, ep_in = open_device()
ep_out.write(b"PROF")
reply = bytes(ep_in.read(4096, 60000))  # the matrix takes ~10 s
magic, count, mhz, cell_size = struct.unpack_from("<4sIII", reply)
assert magic == b"PROF" and cell_size == 32, (magic, cell_size)

rows = []
for i in range(count):
    mem, k, g, _, length, runs, best, elapsed, dma_bytes, total = struct.unpack_from("<BBBBIIIIIQ", reply, 16 + 32 * i)
    rows.append({
        "mem": MEMS[mem], "kernel": KERNELS[k], "gdma": GDMA[g],
        "min_cyc_per_kb": round(best * 1024 / length, 2),
        "avg_cyc_per_kb": round(total / runs * 1024 / length, 2),
        "cpu_mb_s": round(length * runs / (total / mhz), 1),
        "gdma_mb_s": round(dma_bytes * mhz / elapsed, 1) if g else 0.0,
        "runs": runs,
    })

with open(f"{out}.csv", "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=rows[0].keys())
    w.writeheader()
    w.writerows(rows)

print(f"{count} cells @ {mhz} MHz")
print(f"{'mem':6} {'kernel':7} {'gdma':12} {'avg c/KB':>9} {'slowdown':>9} {'cpu MB/s':>9} {'gdma MB/s':>9}")
idle = {(r["mem"], r["kernel"]): r["avg_cyc_per_kb"] for r in rows if r["gdma"] == "idle"}
for r in rows:
    slow = r["avg_cyc_per_kb"] / idle[r["mem"], r["kernel"]] - 1
    print(f"{r['mem']:6} {r['kernel']:7} {r['gdma']:12} {r['avg_cyc_per_kb']:9.1f} {slow:+9.1%} {r['cpu_mb_s']:9.1f} {r['gdma_mb_s']:9.1f}")

# Plot: CPU slowdown vs idle for every mem/kernel, one bar per GDMA mode.
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

groups = list(idle)
fig, ax = plt.subplots(figsize=(11, 4.5))
width = 0.4
for j, g in enumerate(GDMA[1:]):
    vals = [100 * (next(r["avg_cyc_per_kb"] for r in rows if (r["mem"], r["kernel"], r["gdma"]) == (*grp, g)) / idle[grp] - 1) for grp in groups]
    bars = ax.bar([i + (j - 0.5) * width for i in range(len(groups))], vals, width, label=f"GDMA {g}")
    ax.bar_label(bars, fmt="%.0f", fontsize=7)
ax.set_xticks(range(len(groups)), [f"{m}\n{k}" for m, k in groups], fontsize=8)
ax.set_ylabel("CPU slowdown vs idle bus (%)")
ax.set_title(f"ESP32-S3 PIE workload under GDMA contention @ {mhz} MHz")
ax.legend()
fig.tight_layout()
fig.savefig(f"{out}.png", dpi=120)
print(f"wrote {out}.csv, {out}.png")
