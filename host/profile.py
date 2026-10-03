"""Phase 4: run the device's contention matrix over USB, save CSV and a plot.

usage: uv run --with pyusb --with matplotlib host/profile.py [out_prefix] [--replot]
writes <out_prefix>.csv and <out_prefix>.png (default: results)
--replot redraws the PNG from an existing <out_prefix>.csv without the board
"""
import csv
import struct
import sys

from usbdev import open_device

MEMS = ["SRAM", "SRAM*", "PSRAM"]
KERNELS = ["none", "read", "write", "copy", "stride"]
GDMA = ["idle", "sram->sram", "sram->psram"]
args = [a for a in sys.argv[1:] if not a.startswith("--")]
out = args[0] if args else "results"

if "--replot" in sys.argv:  # redraw from an existing CSV, no board needed
    with open(f"{out}.csv") as f:
        rows = [{k: v if k in ("mem", "kernel", "gdma") else float(v) for k, v in r.items()} for r in csv.DictReader(f)]
    count, mhz = len(rows), 240
else:
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

# Plot: three line charts sharing one colour per workload.
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

cell = {(r["mem"], r["kernel"], r["gdma"]): r for r in rows}
workloads = list(idle)
COLOR = {"read": "#2563eb", "write": "#dc2626", "copy": "#16a34a", "stride": "#9333ea"}
LINE = {"SRAM": "-", "SRAM*": ":", "PSRAM": "--"}
MARK = {"SRAM": "o", "SRAM*": "D", "PSRAM": "s"}
def color(m, k): return "#f59e0b" if m == "SRAM*" else COLOR[k]
def name(m, k): return f"{m} {k}"
slow = {key: 100 * (cell[key]["avg_cyc_per_kb"] / idle[key[:2]] - 1) for key in cell}

plt.rcParams.update({"font.family": "DejaVu Sans", "axes.spines.top": False, "axes.spines.right": False,
                     "axes.grid": True, "grid.color": "#e5e7eb", "grid.linewidth": 0.8, "axes.edgecolor": "#9ca3af",
                     "axes.titleweight": "bold", "axes.titlesize": 12, "axes.titlepad": 10})
fig = plt.figure(figsize=(15, 10), facecolor="white")
gs = fig.add_gridspec(2, 2, height_ratios=[1.1, 1], hspace=0.42, wspace=0.18, top=0.82, bottom=0.08, left=0.06, right=0.97)
ax1, ax2, ax3 = fig.add_subplot(gs[0, 0]), fig.add_subplot(gs[0, 1]), fig.add_subplot(gs[1, :])
x = range(len(GDMA))
modes = ["bus idle", "GDMA\nSRAM → SRAM", "GDMA\nSRAM → PSRAM"]

# 1. slowdown vs idle
ax1.axhspan(-1, 10, color="#dcfce7", alpha=0.5, zorder=0)
ax1.text(0.02, 5, "< 10 %: negligible", fontsize=8, color="#15803d", va="center")
for m, k in workloads:
    ys = [slow[m, k, g] for g in GDMA]
    ax1.plot(x, ys, LINE[m], marker=MARK[m], color=color(m, k), lw=2.2, ms=7, label=name(m, k))
# label the big end points; lines that land on the same value (read and stride) share one label
ends = {}
for m, k in workloads:
    if slow[m, k, GDMA[-1]] > 20:
        ends.setdefault(round(slow[m, k, GDMA[-1]]), []).append((m, k))
for v, ws in ends.items():
    ax1.annotate(f"+{v}%  {ws[0][0]} " + " / ".join(k for _, k in ws), (2, v), xytext=(10, 0), textcoords="offset points",
                 va="center", fontsize=9, fontweight="bold", color=color(*ws[0]))
ax1.set_xticks(x, modes)
ax1.set_xlim(-0.15, 3.0)
ax1.set_ylim(-3, max(slow.values()) * 1.12)
ax1.set_ylabel("CPU slowdown vs idle bus (%)")
ax1.set_title("How much slower the CPU gets")

# 2. absolute CPU throughput (log)
for m, k in workloads:
    ys = [cell[m, k, g]["cpu_mb_s"] for g in GDMA]
    ax2.plot(x, ys, LINE[m], marker=MARK[m], color=color(m, k), lw=2.2, ms=7)
ax2.set_yscale("log")
ax2.set_xticks(x, modes)
ax2.set_xlim(-0.15, 2.15)
ax2.set_ylabel("CPU throughput (MB/s, log scale)")
ax2.set_title("CPU throughput: SRAM vs PSRAM")
sram_mb = cell["SRAM", "read", "idle"]["cpu_mb_s"]
psram_mb = cell["PSRAM", "read", "idle"]["cpu_mb_s"]
ax2.annotate("", xy=(0.08, psram_mb), xytext=(0.08, sram_mb), arrowprops=dict(arrowstyle="<->", color="#6b7280"))
ax2.text(0.14, (sram_mb * psram_mb) ** 0.5, f"PSRAM is {sram_mb / psram_mb:.0f}× slower\nthan SRAM (read)", fontsize=9, color="#374151", va="center")

# 3. GDMA throughput under each workload
wx = range(len(workloads))
for g, c in [("sram->sram", "#0ea5e9"), ("sram->psram", "#f97316")]:
    ys = [cell[m, k, g]["gdma_mb_s"] for m, k in workloads]
    ax3.plot(wx, ys, "-o", color=c, lw=2.5, ms=7, label=f"GDMA {g.replace('->', ' → ').upper()}")
    ax3.fill_between(wx, ys, alpha=0.12, color=c)
    for i, y in enumerate(ys):
        ax3.annotate(f"{y:.1f}", (i, y), xytext=(0, 7), textcoords="offset points", ha="center", fontsize=8, color=c)
first_psram = next(i for i, (m, _) in enumerate(workloads) if m == "PSRAM")
ax3.axvspan(first_psram - 0.5, len(workloads) - 0.5, color="#fef3c7", alpha=0.6, zorder=0)
ax3.text(first_psram - 0.4, 3, "CPU working in PSRAM", fontsize=9, color="#b45309", fontweight="bold")
ax3.set_xticks(wx, [name(m, k) for m, k in workloads])
for t, (m, k) in zip(ax3.get_xticklabels(), workloads):
    t.set_color(color(m, k))
ax3.set_xlim(-0.5, len(workloads) - 0.5)
ax3.set_ylim(0, max(r["gdma_mb_s"] for r in rows) * 1.4)
ax3.set_ylabel("GDMA throughput (MB/s)")
ax3.set_title("GDMA throughput while the CPU runs each workload")
ax3.legend(loc="upper center", ncol=2, frameon=False)

worst = max(slow, key=slow.get)
fig.suptitle(f"ESP32-S3 bus contention: PIE SIMD (Core 1) vs GDMA (Core 0) @ {mhz} MHz", fontsize=16, fontweight="bold", x=0.06, y=0.975, ha="left")
fig.text(0.06, 0.92, f"Worst case: {name(*worst[:2])} with GDMA {worst[2]} is +{slow[worst]:.0f}% slower.  "
         f"SRAM workloads stay under {max(v for (m, _, _), v in slow.items() if m != 'PSRAM'):.0f}%.  "
         "SRAM* = CPU reads the GDMA source buffer.", fontsize=10.5, color="#4b5563")
fig.legend(*ax1.get_legend_handles_labels(), loc="upper left", ncol=9, frameon=False, fontsize=9, bbox_to_anchor=(0.055, 0.905), handlelength=2.8)
fig.savefig(f"{out}.png", dpi=150)
print(f"wrote {out}.csv, {out}.png")
