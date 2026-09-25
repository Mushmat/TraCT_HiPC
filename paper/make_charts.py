import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
import glob
for f in glob.glob("/usr/share/texmf/fonts/opentype/public/tex-gyre/texgyretermes-*.otf"):
    font_manager.fontManager.addfont(f)
from matplotlib.ticker import FixedLocator, FixedFormatter, NullLocator
from data import *

BLUE, ORANGE, AQUA, YELLOW = "#2a78d6", "#eb6834", "#1baf7a", "#eda100"
INK, INK2, GRID = "#0b0b0b", "#52514e", "#dcdcd8"

plt.rcParams.update({
    "font.family": "serif", "font.serif": ["Liberation Serif", "DejaVu Serif"],
    "font.size": 7, "axes.labelsize": 7, "xtick.labelsize": 6.5, "ytick.labelsize": 6.5,
    "legend.fontsize": 6, "axes.edgecolor": INK2, "axes.linewidth": 0.6,
    "xtick.color": INK2, "ytick.color": INK2, "axes.labelcolor": INK,
    "xtick.major.width": 0.6, "ytick.major.width": 0.6, "xtick.major.size": 2.5, "ytick.major.size": 2.5,
    "pdf.fonttype": 42, "ps.fonttype": 42,
})

def style(ax):
    ax.grid(True, axis="y", color=GRID, linewidth=0.5)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)

W, H = 2.3, 1.48

# (a) throughput vs block size
fig, ax = plt.subplots(figsize=(W, H))
style(ax)
xs = [shm_exact_mb[m] for m in shm]
ax.plot(xs, [gbps(shm_exact_mb[m]*MB, shm[m][0]) for m in shm], color=BLUE, lw=1.4,
        marker="o", ms=4, label="SHM, 1 node")
rx = list(rdma)
ax.plot(rx, [gbps(m*MB, rdma[m][0]) for m in rx], color=ORANGE, lw=1.4, ls="--",
        marker="s", ms=4, label="RDMA W_IMM, 1 node")
tx = list(tract)
ax.plot(tx, [gbps(m*MB, tract[m][0]) for m in tx], color=AQUA, lw=1.4, ls="-.",
        marker="^", ms=4.5, label="TraCT write, 3 nodes")
ax.plot(tx, [gbps(m*MB, tract[m][2]) for m in tx], color=YELLOW, lw=1.4, ls=":",
        marker="D", ms=3.8, label="TraCT read, 3 nodes")
ax.axhline(verbs_write_4MB_GBps, color=INK2, lw=0.6, ls=(0, (2, 2)))
ax.set_xscale("log", base=2)
ticks = [4, 8, 12, 16, 24, 32]
ax.xaxis.set_major_locator(FixedLocator(ticks))
ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))
ax.xaxis.set_minor_locator(NullLocator())
ax.set_xlim(3.5, 36)
ax.set_ylim(0, 18.8)
ax.set_yticks([0, 5, 10, 15])
ax.set_xlabel("KV block size (MiB)")
ax.set_ylabel("Throughput (GB/s)")
ax.legend(loc="upper center", ncol=2, frameon=False, handlelength=2.2, columnspacing=0.8,
          borderaxespad=0.05, labelspacing=0.2, handletextpad=0.4, fontsize=5.6)
fig.tight_layout(pad=0.2)
fig.savefig("figs/fig_throughput.pdf"); fig.savefig("figs/fig_throughput.png", dpi=300)

# (b) 32 MiB: throughput per request vs cumulative data written
fig, ax = plt.subplots(figsize=(W, H))
style(ax)
def cum(blocks):
    out, s = [], 0
    for b in blocks:
        s += b * 32 * MB
        out.append(s / 1e9)
    return out
ax.plot(cum(shm_blocks), shm32_gbps, color=BLUE, lw=1.4, marker="o", ms=3, label="SHM, 1 node")
ax.plot(cum(shm_blocks), rdma32_gbps, color=ORANGE, lw=1.4, ls="--", marker="s", ms=3,
        label="RDMA W_IMM, 1 node (4 GiB ring)")
ax.plot(cum(tract_blocks), [32*MB/(ms*1e-3)/1e9 for ms in tract32_ms_per_blk], color=AQUA, lw=1.4,
        ls="-.", marker="^", ms=3.4, label="TraCT write, 3 nodes")
ax.set_xlim(0, 41)
ax.set_ylim(0, 12)
ax.set_xlabel("Data written so far (GB), 32 MiB blocks")
ax.set_ylabel("Per-request throughput (GB/s)")
ax.legend(loc="upper right", frameon=False, handlelength=2.4, borderaxespad=0.1, labelspacing=0.25)
fig.tight_layout(pad=0.2)
fig.savefig("figs/fig_footprint.pdf"); fig.savefig("figs/fig_footprint.png", dpi=300)

# (c) per-block time at 256 KiB
fig, ax = plt.subplots(figsize=(W, H))
ax.grid(True, axis="x", color=GRID, linewidth=0.5); ax.set_axisbelow(True)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
labels = ["Raw RDMA WRITE\n(verbs, 1 node)", "TraCT-RDMA writer\n(1 node)",
          "+ concurrent reader\n(1 node)", "+ concurrent reader\n(3 nodes)"]
vals = [verbs_write_256K_us, tract_256k[0][2], tract_256k[1][2], tract_256k[2][2]]
y = list(range(len(vals)))[::-1]
ax.barh(y, vals, height=0.58, color=BLUE, edgecolor="white", linewidth=0.8)
for yi, v in zip(y, vals):
    ax.text(v + 2, yi, f"{v:.1f}", va="center", fontsize=6, color=INK)
ax.annotate("", xy=(vals[1], y[1] + 0.42), xytext=(vals[0], y[1] + 0.42),
            arrowprops=dict(arrowstyle="<->", lw=0.6, color=INK2, shrinkA=0, shrinkB=0))
ax.text((vals[0] + vals[1]) / 2, y[1] + 0.5, "+23 µs metadata", ha="center", va="bottom", fontsize=5.5, color=INK2)
ax.set_yticks(y); ax.set_yticklabels(labels, fontsize=6)
ax.set_xlim(0, 160)
ax.set_xlabel("Time per 256 KiB block (µs)")
ax.tick_params(axis="y", length=0)
fig.tight_layout(pad=0.2)
fig.savefig("figs/fig_breakdown.pdf"); fig.savefig("figs/fig_breakdown.png", dpi=300)
print("ok")
