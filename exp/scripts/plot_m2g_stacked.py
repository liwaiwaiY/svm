#!/usr/bin/env python3
"""M2G grouped bar chart: IOPS by workload, 4 side-by-side bars each.

x = 4 workloads (randrd/randwt/seqrd/seqwt), each group has 4 bars:
  W=1 (t4), W=2 (t5), W=4 (t0), KVM.
Values: 主总结 §3 authoritative table.
NOTE: raw m2g means differ for (randrd, seqwt) t5/t0 rows (swapped) and the
KVM row (mostly matches kvm_once/) -- awaiting user confirmation.
Emits SVG (+PNG) for manual review only.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

H = "/home/waiai/svm"
OUT_SVG = os.path.join(H, "exp/results/m2g/m2g_grouped_iops.svg")
OUT_PNG = os.path.join(H, "exp/results/m2g/m2g_grouped_iops.png")

WORKLOADS = ["randrd", "randwt", "seqrd", "seqwt"]
SEGS = ["W=1", "W=2", "W=4", "KVM"]
DATA = {
    "randrd": [153.7, 175.9, 189.5, 199.0],
    "randwt": [40.3, 73.5, 94.2, 96.0],
    "seqrd":  [89.0, 135.9, 148.3, 220.0],
    "seqwt":  [67.8, 87.0, 96.6, 122.0],
}
COLORS = {"W=1": "#c8daf0", "W=2": "#9fc5e8", "W=4": "#6a9fd4",
          "KVM": "#8c8c8c"}


def main():
    fig, ax = plt.subplots(figsize=(9.5, 5.2))
    x = np.arange(len(WORKLOADS))
    w = 0.19
    for j, seg in enumerate(SEGS):
        off = (j - 1.5) * w
        vals = [DATA[wl][j] for wl in WORKLOADS]
        ax.bar(x + off, vals, w, color=COLORS[seg], label=seg)
        for xi, v in zip(x, vals):
            ax.text(xi + off, v + 3, f"{v:.0f}", ha="center", fontsize=7.5,
                    color="#333333")
    ax.set_xticks(x)
    ax.set_xticklabels(WORKLOADS)
    ax.set_ylim(0, 245)
    ax.set_ylabel("IOPS (K)", fontsize=10)
    ax.set_title("M2G: IOPS by workload (randread/randwrite/seqread/seqwrite, 4K)\n"
                 "W=1/2/4 (t4/t5/t0) vs KVM", fontsize=11)
    ax.grid(axis="y", alpha=0.3)
    ax.legend(loc="upper left", fontsize=9)
    fig.tight_layout()
    fig.savefig(OUT_PNG, dpi=150)
    fig.savefig(OUT_SVG)
    plt.close(fig)
    print("plot ->", OUT_SVG)
    print("plot ->", OUT_PNG)


if __name__ == "__main__":
    main()
