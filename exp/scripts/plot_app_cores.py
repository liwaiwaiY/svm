#!/usr/bin/env python3
"""App experiments: three separate grouped-bar charts, absolute seconds.

(1) app_pagerank_cores.* : PageRank total_s, cores {c1,c2,c4} x 4 protocols.
(2) app_als_cores.*      : ALS-IO total_s stacked (build bottom + train top).
(3) app_xgb_cores.*      : xgb total_s stacked (build bottom + train top).
KVM leftmost in each group. Values: 主总结 §9.1/9.2/9.6 tables.
Emits SVG (+PNG) for manual review only.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

H = "/home/waiai/svm"
OUT = os.path.join(H, "exp/results/app-net")

CORES = ["c1", "c2", "c4"]
PROTOS = ["KVM", "SVM", "NVMe-oF", "iSCSI"]
COLORS = {"KVM": "#8c8c8c", "SVM": "#4c72b0",
          "NVMe-oF": "#55a868", "iSCSI": "#c44e52"}

# PageRank total_s (RTT~150us)
PAGERANK = {
    "KVM":     [39.56, 27.27, 23.03],
    "SVM":     [39.18, 24.94, 21.67],
    "NVMe-oF": [38.24, 24.25, 20.85],
    "iSCSI":   [38.31, 24.64, 21.05],
}
# ALS-IO (build, train)
ALS = {
    "KVM":     [(28.11, 45.7), (16.80, 45.3), (11.65, 46.1)],
    "SVM":     [(30.32, 49.4), (15.30, 54.4), (9.38, 56.4)],
    "NVMe-oF": [(27.24, 51.6), (16.57, 50.6), (11.50, 50.8)],
    "iSCSI":   [(29.47, 117.0), (21.12, 118.5), (25.58, 118.7)],
}
# xgb (build, train)
XGB = {
    "KVM":     [(11.24, 44.13), (6.81, 43.79), (5.04, 44.34)],
    "SVM":     [(11.76, 48.24), (7.29, 50.37), (6.70, 50.54)],
    "NVMe-oF": [(10.91, 47.15), (7.08, 46.59), (5.26, 46.34)],
    "iSCSI":   [(13.18, 83.67), (9.21, 85.70), (9.50, 84.09)],
}


def style(ax):
    ax.set_xticks(np.arange(len(CORES)))
    ax.set_xticklabels(CORES)
    ax.set_xlabel("Spark cores", fontsize=10)
    ax.set_ylabel("total time (s, lower is better)", fontsize=10)
    ax.grid(axis="y", alpha=0.3)
    ax.legend(loc="upper right", fontsize=8.5, ncol=4)


def draw_simple(name, data, title, ylim):
    fig, ax = plt.subplots(figsize=(9, 5))
    x = np.arange(len(CORES))
    w = 0.2
    for i, p in enumerate(PROTOS):
        off = (i - 1.5) * w
        ax.bar(x + off, data[p], w, color=COLORS[p], label=p)
        for xi, v in zip(x, data[p]):
            ax.text(xi + off, v + 0.6, f"{v:.1f}", ha="center", fontsize=8,
                    color=COLORS[p])
    ax.set_ylim(*ylim)
    ax.set_title(title, fontsize=11)
    style(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "app_pagerank_cores.png"), dpi=150)
    fig.savefig(os.path.join(OUT, "app_pagerank_cores.svg"))
    plt.close(fig)
    print("plot ->", os.path.join(OUT, "app_pagerank_cores.svg"))


def draw_stacked(name, data, title, ylim):
    fig, ax = plt.subplots(figsize=(9, 5))
    x = np.arange(len(CORES))
    w = 0.2
    for i, p in enumerate(PROTOS):
        off = (i - 1.5) * w
        b = [bt[0] for bt in data[p]]
        t = [bt[1] for bt in data[p]]
        ax.bar(x + off, b, w, color=COLORS[p], alpha=0.5,
               label=f"{p} build")
        ax.bar(x + off, t, w, bottom=b, color=COLORS[p],
               label=f"{p} train")
        for xi, (bb, tt) in zip(x, zip(b, t)):
            ax.text(xi + off, bb + tt + 1.5, f"{bb + tt:.1f}",
                    ha="center", fontsize=8, color=COLORS[p])
    ax.set_ylim(*ylim)
    ax.set_title(title, fontsize=11)
    style(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, f"app_{name}_cores.png"), dpi=150)
    fig.savefig(os.path.join(OUT, f"app_{name}_cores.svg"))
    plt.close(fig)
    print("plot ->", os.path.join(OUT, f"app_{name}_cores.svg"))


def main():
    draw_simple("pagerank", PAGERANK,
                "PageRank @ RTT~150us: total time by cores", (0, 45))
    draw_stacked("als", ALS,
                 "ALS-IO: total time stacked (build + train)", (0, 165))
    draw_stacked("xgb", XGB,
                 "xgb (forced rescan): total time stacked (build + train)",
                 (0, 105))


if __name__ == "__main__":
    main()
