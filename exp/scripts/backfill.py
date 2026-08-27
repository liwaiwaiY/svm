#!/usr/bin/env python3
"""Backfill missing SVM blk experiment runs (current tier config Z=1MB).

Usage: python3 backfill.py <phase> <tier>
  phase a: fio M2/M3/M4 -- only the (exp, rw, run) combos missing from
           /home/wai/svm_exp_out/*_svm.jsonl are re-run
  phase b: macro + app (all 3 runs, they were never run)
"""
import os
import re
import sys

sys.path.insert(0, "/home/wai")
from svm_runner import (fio_job, run_macro, run_app, OUT, VARIANT)

ZC = 1048576

def rec_exists(exp, variant, runno, rw):
    fn = os.path.join(OUT, "%s_svm.jsonl" % exp.lower())
    if not os.path.exists(fn):
        return False
    pat = (r'"exp": "%s".*?"variant": "%s".*?"zc_send_min": %d.*?'
           r'"run": %d.*?"rw": "%s"' % (exp, variant, ZC, runno, rw))
    rx = re.compile(pat, re.S)
    return any(rx.search(ln) for ln in open(fn))

def missing_runs(exp, variant, rwlabels):
    miss = {}
    for label in rwlabels:
        miss[label] = [r for r in (1, 2, 3)
                       if not rec_exists(exp, variant, r, label)]
    return miss

def backfill_m2(tier, workers):
    labels = [("randrd", "randread"), ("randwt", "randwrite"),
              ("seqrd", "read"), ("seqwt", "write")]
    for label, rw in labels:
        for r in (1, 2, 3):
            if not rec_exists("M2", VARIANT[tier], r, label):
                print("M2 t%d %s run%d MISSING -> run" % (tier, label, r), flush=True)
                fio_job("M2", tier, rw, workers, 16, label, r)
            else:
                print("M2 t%d %s run%d ok" % (tier, label, r), flush=True)

def backfill_m3(tier, inflight):
    label = "i%d" % inflight
    for r in (1, 2, 3):
        if not rec_exists("M3", VARIANT[tier], r, label):
            print("M3 t%d %s run%d MISSING -> run" % (tier, label, r), flush=True)
            fio_job("M3", tier, "randrw", 1, inflight, label, r, rwmix=70)
        else:
            print("M3 t%d %s run%d ok" % (tier, label, r), flush=True)

def backfill_m4(tier, batch):
    for iodepth in (16, 32, 64, 128, 256):
        label = "b%d-d%d" % (batch, iodepth)
        for r in (1, 2, 3):
            if not rec_exists("M4", VARIANT[tier], r, label):
                print("M4 t%d %s run%d MISSING -> run" % (tier, label, r), flush=True)
                fio_job("M4", tier, "randrw", 1, iodepth, label, r, rwmix=70)
            else:
                print("M4 t%d %s run%d ok" % (tier, label, r), flush=True)

def main():
    phase, tier = sys.argv[1], int(sys.argv[2])
    if phase == "a":
        if tier == 0:
            backfill_m2(0, 4)
            backfill_m3(0, 32)
        elif tier == 4:
            backfill_m2(4, 1)
        elif tier == 5:
            backfill_m2(5, 2)
        elif tier == 7:
            backfill_m4(7, 1)
        elif tier == 8:
            backfill_m4(8, 4)
        else:
            print("phase a: no backfill for tier %d" % tier)
    elif phase == "b":
        if tier in (0, 4, 5):
            run_macro(tier, {0: 4, 4: 1, 5: 2}[tier])
            run_app(tier, {0: 4, 4: 1, 5: 2}[tier])
        else:
            print("phase b: no backfill for tier %d" % tier)
    print("BACKFILL DONE", flush=True)

if __name__ == "__main__":
    main()
