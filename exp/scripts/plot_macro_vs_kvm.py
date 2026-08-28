#!/usr/bin/env python3
"""Macro: two separate line charts, absolute values.

(a) macro_txn_abs.* : txns/s (absolute), 4 protocols vs threads.
(b) macro_p95_abs.* : p95 transaction latency ms (absolute), 4 protocols.
x = threads {1,2,4} (categorical), KVM-b = gray dashed baseline.

Values: 主总结 §8.1 r1 authoritative tables (L184-209).
Emits SVG (+PNG) for manual review only.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

H = "/home/waiai/svm"
OUT = os.path.join(H, "exp/results/macro-b")

THREADS = [1, 2, 4]
XCAT = list(range(len(THREADS)))
PROTOS = ["KVM-b", "SVM-b", "NVMe-oF", "iSCSI"]
COLORS = {"KVM-b": "#8c8c8c", "SVM-b": "#4c72b0",
          "NVMe-oF": "#55a868", "iSCSI": "#c44e52"}
MARKERS = {"KVM-b": "D", "SVM-b": "o", "NVMe-oF": "s", "iSCSI": "^"}
LS = {"KVM-b": "--"}

# r1 absolute values
TXN = {
    "KVM-b":   [240.0, 298.7, 446.3],
    "SVM-b":   [185.6, 252.2, 248.5],
    "NVMe-oF": [158.3, 221.5, 273.8],
    "iSCSI":   [109.3, 171.6, 249.5],
}
P95 = {
    "KVM-b":   [8.13, 13.70, 20.74],
    "SVM-b":   [10.65, 17.63, 44.17],
    "NVMe-oF": [13.22, 20.74, 37.56],
    "iSCSI":   [30.26, 38.94, 45.79],
}


def draw(name, data, ylabel, title, ylim, fmt):
    fig, ax = plt.subplots(figsize=(8, 5))
    for p in PROTOS:
        v = data[p]
        ax.plot(XCAT, v, marker=MARKERS[p], lw=1.8, ms=6,
                color=COLORS[p], ls=LS.get(p, "-"), label=p)
        for x, val in zip(XCAT, v):
            ax.annotate(f"{val:{fmt}}", (x, val), textcoords="offset points",
                        xytext=(0, -13), ha="center", fontsize=8,
                        color=COLORS[p])
    ax.set_xticks(XCAT)
    ax.set_xticklabels([f"t={t}" for t in THREADS])
    ax.set_xlabel("threads", fontsize=10)
    ax.set_ylabel(ylabel, fontsize=10)
    ax.set_ylim(*ylim)
    ax.set_title(title, fontsize=11)
    ax.grid(axis="both", alpha=0.3)
    ax.legend(loc="upper left", fontsize=9)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, f"{name}.png"), dpi=150)
    fig.savefig(os.path.join(OUT, f"{name}.svg"))
    plt.close(fig)
    print("plot ->", os.path.join(OUT, f"{name}.svg"))


def main():
    draw("macro_txn_abs", TXN, "txns/s (r1)", "Macro TPCC: throughput (r1)",
         (0, 480), ".0f")
    draw("macro_p95_abs", P95, "p95 transaction latency (ms, r1)",
         "Macro TPCC: p95 latency (r1)", (0, 52), ".1f")


if __name__ == "__main__":
    main()
