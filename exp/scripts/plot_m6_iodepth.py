#!/usr/bin/env python3
"""M6 line chart: IOPS vs guest iodepth, one line per stub_queue_max Q.

x = iodepth (categorical, 4 points evenly spaced), y = IOPS (K, linear).
  Q=1: 5.5-7.2K flat  (strict serial, device concurrency locked)
  Q>=16: iodepth>=16 unlocks disk parallelism (7K -> 55-56K, ~10x jump)
  Q=64/I=128: 92.2K peak (window full, zero slack)
  KVM-equivalent: 7.47-101.4K, always above SVM at same iodepth.

Values: 主总结 §7 authoritative table (L159-164).
Emits SVG (+PNG) for manual review only.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

H = "/home/waiai/svm"
OUT_SVG = os.path.join(H, "exp/results/m5/m6_iodepth_curves.svg")
OUT_PNG = os.path.join(H, "exp/results/m5/m6_iodepth_curves.png")

IODEPTH = [1, 16, 32, 64]
SERIES = {
    "Q=1":        [5.5, 7.0, 7.1, 7.2],
    "Q=16":       [5.5, 55.6, 66.7, 71.2],
    "Q=32":       [5.5, 55.4, 77.0, 82.8],
    "Q=64 (I=128)": [5.5, 55.7, 77.2, 92.2],
    "KVM-equiv":  [7.47, 65.3, 88.5, 101.4],
}
COLORS = {"Q=1": "#c44e52", "Q=16": "#4c72b0", "Q=32": "#dd8452",
          "Q=64 (I=128)": "#55a868", "KVM-equiv": "#8c8c8c"}
MARKERS = {"Q=1": "o", "Q=16": "s", "Q=32": "^", "Q=64 (I=128)": "v",
           "KVM-equiv": "D"}
LS = {"KVM-equiv": "--"}


def main():
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    XCAT = list(range(len(IODEPTH)))
    for name, y in SERIES.items():
        ax.plot(XCAT, y, marker=MARKERS[name], lw=1.8, ms=6,
                color=COLORS[name], ls=LS.get(name, "-"), label=name)
        # annotate last point (iodepth=64) only
        ax.annotate(f"{y[-1]:.0f}", (XCAT[-1], y[-1]),
                    textcoords="offset points", xytext=(6, 0),
                    ha="left", va="center", fontsize=8, color=COLORS[name])
    # annotate the ~10x unlock jump at iodepth=16 (cat index 1) for Q=16
    ax.annotate("Q>=16 unlock disk\nparallelism (~10x)", xy=(1, 55.6),
                xytext=(1.5, 30), fontsize=8.5, color="#4c72b0",
                arrowprops=dict(arrowstyle="->", color="#4c72b0", lw=0.8))
    ax.annotate("Q=1: strict serial\n(device concurrency locked)",
                xy=(1, 7.0), xytext=(-0.55, 16), fontsize=8.5, color="#c44e52",
                arrowprops=dict(arrowstyle="->", color="#c44e52", lw=0.8))

    ax.set_xticks(range(len(IODEPTH)))
    ax.set_xticklabels([str(d) for d in IODEPTH])
    ax.set_xlabel("guest iodepth (categorical)", fontsize=10)
    ax.set_ylabel("IOPS (K, randread 4K)", fontsize=10)
    ax.set_ylim(0, 115)
    ax.set_title("M6: IOPS vs iodepth, by stub_queue_max Q\n"
                 "(randread 4K, Q=64 uses I=128)", fontsize=11)
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
