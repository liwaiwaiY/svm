#!/usr/bin/env python3
"""Parse M2F (fixed numjobs=4) SVM results; compare with KVM baseline and old M2 (numjobs=W)."""
import json, glob, os, statistics, csv

BASE = "/home/waiai/svm/exp/results"
M2F_DIR = os.path.join(BASE, "m2f")
KVM = "/home/waiai/svm/exp_results/m2_kvm.jsonl"
OLD = os.path.join(BASE, "m2_svm.jsonl")

WR = ["randrd", "randwt", "seqrd", "seqwt"]
TIER_W = {0: 4, 4: 1, 5: 2}


def load_jsonl(path):
    rows = []
    if not os.path.exists(path):
        return rows
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        rows.append(d)
    return rows


def iops(d, rw):
    j = json.loads(d["result"])["jobs"][0]
    # fio result key by label: randrd/seqrd -> read, randwt/seqwt -> write
    key = "read" if rw.endswith("rd") else "write"
    return j[key]["iops"]


def tier_of(d):
    v = d.get("variant", "")
    return int("".join(ch for ch in v if ch.isdigit()) or "0")


# --- M2F (fixed numjobs=4) ---
m2f = load_jsonl(os.path.join(M2F_DIR, "m2f_svm.jsonl"))
m2f_by = {}
for d in m2f:
    t = tier_of(d)
    m2f_by.setdefault(t, {}).setdefault(d.get("rw"), []).append(iops(d, d.get("rw")))

# --- KVM baseline (numjobs=4) ---
kvm = {}
for d in load_jsonl(KVM):
    if d.get("rw") in WR:
        kvm.setdefault(d.get("rw"), []).append(iops(d, d.get("rw")))

# --- old M2 (numjobs=W) ---
old = {}
for d in load_jsonl(OLD):
    if d.get("rw") in WR:
        t = tier_of(d)
        old.setdefault(t, {}).setdefault(d.get("rw"), []).append(iops(d, d.get("rw")))


def mean(x):
    return statistics.mean(x) if x else float("nan")


def fmt(v):
    return "%.1fK" % (v / 1000) if v == v else "-"


print("=== M2F fixed numjobs=4 (total inflight 64), mean over runs ===")
print(f"{'tier':<5}{'W':<4}{'randrd':>9}{'randwt':>9}{'seqrd':>9}{'seqwt':>9}")
for t in (4, 5, 0):
    d = m2f_by.get(t, {})
    print(f"{t:<5}{TIER_W[t]:<4}" + "".join(f"{fmt(mean(d.get(w, []))):>9}" for w in WR))
print(f"{'KVM':<5}{'-':<4}" + "".join(f"{fmt(mean(kvm.get(w, []))):>9}" for w in WR))

print("\n=== old M2 numjobs=W (for diff) ===")
print(f"{'tier':<5}{'W':<4}{'randrd':>9}{'randwt':>9}{'seqrd':>9}{'seqwt':>9}")
for t in (4, 5, 0):
    d = old.get(t, {})
    print(f"{t:<5}{TIER_W[t]:<4}" + "".join(f"{fmt(mean(d.get(w, []))):>9}" for w in WR))

# raw values for M2F
print("\n=== M2F raw runs ===")
for t in (4, 5, 0):
    for w in WR:
        vals = m2f_by.get(t, {}).get(w, [])
        if vals:
            print(f"t{t} {w}: " + " / ".join("%.0f" % v for v in vals))

# CSV summary
out_csv = os.path.join(M2F_DIR, "m2f_summary.csv")
with open(out_csv, "w", newline="") as cf:
    wcsv = csv.writer(cf)
    wcsv.writerow(["tier", "workers", "load", "randrd", "randwt", "seqrd", "seqwt"])
    for t in (4, 5, 0):
        d = m2f_by.get(t, {})
        wcsv.writerow([t, TIER_W[t], "4job x iod16"] + [round(mean(d.get(w, [])), 1) for w in WR])
    wcsv.writerow(["kvm", "-", "4job x iod16"] + [round(mean(kvm.get(w, [])), 1) for w in WR])
print("\nwrote", out_csv)
