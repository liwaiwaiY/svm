#!/usr/bin/env python3
"""M1 mem violin: distribution of SVM RSS increase over KVM (per sample).

Reads exp/results/m1mem/points.csv (tag,bs,mode,local_rss_kB,stub_rss_kB,skmem_B).
Per sample: total_kB = local + stub + skmem/1024.
Per cell:   pct = (total_sample - mean(KVM total of same bs)) / mean(KVM total) * 100.
X = 12 cells grouped by bs (zc / copy / KVM), Y = % vs KVM (same bs).
KVM cells are all-zero -> drawn as a flat gray line (not a violin).

Emits exp/results/m1mem/m1mem_violin_pct_kvm.png (manual review only).
"""
import os
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

H = "/home/waiai/svm"
CSV_PATH = os.path.join(H, "exp/results/m1mem/points.csv")
OUT_PCT = os.path.join(H, "exp/results/m1mem/m1mem_violin_pct_kvm.png")
OUT_ABS = os.path.join(H, "exp/results/m1mem/m1mem_violin_abs.png")

BS = ("64k", "512k", "1m", "2m")
MODES = ("svm_z0", "svm_z4m", "kvm")
MODE_LABEL = {"svm_z0": "zc=0", "svm_z4m": "copy(4M)", "kvm": "KVM"}
COLOR = {"svm_z0": "#4c72b0", "svm_z4m": "#dd8452", "kvm": "#8c8c8c"}

CELLS = [(bs, mode) for bs in BS for mode in MODES]


def make_fig(pct_data, abs_data, out, metric):
    """metric='pct': y = % vs KVM same-bs baseline; metric='abs': y = total MB."""
    fig, ax = plt.subplots(figsize=(11, 5))
    xpos = []
    labels = []
    for i, (bs, mode) in enumerate(CELLS):
        x = i
        xpos.append(x)
        v = pct_data[(bs, mode)] if metric == "pct" else abs_data[(bs, mode)]
        if mode == "kvm":
            ref = 0.0 if metric == "pct" else v[0]
            ax.plot([x - 0.32, x + 0.32], [ref, ref], color=COLOR[mode], lw=3,
                    solid_capstyle="butt", label="KVM (baseline)" if i == 2 else "")
        else:
            parts = ax.violinplot([v], positions=[x], widths=0.62,
                                  showmeans=False, showmedians=False)
            body = parts["bodies"][0]
            body.set_facecolor(COLOR[mode])
            body.set_alpha(0.55)
            body.set_edgecolor(COLOR[mode])
            m = sum(v) / len(v)
            ax.scatter([x], [m], color=COLOR[mode], marker="D", s=26, zorder=5,
                       edgecolor="white", linewidths=0.6)
            if metric == "abs":
                ax.text(x, max(v) + 2, f"{m:.1f}", ha="center", va="bottom",
                        fontsize=7, color=COLOR[mode])
        labels.append(f"{bs}\n{MODE_LABEL[mode]}")

    ax.set_xticks(xpos)
    ax.set_xticklabels(labels, fontsize=8)
    if metric == "pct":
        ax.axhline(0, color="black", lw=0.8, ls="--")
        ax.set_ylabel("RSS total vs KVM (%)", fontsize=10)
        title = ("per-sample distribution of SVM RSS increase over KVM\n"
                 "(total = local+stub+skmem; fio randrw 7:3 d=1 I=256 P=off; n=60/cell)")
    else:
        ax.set_ylabel("RSS total (MB)", fontsize=10)
        title = ("per-sample distribution of absolute RSS total (MB)\n"
                 "(total = local+stub+skmem; fio randrw 7:3 d=1 I=256 P=off; n=60/cell)")
    ax.set_title("M1 mem: " + title, fontsize=11)
    ax.grid(axis="y", alpha=0.3)
    ax.legend(loc="upper right", fontsize=8, ncol=3)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print("plot ->", out)


def main():
    rows = []
    with open(CSV_PATH) as f:
        for r in csv.DictReader(f):
            rows.append({
                "tag": r["tag"],
                "bs": r["bs"],
                "local": float(r["local_rss_kB"]),
                "stub": float(r["stub_rss_kB"]),
                "skmem": float(r["skmem_B"] or 0.0),
            })

    # KVM total baseline per bs (mean over samples; KVM stub=0, skmem=0)
    kv_base = {}
    for bs in BS:
        tot = [r["local"] + r["stub"] + r["skmem"] / 1024.0
               for r in rows if r["tag"] == "kvm_" + bs]
        kv_base[bs] = sum(tot) / len(tot) if tot else 0.0

    # per-sample values, grouped by cell (bs, mode): pct vs KVM, and absolute MB
    pct = {c: [] for c in CELLS}
    absv = {c: [] for c in CELLS}
    for r in rows:
        mode = "kvm" if r["tag"].startswith("kvm") else (
            "svm_z0" if "z0" in r["tag"] else "svm_z4m")
        bs = r["bs"]
        tot = r["local"] + r["stub"] + r["skmem"] / 1024.0
        pct[(bs, mode)].append((tot - kv_base[bs]) / kv_base[bs] * 100.0)
        absv[(bs, mode)].append(tot / 1024.0)

    make_fig(pct, absv, OUT_PCT, "pct")
    make_fig(pct, absv, OUT_ABS, "abs")


if __name__ == "__main__":
    main()
