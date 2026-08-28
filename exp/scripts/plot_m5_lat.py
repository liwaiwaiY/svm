#!/usr/bin/env python3
"""M5, two panels:
  Left : per-request latency (us, =1/IOPS) vs bs, x log / y linear, 3 curves.
         SVM latency grows much faster than KVM with bs (remote per-byte cost).
  Right: SVM P=off vs P=on latency, x log / y linear, on/off gap annotated.
         Linear y makes the on/off gap visible (2M: 671 vs 818us, +147us).

Values from 主总结 §5 authoritative table (verified vs exp/results/m5/*.bs.json).
Emits SVG (+PNG) for manual review only.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

H = "/home/waiai/svm"
OUT_SVG = os.path.join(H, "exp/results/m5/m5_bs_latency.svg")
OUT_PNG = os.path.join(H, "exp/results/m5/m5_bs_latency.png")

BS_KB = [16, 64, 256, 1024, 2048]          # x axis, KB
BS_LABEL = ["16K", "64K", "256K", "1M", "2M"]
LAT = {
    "KVM":       [19.8, 23.6, 27.3, 62.8, 88.5],
    "SVM P=off": [48.7, 72.2, 172.3, 506.1, 818.0],
    "SVM P=on":  [49.8, 74.8, 154.5, 375.6, 671.3],
}
COLORS = {"KVM": "#8c8c8c", "SVM P=off": "#4c72b0", "SVM P=on": "#dd8452"}
MARKERS = {"KVM": "D", "SVM P=off": "o", "SVM P=on": "s"}
SLOPES = {"KVM": 0.31, "SVM P=off": 0.58, "SVM P=on": 0.54}


def main():
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5))

    # --- left: log-log latency curves ---
    for name, y in LAT.items():
        ax1.plot(BS_KB, y, marker=MARKERS[name], lw=1.8, ms=6,
                 color=COLORS[name],
                 label=f"{name} (slope {SLOPES[name]:.2f})")
        ax1.annotate(f"{y[0]:.0f}", (BS_KB[0], y[0]),
                     textcoords="offset points", xytext=(0, 8),
                     ha="center", fontsize=8, color=COLORS[name])
        ax1.annotate(f"{y[-1]:.0f}", (BS_KB[-1], y[-1]),
                     textcoords="offset points", xytext=(6, 0),
                     ha="left", va="center", fontsize=8, color=COLORS[name])
    ax1.set_xscale("log")
    ax1.set_xticks(BS_KB)
    ax1.set_xticklabels(BS_LABEL)
    ax1.set_xlabel("bs (log scale)", fontsize=10)
    ax1.set_ylabel("per-request latency (us, =1/IOPS)", fontsize=10)
    ax1.set_title("(a) latency vs bs (x log, y linear)\nSVM I=256, iodepth=1, zc, M=4M",
                  fontsize=11)
    ax1.grid(axis="both", alpha=0.3, which="both")
    ax1.legend(loc="upper left", fontsize=9)

    # --- right: SVM P=off vs P=on latency, x log / y linear, gap annotated ---
    poff, pon = LAT["SVM P=off"], LAT["SVM P=on"]
    ax2.plot(BS_KB, poff, marker="o", lw=1.8, ms=6,
             color=COLORS["SVM P=off"], label="SVM P=off")
    ax2.plot(BS_KB, pon, marker="s", lw=1.8, ms=6,
             color=COLORS["SVM P=on"], label="SVM P=on")
    for i, (x, a, b) in enumerate(zip(BS_KB, poff, pon)):
        ax2.plot([x, x], [a, b], color="#777777", lw=0.8, ls=":")
        ax2.annotate(f"{a:.0f}", (x, a), textcoords="offset points",
                     xytext=(0, 6), ha="center", fontsize=8,
                     color=COLORS["SVM P=off"])
        ax2.annotate(f"{b:.0f}", (x, b), textcoords="offset points",
                     xytext=(0, -10), ha="center", fontsize=8,
                     color=COLORS["SVM P=on"])
        ax2.annotate(f"{a-b:.0f}", (x, (a + b) / 2), textcoords="offset points",
                     xytext=(4, 0), ha="left", va="center", fontsize=7.5,
                     color="#555555")
    ax2.set_xscale("log")
    ax2.set_xticks(BS_KB)
    ax2.set_xticklabels(BS_LABEL)
    ax2.set_xlabel("bs (log scale)", fontsize=10)
    ax2.set_ylabel("per-request latency (us)", fontsize=10)
    ax2.set_title("(b) P=off vs P=on latency\n(gap = Poff-Pon, us)",
                  fontsize=11)
    ax2.grid(axis="both", alpha=0.3, which="both")
    ax2.legend(loc="upper left", fontsize=9)

    fig.tight_layout()
    fig.savefig(OUT_PNG, dpi=150)
    fig.savefig(OUT_SVG)
    plt.close(fig)
    print("plot ->", OUT_SVG)
    print("plot ->", OUT_PNG)


if __name__ == "__main__":
    main()
