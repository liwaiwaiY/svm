#!/usr/bin/env python3
"""M5 difference curve: y = lat(P=off) - lat(P=on), per-request latency (us).

x = bs (log), y linear, 0 reference line. Positive = P=on is faster (benefit).
  -1.1 / -2.6 (<=64K, noise)  +17.8 / +130.5 / +146.7 (256K..2M, benefit).
Benefit only appears at bs>=256K.

Values from 主总结 §5 authoritative table (verified vs exp/results/m5/*.bs.json).
Emits SVG (+PNG) for manual review only.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

H = "/home/waiai/svm"
OUT_SVG = os.path.join(H, "exp/results/m5/m5_pdiff.svg")
OUT_PNG = os.path.join(H, "exp/results/m5/m5_pdiff.png")

BS_KB = [16, 64, 256, 1024, 2048]          # x axis, KB
BS_LABEL = ["16K", "64K", "256K", "1M", "2M"]
LAT = {
    "SVM P=off": [48.7, 72.2, 172.3, 506.1, 818.0],
    "SVM P=on":  [49.8, 74.8, 154.5, 375.6, 671.3],
}
COLOR = "#4c72b0"


def main():
    poff, pon = LAT["SVM P=off"], LAT["SVM P=on"]
    diff = [a - b for a, b in zip(poff, pon)]  # Poff - Pon

    fig, ax = plt.subplots(figsize=(8.5, 5))
    ax.plot(BS_KB, diff, marker="o", lw=1.8, ms=6, color=COLOR,
            label="lat(P=off) - lat(P=on)")
    ax.axhline(0, color="black", lw=0.9)
    for x, d in zip(BS_KB, diff):
        ax.annotate(f"{d:+.1f}", (x, d), textcoords="offset points",
                    xytext=(0, 7 if d >= 0 else -12), ha="center", fontsize=9,
                    fontweight="bold",
                    color="#55a868" if d > 0 else "#c44e52")

    ax.set_xscale("log")
    ax.set_xticks(BS_KB)
    ax.set_xticklabels(BS_LABEL)
    ax.set_xlabel("bs (log scale)", fontsize=10)
    ax.set_ylabel("lat(P=off) - lat(P=on) (us)", fontsize=10)
    ax.set_title("M5: P=off minus P=on latency by bs\n"
                 "(positive = P=on faster; SVM I=256, iodepth=1, zc, M=4M)",
                 fontsize=11)
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
