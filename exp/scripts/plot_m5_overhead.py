#!/usr/bin/env python3
"""M5 overhead curves: per-request latency ratio vs KVM, lat_SVM/lat_KVM.

x = bs (log), y = latency ratio (linear), KVM = 1.0 reference line.
  SVM P=off: 2.46 -> 9.24x   (overhead grows with bs: remote per-byte cost)
  SVM P=on : 2.52 -> 7.58x   (P=on flattens the big-block tail)
On/off gap at 2M: 9.24 vs 7.58x.

Values from 主总结 §5 authoritative table (verified vs exp/results/m5/*.bs.json).
Emits SVG (+PNG) for manual review only.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

H = "/home/waiai/svm"
OUT_SVG = os.path.join(H, "exp/results/m5/m5_overhead.svg")
OUT_PNG = os.path.join(H, "exp/results/m5/m5_overhead.png")

BS_KB = [16, 64, 256, 1024, 2048]          # x axis, KB
BS_LABEL = ["16K", "64K", "256K", "1M", "2M"]
LAT = {
    "KVM":       [19.8, 23.6, 27.3, 62.8, 88.5],
    "SVM P=off": [48.7, 72.2, 172.3, 506.1, 818.0],
    "SVM P=on":  [49.8, 74.8, 154.5, 375.6, 671.3],
}
COLORS = {"SVM P=off": "#4c72b0", "SVM P=on": "#dd8452", "KVM": "#8c8c8c"}
MARKERS = {"SVM P=off": "o", "SVM P=on": "s"}
KVM = LAT["KVM"]


def main():
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    for name in ("SVM P=off", "SVM P=on"):
        ratio = [s / k for s, k in zip(LAT[name], KVM)]
        ax.plot(BS_KB, ratio, marker=MARKERS[name], lw=1.8, ms=6,
                color=COLORS[name], label=name)
        for x, r in zip(BS_KB, ratio):
            ax.annotate(f"{r:.2f}x", (x, r), textcoords="offset points",
                        xytext=(0, 7), ha="center", fontsize=8, color=COLORS[name])
    ax.axhline(1.0, color=COLORS["KVM"], lw=1.6, ls="--", label="KVM = 1.0x")
    ax.annotate("1.0x", (BS_KB[-1], 1.0), textcoords="offset points",
                xytext=(6, 0), ha="left", va="center", fontsize=8,
                color=COLORS["KVM"])

    ax.set_xscale("log")
    ax.set_xticks(BS_KB)
    ax.set_xticklabels(BS_LABEL)
    ax.set_xlabel("bs (log scale)", fontsize=10)
    ax.set_ylabel("per-request latency ratio vs KVM (lat_SVM/lat_KVM)", fontsize=10)
    ax.set_ylim(0, 10.5)
    ax.set_title("M5: SVM latency overhead over KVM vs bs\n"
                 "(SVM I=256, iodepth=1, zc, M=4M)", fontsize=11)
    ax.grid(axis="both", alpha=0.3)
    ax.legend(loc="upper left", fontsize=9)
    fig.tight_layout()
    fig.savefig(OUT_PNG, dpi=150)
    fig.savefig(OUT_SVG)
    plt.close(fig)
    print("plot ->", OUT_SVG)
    print("plot ->", OUT_PNG)


if __name__ == "__main__":
    main()
