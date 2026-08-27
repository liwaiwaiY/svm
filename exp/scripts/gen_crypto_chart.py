#!/usr/bin/env python3
"""gen_crypto_chart.py - render M1/M5 blocksize-throughput SVG charts from jsonl."""
import json
import math

BS = [16, 64, 256, 1024, 8192, 16384]
W, H, ML, MT, MR, MB = 820, 500, 70, 30, 20, 55
PW, PH = W - ML - MR, H - MT - MB
COLORS = {"KVM": "#d62728", "SVM-档0": "#1f77b4", "SVM-档1": "#ff7f0e",
          "SVM-档2": "#2ca02c", "SVM-档3": "#9467bd", "SVM-档9": "#8c564b"}
XLABELS = {16: "16B", 64: "64B", 256: "256B", 1024: "1K", 8192: "8K", 16384: "16K"}


def x_of(bs):
    return ML + (math.log2(bs) - 4) / (14 - 4) * PW


def load(path, exp):
    data = {}
    for line in open(path):
        r = json.loads(line)
        if r["exp"] != exp:
            continue
        data.setdefault(r["variant"], []).append(r["result"])
    return data


def svg_header(title):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
            f'viewBox="0 0 {W} {H}"><rect width="{W}" height="{H}" fill="white"/>'
            f'<text x="{W/2}" y="20" font-size="16" text-anchor="middle" '
            f'font-family="sans-serif">{title}</text>')


def axes(ymax):
    out = []
    # y gridlines
    for v in range(0, ymax + 1, max(1, ymax // 5)):
        y = H - MB - v / ymax * PH
        out.append(f'<line x1="{ML}" y1="{y:.1f}" x2="{W-MR}" y2="{y:.1f}" '
                   f'stroke="#ddd" stroke-width="1"/>')
        out.append(f'<text x="{ML-6}" y="{y+4:.1f}" font-size="10" text-anchor="end" '
                   f'font-family="sans-serif">{v}</text>')
    for bs in BS:
        x = x_of(bs)
        out.append(f'<line x1="{x:.1f}" y1="{MT}" x2="{x:.1f}" y2="{H-MB}" stroke="#eee"/>')
        out.append(f'<text x="{x:.1f}" y="{H-MB+16}" font-size="10" text-anchor="middle" '
                   f'font-family="sans-serif">{XLABELS[bs]}</text>')
    out.append(f'<text x="{ML}" y="{H-MB+34}" font-size="11" font-family="sans-serif">blocksize</text>')
    out.append(f'<text x="16" y="{MT+PH/2}" font-size="11" font-family="sans-serif" '
               f'transform="rotate(-90 16 {MT+PH/2})">MB/s</text>')
    return "".join(out)


def series(variant, results, ymax, y_of):
    pts = []
    for bs in BS:
        mean = sum(r[str(bs)] for r in results) / len(results)
        pts.append((x_of(bs), y_of(mean)))
    path = "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in pts)
    color = COLORS.get(variant, "#000")
    s = f'<path d="{path}" stroke="{color}" stroke-width="2" fill="none"/>'
    for (x, y) in pts:
        s += f'<circle cx="{x:.1f}" cy="{y:.1f}" r="3.5" fill="{color}"/>'
    return s


def legend(variants):
    items = []
    for i, v in enumerate(variants):
        x = ML + i * 130
        items.append(f'<rect x="{x}" y="34" width="14" height="4" fill="{COLORS.get(v,"#000")}"/>')
        items.append(f'<text x="{x+18}" y="39" font-size="11" font-family="sans-serif">{v}</text>')
    return "".join(items)


def render(path, exp, title, variants, ymax):
    data = load(path, exp)
    y_of = lambda mb: H - MB - mb / ymax * PH
    svg = svg_header(title) + axes(ymax) + legend(variants)
    for v in variants:
        if v in data:
            svg += series(v, data[v], ymax, y_of)
    svg += "</svg>"
    out = path.replace(".jsonl", "-%s.svg" % exp)
    open(out, "w").write(svg)
    print("wrote", out)


render("/home/waiai/svm/exp/results/crypto/M1.jsonl", "M1",
       "M1 crypto: blocksize-throughput (aes-128-cbc, SVM tiers)",
       ["KVM", "SVM-档0", "SVM-档1", "SVM-档2", "SVM-档3"], 450)
render("/home/waiai/svm/exp/results/crypto/M5.jsonl", "M5",
       "M5 crypto: buf pool off/on (aes-128-cbc, SVM)",
       ["SVM-档0", "SVM-档9"], 200)
