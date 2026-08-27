#!/usr/bin/env python3
"""ALS-BIN 并行 build 四协议 cores 扩展图（build_s vs c1/c2/c4）。
数据: exp/results/app2/als-{kvm,svm,nvmeof,iscsi}/logs/*.log
用法: python3 plot_als_build.py [out.png]
"""
import re, sys, glob, os
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

H = "/home/waiai/svm/exp/results/app2"
PROTOS = ["kvm", "svm", "nvmeof", "iscsi"]
CORES = [1, 2, 4]
COLORS = {"kvm": "#1f77b4", "svm": "#d62728", "nvmeof": "#2ca02c", "iscsi": "#ff7f0e"}

def build_time(proto, c):
    logs = glob.glob(f"{H}/als-{proto}/logs/*-c{c}-r1.log")
    if not logs:
        return None
    m = re.search(r"build_s=([\d.]+)", open(logs[0]).read())
    return float(m.group(1)) if m else None

data = {}
for p in PROTOS:
    data[p] = [build_time(p, c) for c in CORES]

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.2))

# 左：build_s vs cores 折线
for p in PROTOS:
    ax1.plot(CORES, data[p], "o-", color=COLORS[p], lw=2, label=p.upper())
ax1.set_xlabel("cores (build split streams)")
ax1.set_ylabel("build time (s), cold read 2.4GB, lower=better")
ax1.set_xticks(CORES)
ax1.set_title("ALS-BIN build: cores scaling (4 protocols)")
ax1.grid(alpha=0.3)
ax1.legend()
for p in PROTOS:
    for c, v in zip(CORES, data[p]):
        ax1.annotate(f"{v:.2f}", (c, v), textcoords="offset points",
                     xytext=(0, 7 if p != "iscsi" else -16), ha="center",
                     fontsize=8, color=COLORS[p])

# 右：c4/c1 归一化扩展比
ratio = {p: data[p][2] / data[p][0] for p in PROTOS}
xs = range(len(PROTOS))
bars = ax2.bar(xs, [ratio[p] for p in PROTOS], color=[COLORS[p] for p in PROTOS])
ax2.axhline(1.0, color="gray", ls="--", lw=1)
ax2.set_xticks(list(xs))
ax2.set_xticklabels([p.upper() for p in PROTOS])
ax2.set_ylabel("c4/c1 time ratio (<1 scales well, >1 negative)")
ax2.set_title("Core scaling: c4 vs c1")
for i, p in enumerate(PROTOS):
    ax2.annotate(f"{ratio[p]:.2f}×", (i, ratio[p]), textcoords="offset points",
                 xytext=(0, 5), ha="center", fontsize=10)
ax2.grid(axis="y", alpha=0.3)

plt.tight_layout()
out = sys.argv[1] if len(sys.argv) > 1 else f"{H}/als_build_cores.png"
plt.savefig(out, dpi=130)
print(f"saved {out}")
print("build_s:", {p: data[p] for p in PROTOS})
print("c4/c1:", {p: f"{ratio[p]:.2f}x" for p in PROTOS})
