#!/usr/bin/env python3
"""parse_m2g.py - summarize M2G (60s back-to-back) SVM results + host anchors + KVM.
Prints a table: rows=workload, cols=t4/t5/t0 (SVM avg) with anchor columns."""
import json, glob, os, re, sys

BASE = "/home/waiai/svm/exp/results/m2g"
LOADS = [("randrd", "read"), ("randwt", "write"), ("seqrd", "read"), ("seqwt", "write")]
TIERS = {"t4": 4, "t5": 5, "t0": 0}
W = {"t4": 1, "t5": 2, "t0": 4}

def iops_of(rwlabel, d):
    key = "read" if rwlabel.endswith("rd") else "write"
    j = (d.get("jobs") or [{}])[0]
    return j.get(key, {}).get("iops")

def parse_jsonl(path):
    rows = []
    if not os.path.exists(path):
        return rows
    for line in open(path):
        try:
            d = json.loads(line)
        except Exception:
            continue
        res = d.get("result", "")
        if isinstance(res, str):
            try:
                res = json.loads(res)
            except Exception:
                continue
        rows.append(d)
    return rows

def svm_matrix():
    mat = {}
    for label, _ in LOADS:
        mat[label] = {}
    for tier in TIERS:
        rows = parse_jsonl(os.path.join(BASE, "m2g_t%s.jsonl" % tier))
        if not rows:
            rows = parse_jsonl(os.path.join(BASE, "m2g_svm.jsonl"))
        for d in rows:
            rw = d.get("rw")
            if rw not in mat:
                continue
            res = d.get("result")
            if isinstance(res, str):
                try:
                    res = json.loads(res)
                except Exception:
                    continue
            v = iops_of(rw, res)
            if v:
                mat[rw].setdefault(tier, []).append(round(v, 1))
    return mat

def anchor_map():
    anc = {}
    for f in sorted(glob.glob(os.path.join(BASE, "anchors", "host_*_randrd.json"))):
        tag = re.search(r"host_(.*)_randrd", os.path.basename(f)).group(1)
        try:
            d = json.load(open(f))
            anc[tag] = {"randrd": round(iops_of("randrd", d), 1)}
        except Exception:
            pass
    for f in sorted(glob.glob(os.path.join(BASE, "anchors", "host_*_randwt.json"))):
        tag = re.search(r"host_(.*)_randwt", os.path.basename(f)).group(1)
        try:
            d = json.load(open(f))
            anc.setdefault(tag, {})["randwt"] = round(iops_of("randwt", d), 1)
        except Exception:
            pass
    return anc

def kvm_matrix():
    mat = {}
    for label, _ in LOADS:
        mat[label] = []
    for f in sorted(glob.glob(os.path.join(BASE, "kvm", "kvm_*.json"))):
        m = re.search(r"kvm_(\w+)_r(\d)", os.path.basename(f))
        try:
            d = json.load(open(f))
        except Exception:
            continue
        v = iops_of(m.group(1), d)
        if v:
            mat[m.group(1)].append(round(v, 1))
    return mat

def show(title, header, rows):
    print("\n%s" % title)
    print("%-8s %s" % ("load", "  ".join("%14s" % h for h in header)))
    for r in rows:
        print("%-8s %s" % (r[0], "  ".join("%14s" % c for c in r[1:])))

def avg(v):
    return round(sum(v) / len(v), 1) if v else None

mat = svm_matrix()
anc = anchor_map()
kvm = kvm_matrix()

# SVM table with W labels + anchor columns
cols = []
for tier in ("t4", "t5", "t0"):
    cols.append((tier + "(W=%d)" % W[tier], tier))
rows = []
for label, _ in LOADS:
    cells = []
    for title, tier in cols:
        v = mat[label].get(tier)
        cells.append(avg(v) if v else "-")
    rows.append((label, cells))
header = ["%s" % c[0] for c in cols]
show("M2G SVM (60s, numjobs=4, iodepth=16)", header, rows)

if anc:
    rows = []
    tags = sorted(anc)
    for label in ("randrd", "randwt"):
        cells = []
        for tag in tags:
            v = anc[tag].get(label)
            cells.append(avg([v]) if v else "-")
        rows.append((label, cells))
    show("host anchors (%s)" % ", ".join(tags), tags, rows)

rows = []
for label, _ in LOADS:
    v = kvm[label]
    rows.append((label, [avg(v) if v else "-"]))
show("KVM (60s, cache=none, same fio.raw)", ["avg iops"], rows)

# sample detail for t0
for d in parse_jsonl(os.path.join(BASE, "m2g_t0.jsonl"))[:2]:
    print("sample:", d.get("exp"), d.get("env"), d.get("rw"))
