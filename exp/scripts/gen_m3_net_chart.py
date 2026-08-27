#!/usr/bin/env python3
"""Generate M3 network RTT-IOPS SVG curves (no matplotlib dependency)."""
import json, statistics, os

RES = "/home/waiai/svm/exp/results/network"
JSONL = os.path.join(RES, "m3_net.jsonl")
OUT = os.path.join(RES, "M3-net-RTT-iops.svg")

rows = [json.loads(l) for l in open(JSONL)]
probe = {}
for r in rows:
    probe.setdefault(r["proto"], {}).setdefault(r["rtt_us"], []).append(r)

# x values
rtts = [0, 50, 100, 150]
colors = {"nvmeof": "#1f77b4", "iscsi": "#d62728"}
labels = {"nvmeof": "NVMe-oF", "iscsi": "iSCSI"}

W, H, ML, MR, MT, MB = 800, 480, 90, 30, 40, 60
plot_w, plot_h = W - ML - MR, H - MT - MB
xmax, ymax = 160, 0
for p, d in probe.items():
    for r in rtts:
        ymax = max(ymax, max(x["read_iops"] for x in d.get(r, [])))
        ymax = max(ymax, max(x["write_iops"] for x in d.get(r, [])))
ymax = int(ymax * 1.1 / 1000) * 1000

def X(r): return ML + plot_w * r / xmax
def Y(v): return MT + plot_h * (1 - v / ymax)

lines = []
lines.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}">')
lines.append('<rect width="100%" height="100%" fill="white"/>')
# grid + y labels
for gv in range(0, ymax + 1, max(ymax // 5, 1)):
    y = Y(gv)
    lines.append(f'<line x1="{ML}" y1="{y:.1f}" x2="{W - MR}" y2="{y:.1f}" stroke="#e0e0e0"/>')
    lines.append(f'<text x="{ML - 8}" y="{y + 4:.1f}" font-size="12" text-anchor="end">{gv//1000}K</text>')
# x labels
for r in rtts:
    x = X(r)
    lines.append(f'<text x="{x:.1f}" y="{H - MB + 20}" font-size="12" text-anchor="middle">{r}</text>')
lines.append(f'<text x="{ML + plot_w/2}" y="{H - 8}" font-size="14" text-anchor="middle" font-weight="bold">RTT (us)</text>')
lines.append(f'<text x="18" y="{MT + plot_h/2}" font-size="14" text-anchor="middle" font-weight="bold" transform="rotate(-90 18 {MT + plot_h/2})">IOPS</text>')
lines.append(f'<text x="{ML + plot_w/2}" y="22" font-size="16" text-anchor="middle" font-weight="bold">M3 (fio randrw 70/30, bs=4k, 1job, iodepth=32) - RTT-IOPS</text>')

# legend
lx = W - MR - 200
for i, (p, c) in enumerate(colors.items()):
    ly = 22 + i * 20
    lines.append(f'<line x1="{lx}" y1="{ly}" x2="{lx + 30}" y2="{ly}" stroke="{c}" stroke-width="3"/>')
    lines.append(f'<text x="{lx + 36}" y="{ly + 4}" font-size="13">{labels[p]}</text>')

# lines for read (solid) and write (dashed)
for p, c in colors.items():
    d = probe[p]
    def pts(key):
        return " ".join(f"{X(r):.1f},{Y(statistics.mean(x[key] for x in d[r])):.1f}" for r in rtts if r in d)
    rd_pts = pts("read_iops")
    wr_pts = pts("write_iops")
    lines.append(f'<polyline points="{rd_pts}" fill="none" stroke="{c}" stroke-width="2.5"/>')
    lines.append(f'<polyline points="{wr_pts}" fill="none" stroke="{c}" stroke-width="2.5" stroke-dasharray="6,4"/>')
    # error bars (min/max over runs)
    for r in rtts:
        if r not in d: continue
        vals = [x["read_iops"] for x in d[r]]
        lo, hi = min(vals), max(vals)
        x = X(r)
        lines.append(f'<line x1="{x:.1f}" y1="{Y(hi):.1f}" x2="{x:.1f}" y2="{Y(lo):.1f}" stroke="{c}" stroke-width="1.5"/>')

lines.append('</svg>')
open(OUT, "w").write("\n".join(lines))
print("wrote", OUT)
