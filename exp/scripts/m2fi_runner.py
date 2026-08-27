#!/usr/bin/env python3
"""M2FI runner (guest): W=4 fixed, vary SVM inflight window I.
Workloads: randrd + seqrd, numjobs=4, iodepth=16 (same as M2F baseline).
Records to m2fi_svm.jsonl with variant 'SVM-档0-I<val>'.
Usage: sudo python3 m2fi_runner.py <I>
"""
import json
import os
import subprocess
import sys

I = int(sys.argv[1])
OUT = "/home/wai/svm_exp_out"
LOG = os.path.join(OUT, "logs")
os.makedirs(LOG, exist_ok=True)
RUNS = 3


def run(cmd, timeout=7200):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr


TIERS0 = {"zc_send_min": 1048576, "workers": 4, "inflight_size": I, "batch_n": 16}

for label, rw in [("randrd", "randread"), ("seqrd", "read")]:
    for runno in range(1, RUNS + 1):
        base = ("fio --name=M2FI --filename=/dev/vdb --rw=%s --bs=4k "
                "--numjobs=4 --iodepth=16 --ioengine=libaio --direct=1 "
                "--runtime=60 --time_based --group_reporting "
                "--output-format=json" % rw)
        out = run(base)
        with open(os.path.join(LOG, "M2FI-I%d-%s-r%d.json" % (I, label, runno)), "w") as f:
            f.write(out)
        d = {"dev": "blk", "exp": "M2FI", "variant": "SVM-档0-I%d" % I,
             "env": TIERS0, "command": base, "result": out,
             "run": runno, "rw": label}
        with open(os.path.join(OUT, "m2fi_svm.jsonl"), "a") as f:
            f.write(json.dumps(d, ensure_ascii=False) + "\n")
        print("[M2FI] I=%d %s run%d done" % (I, label, runno), flush=True)

print("ALL DONE I=%d" % I, flush=True)
