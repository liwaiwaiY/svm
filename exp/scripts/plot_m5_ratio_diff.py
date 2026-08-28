#!/usr/bin/env python3
"""M5 P=on benefit bar: y = (lat(P=off) - lat(P=on)) / lat(P=off) * 100.

Positive = P=on is faster. Benefit appears only at bs>=256K:
  -2.3% / -3.6% (<=64K, noise)   +10.3% / +25.8% / +17.9% (256K..2M).

Values from 主总结 §5 authoritative table (verified vs exp/results/m5/*.bs.json).
Emits SVG (+PNG) for manual review only.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

H = "/home/waiai/svm"
OUT_SVG = os.path.join(H, "exp/results/m5/m5_ratio_diff.svg")
OUT_PNG = os.path.join(H, "exp/results/m5/m5_ratio_diff.png")

BS_LABEL = ["16K", "64K", "256K", "1M", "2M"]
LAT = {
    "SVM P=off": [48.7, 72.2, 172.3, 506.1, 818.0],
    "SVM P=on":  [49.8, 74.8, 154.5, 375.6, 671.3],
}


def main():
    poff, pon = LAT["SVM P=off"], LAT["SVM P=on"]
    benef = [(a - b) / a * 100.0 for a, b in zip(poff, pon)]

    fig, ax = plt.subplots(figsize=(8.5, 5))
    colors = ["#55a868" if d > 0 else "#c44e52" for d in benef]
    ax.bar(BS_LABEL, benef, 0.55, color=colors, edgecolor="black", lw=0.4)
    ax.axhline(0, color="black", lw=0.9)
    for i, d in enumerate(benef):
        ax.text(i, d + (1.2 if d >= 0 else -1.2),
                f"{d:+.1f}%", ha="center",
                va="bottom" if d >= 0 else "top",
                fontsize=9, fontweight="bold",
                color="#55a868" if d > 0 else "#c44e52")

    ax.set_ylim(-10, 32)
    ax.set_xlabel("bs", fontsize=10)
    ax.set_ylabel("(lat(P=off)-lat(P=on))/lat(P=off) (%)", fontsize=10)
    ax.set_title("M5: P=on benefit over P=off by bs\n"
                 "(positive = P=on faster; SVM I=256, iodepth=1, zc, M=4M)",
                 fontsize=11)
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(OUT_PNG, dpi=150)
    fig.savefig(OUT_SVG)
    plt.close(fig)
    print("plot ->", OUT_SVG)
    print("plot ->", OUT_PNG)


if __name__ == "__main__":
    main()
