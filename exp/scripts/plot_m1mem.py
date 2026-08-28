#!/usr/bin/env python3
"""M1 mem stacked bar: per-bs figure, local RSS (bottom) + stub RSS (top).

Reads exp/results/m1mem/<tag>.samples (60 samples: ts local_kB stub_kB skmem_B).
Each bar = mean local + mean stub (skmem excluded from bars; see summary txt).
Emits PNG+SVG (manual review, do NOT feed to the model) + text summary.
"""
import os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

H = "/home/waiai/svm"
OUT = os.path.join(H, "exp/results/m1mem")
TXT = os.path.join(OUT, "m1mem_summary.txt")

CELLS = ["svm_z0_64k", "svm_z0_512k", "svm_z0_1m", "svm_z0_2m",
         "svm_z4m_64k", "svm_z4m_512k", "svm_z4m_1m", "svm_z4m_2m",
         "kvm_64k", "kvm_512k", "kvm_1m", "kvm_2m"]
BS = ("64k", "512k", "1m", "2m")
BS_PLOT = ("64k", "512k", "1m")   # plots exclude 2M (zc/copy 反号 + 大块噪声)
LABELS = {
    "svm_z0_64k":  "SVM zc=0 64K",
    "svm_z0_512k": "SVM zc=0 512K",
    "svm_z0_1m":   "SVM zc=0 1M",
    "svm_z0_2m":   "SVM zc=0 2M",
    "svm_z4m_64k": "SVM zc=4M 64K",
    "svm_z4m_512k":"SVM zc=4M 512K",
    "svm_z4m_1m":  "SVM zc=4M 1M",
    "svm_z4m_2m":  "SVM zc=4M 2M",
    "kvm_64k":     "KVM 64K",
    "kvm_512k":    "KVM 512K",
    "kvm_1m":      "KVM 1M",
    "kvm_2m":      "KVM 2M",
}

def _f(x):
    try:
        return float(x)
    except (ValueError, TypeError):
        return 0.0

def load_samples(tag):
    """-> list of (local_kB, stub_kB, skmem_B); stub=0 for KVM."""
    rows = []
    with open(os.path.join(OUT, tag + ".samples")) as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.split()
            if len(p) == 3:          # KVM lines: ts local skmem (no stub)
                p = [p[0], p[1], "0", p[2]]
            rows.append((_f(p[1]), _f(p[2]), _f(p[3])))
    return rows

def mean(v):
    return sum(v) / len(v) if v else 0.0

def med(v):
    s = sorted(v)
    return s[len(s) // 2]

def stacked_bar(ax, data, bs):
    """Stacked bars for one bs: local (bottom) + stub (top), one bar per cell.
    Max whisker = max over samples of (local+stub+skmem/1024)."""
    cells = [c for c in CELLS if c.endswith(bs)]
    loc = [mean(data[c]["local"]) for c in cells]
    stb = [mean(data[c]["stub"]) for c in cells]
    tot = [l + s for l, s in zip(loc, stb)]
    x = range(len(cells))
    ax.bar(x, loc, 0.6, color="#4c72b0", label="local RSS")
    ax.bar(x, stb, 0.6, bottom=loc, color="#dd8452", label="remote stub RSS")
    for i, (c, l, s, t) in enumerate(zip(cells, loc, stb, tot)):
        samples = [a + b + k / 1024.0 for a, b, k
                   in zip(data[c]["local"], data[c]["stub"], data[c]["skmem"])]
        mx = max(samples)
        ax.vlines(i, t, mx, color="#333333", lw=1)
        ax.plot([i - 0.12, i + 0.12], [mx, mx], color="#333333", lw=1)
        ax.text(i, l / 2, f"{l:,.0f}", ha="center", va="center",
                fontsize=7, color="white")
        if s > 0:
            ax.text(i, l + s / 2, f"{s:,.0f}", ha="center", va="center",
                    fontsize=7, color="white")
        ax.text(i, t, f"{t:,.0f}", ha="center", va="bottom", fontsize=8)
    ax.set_xticks(x)
    ax.set_xticklabels([LABELS[c].replace("SVM ", "").replace("KVM ", "")
                        for c in cells], fontsize=8)
    ax.set_title(f"bs = {bs}", fontsize=10)
    ax.grid(axis="y", alpha=0.3)
    return tot

def combined_fig(data, out):
    """One figure, N panels (per bs, BS_PLOT only), stacked absolute RSS bars."""
    fig, axes = plt.subplots(1, len(BS_PLOT), figsize=(15, 4.2), sharey=True)
    for ax, bs in zip(axes, BS_PLOT):
        stacked_bar(ax, data, bs)
    axes[0].set_ylabel("kB")
    fig.suptitle("M1 mem: virtio-remote RSS (local bottom + stub top) per bs\n"
                 "fio randrw 7:3 d=1 I=256 P=off", fontsize=12)
    axes[0].legend(loc="upper right", fontsize=8)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(out, dpi=150)
    fig.savefig(out.replace(".png", ".svg"))
    plt.close(fig)
    print("plot ->", out)

def pct_fig(data, out):
    """One merged chart: % RSS(local+stub) increase over KVM, 6 bars (3 bs x zc/copy, 2M excluded).
    Stacked: local-increase over KVM (bottom) + stub (top).
    Max whisker = max over samples of (local+stub+skmem/1024) pct vs KVM."""
    order = [(bs, m) for bs in BS_PLOT for m in ("z0", "z4m")]
    fig, ax = plt.subplots(figsize=(9, 4.5))
    for i, (bs, mode) in enumerate(order):
        kv = mean(data["kvm_" + bs]["local"])
        d = data[f"svm_{mode}_{bs}"]
        loc_p = (mean(d["local"]) - kv) / kv * 100
        stb_p = mean(d["stub"]) / kv * 100
        tot_p = loc_p + stb_p
        samples = [a + b + k / 1024.0 for a, b, k
                   in zip(d["local"], d["stub"], d["skmem"])]
        mx_p = max((s - kv) / kv * 100 for s in samples)
        ax.bar(i, loc_p, 0.7, color="#4c72b0", label="local inc. vs KVM" if i == 0 else "")
        ax.bar(i, stb_p, 0.7, bottom=loc_p, color="#dd8452", label="remote stub" if i == 0 else "")
        ax.vlines(i, tot_p, mx_p, color="#333333", lw=1)
        ax.plot([i - 0.12, i + 0.12], [mx_p, mx_p], color="#333333", lw=1)
        if loc_p > 3:
            ax.text(i, loc_p / 2, f"{loc_p:.1f}", ha="center", va="center",
                    fontsize=7, color="white")
        if stb_p > 3:
            ax.text(i, loc_p + stb_p / 2, f"{stb_p:.1f}", ha="center", va="center",
                    fontsize=7, color="white")
        ax.text(i, tot_p + 0.6, f"{tot_p:.1f}%", ha="center", fontsize=8)
    ax.axhline(0, color="black", lw=0.8)
    ax.set_xticks(range(len(order)))
    ax.set_xticklabels([f"{bs} {('zc' if m == 'z0' else 'copy')}" for bs, m in order],
                       fontsize=8)
    ax.set_ylabel("% of KVM RSS (local+stub)")
    ax.set_title("SVM virtio-remote RSS (local+stub) vs KVM: % increase\n"
                 "fio randrw 7:3 d=1 I=256 P=off", fontsize=11)
    ax.grid(axis="y", alpha=0.3)
    ax.legend(loc="upper left", fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    fig.savefig(out.replace(".png", ".svg"))
    plt.close(fig)
    print("plot ->", out)

def main():
    data = {c: {"local": [], "stub": [], "skmem": []} for c in CELLS}
    for c in CELLS:
        for local, stub, skmem in load_samples(c):
            data[c]["local"].append(local)
            data[c]["stub"].append(stub)
            data[c]["skmem"].append(skmem)
    miss = [c for c in CELLS if not data[c]["local"]]
    if miss:
        print("MISSING DATA for:", miss); sys.exit(1)

    # one figure with 4 panels (per bs) + one merged %-vs-KVM chart
    combined_fig(data, os.path.join(OUT, "m1mem_bar_all.png"))
    pct_fig(data, os.path.join(OUT, "m1mem_pct_kvm.png"))

    # text summary (mean/max, no perf)
    def stats(vals):
        n = len(vals)
        return (sum(vals) / n, max(vals)) if n else (0.0, 0.0)

    with open(TXT, "w") as o:
        def w(s=""): print(s, file=o)
        w("== M1 mem: mean/max (kB; virtio-remote total = local+stub+skmem) ==")
        w(f"{'cell':<14} {'total-mean':>10} {'total-max':>10} {'local-mean':>11} {'local-max':>10} {'stub-mean':>10} {'stub-max':>9} {'skmem-mean':>11} {'skmem-max':>10}")
        for c in CELLS:
            d = data[c]
            tot = [l + s + k / 1024.0 for l, s, k in zip(d["local"], d["stub"], d["skmem"])]
            m = {k: stats(v) for k, v in
                 [("total", tot), ("local", d["local"]), ("stub", d["stub"]), ("skmem", d["skmem"])]}
            w(f"{c:<14} {m['total'][0]:>9.0f} {m['total'][1]:>9.0f} {m['local'][0]:>11.0f} {m['local'][1]:>10.0f} {m['stub'][0]:>10.0f} {m['stub'][1]:>9.0f} {m['skmem'][0]:>11.0f} {m['skmem'][1]:>10.0f}  (n={len(d['local'])})")
        w("")
        w("-- zc effect (skmem, kernel socket buffer, max) --")
        for bs in BS:
            z0, z4 = "svm_z0_" + bs, "svm_z4m_" + bs
            s0, s4 = med(data[z0]["skmem"]), med(data[z4]["skmem"])
            w(f"  bs={bs}: zc=0 med {s0:.0f} B vs zc=4M med {s4:.0f} B")
        w("")
        w("-- SVM vs KVM: RSS(local+stub) increase % (mean) --")
        for bs in BS:
            kv = mean(data["kvm_" + bs]["local"])
            parts = []
            for mode in ("z0", "z4m"):
                d = data[f"svm_{mode}_{bs}"]
                loc_p = (mean(d["local"]) - kv) / kv * 100
                stb_p = mean(d["stub"]) / kv * 100
                lab = "zc" if mode == "z0" else "copy"
                parts.append(f"{lab}: +{loc_p + stb_p:.1f}% (local {loc_p:+.1f}%, "
                             f"stub {stb_p:.1f}%)")
            w(f"  bs={bs}: " + " | ".join(parts))
    print("summary ->", TXT)

if __name__ == "__main__":
    main()
