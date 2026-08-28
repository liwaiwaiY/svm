#!/usr/bin/env python3
"""M3 RTT sweep line chart: total IOPS (read+write) vs RTT, log x-axis.

Curves: SVM-I16, SVM-I32 (m3rtt3/), NVMe-oF, iSCSI (m3net2/).
KVM has no RTT injection -> flat reference line at doc value 65.1K
(measured 69.1K @0.1ms / 60.3K @2ms, mean ~64.7K; 主总结 §4.1 uses 65.1K).

Values verified against raw json (2-run means, randrw 70/30, 4K, iodepth=32):
  0.1ms: I16 29.1  I32 53.5  NVMe 48.2  iSCSI 61.4
  0.2ms: I16 24.7  I32 47.6  NVMe 50.2  iSCSI 57.4
  0.5ms: I16 16.3  I32 33.9  NVMe 37.4  iSCSI 40.3
  1ms:   I16 9.3   I32 23.2  NVMe 29.7  iSCSI 26.8
  2ms:   I16 5.8   I32 14.1  NVMe 15.1  iSCSI 13.8
All match 主总结 §4.1 authoritative table.

Emits SVG (+PNG) for manual review only.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

H = "/home/waiai/svm"
OUT_SVG = os.path.join(H, "exp/results/m3rtt3/m3_rtt_iops.svg")
OUT_PNG = os.path.join(H, "exp/results/m3rtt3/m3_rtt_iops.png")

RTT = [0.1, 0.2, 0.5, 1.0, 2.0]  # ms
SERIES = {
    "SVM-I16":  [29.1, 24.7, 16.3, 9.3, 5.8],
    "SVM-I32":  [53.5, 47.6, 33.9, 23.2, 14.1],
    "NVMe-oF":  [48.2, 50.2, 37.4, 29.7, 15.1],
    "iSCSI":    [61.4, 57.4, 40.3, 26.8, 13.8],
}
COLORS = {"SVM-I16": "#4c72b0", "SVM-I32": "#dd8452",
          "NVMe-oF": "#55a868", "iSCSI": "#c44e52", "KVM": "#8c8c8c"}
MARKERS = {"SVM-I16": "o", "SVM-I32": "s", "NVMe-oF": "^", "iSCSI": "v"}
KVM_IOPS = 65.1  # K


def main():
    fig, ax = plt.subplots(figsize=(8.5, 5.2))
    for name, y in SERIES.items():
        ax.plot(RTT, y, marker=MARKERS[name], lw=1.8, ms=6,
                color=COLORS[name], label=name)
        for x, v in zip(RTT, y):
            ax.annotate(f"{v:.0f}", (x, v), textcoords="offset points",
                        xytext=(0, 7), ha="center", fontsize=7.5, color=COLORS[name])

    # KVM: flat reference line (no RTT injection), doc value 65.1K
    ax.axhline(KVM_IOPS, color=COLORS["KVM"], lw=1.6, ls="--",
               label=f"KVM {KVM_IOPS:.1f}K (no RTT)")
    ax.text(2.0, KVM_IOPS + 1.5, f"{KVM_IOPS:.1f}K", ha="center", fontsize=8,
            color=COLORS["KVM"])

    ax.set_xscale("log")
    ax.set_xticks(RTT)
    ax.set_xticklabels([f"{r:.1f}" for r in RTT])
    ax.set_xlabel("RTT (ms, log scale)", fontsize=10)
    ax.set_ylabel("total IOPS (K, read+write)", fontsize=10)
    ax.set_title("M3: IOPS vs RTT (randrw 70/30, 4K, iodepth=32, numjobs=1, 60s)",
                 fontsize=11)
    ax.grid(axis="both", alpha=0.3)
    ax.legend(loc="upper right", fontsize=9)
    fig.tight_layout()
    fig.savefig(OUT_PNG, dpi=150)
    fig.savefig(OUT_SVG)
    plt.close(fig)
    print("plot ->", OUT_SVG)
    print("plot ->", OUT_PNG)


if __name__ == "__main__":
    main()
