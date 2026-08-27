#!/usr/bin/env python3
"""SVM virtio-blk experiment runner (guest side).

Usage: sudo python3 svm_runner.py <phase> <tier>
  phase: a  -> fio experiments (M2/M3/M4)   [/dev/vdb = scratch fio.raw]
         b  -> Macro + App (sysbench/pyspark) [/dev/vdb = data.raw]
  tier:  0/2/4/5/6/7/8  (SVM parameter tier)

JSONL records (experiment-matrix schema) appended to:
  ~/svm_exp_out/{m2,m3,m4,macro,app}_svm.jsonl   (accumulate across tiers)
Raw logs in ~/svm_exp_out/logs/.
"""
import json
import os
import subprocess
import sys
import time

OUT = "/home/wai/svm_exp_out"
LOG = os.path.join(OUT, "logs")
os.makedirs(LOG, exist_ok=True)

# runs per fio config. Re-run phases use 1 (SVM_RUNS=1): single-shot data,
# verified against the old records / kvm baseline before accepting.
RUNS = int(os.environ.get("SVM_RUNS", "3"))

TIERS = {
    # zc_send_min pinned at 1MB for every tier: MSG_ZEROCOPY on 4KB sends defers
    # the used-ring push until the peer ACKs (delayed ACK = 40ms tail, tier6
    # crash); copy-send 4KB and keep zc for >=1MB.  Values here MUST match
    # host_switch.sh (the env dict is recorded verbatim in the JSONL).
    0: {"zc_send_min": 1048576, "workers": 4, "inflight_size": 32, "batch_n": 16},
    2: {"zc_send_min": 1048576, "workers": 4, "inflight_size": 32, "batch_n": 16},
    4: {"zc_send_min": 1048576, "workers": 1, "inflight_size": 32, "batch_n": 16},
    5: {"zc_send_min": 1048576, "workers": 2, "inflight_size": 32, "batch_n": 16},
    6: {"zc_send_min": 1048576, "workers": 4, "inflight_size": 32, "batch_n": 16},
    7: {"zc_send_min": 1048576, "workers": 4, "inflight_size": 32, "batch_n": 1},
    8: {"zc_send_min": 1048576, "workers": 4, "inflight_size": 32, "batch_n": 4},
}
VARIANT = {0: "SVM-档0-基准", 2: "SVM-档2", 4: "SVM-档4", 5: "SVM-档5",
           6: "SVM-档6", 7: "SVM-档7", 8: "SVM-档8"}


def run(cmd, timeout=7200):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr


def append(fname, d):
    with open(os.path.join(OUT, fname), "a") as f:
        f.write(json.dumps(d, ensure_ascii=False) + "\n")


def record(exp, tier, env, command, result, run=0, extra=None):
    t = TIERS[tier]
    d = {"dev": "blk", "exp": exp, "variant": VARIANT[tier],
         "env": {"zc_send_min": t["zc_send_min"], "workers": t["workers"],
                 "inflight_size": t["inflight_size"],
                 "local_batch_n": t["batch_n"], "stub_batch_n": t["batch_n"],
                 "buf_pool": 0},
         "command": command, "result": result}
    if run:
        d["run"] = run
    if extra:
        d.update(extra)
    return d


def save_raw(name, out):
    with open(os.path.join(LOG, name), "w") as f:
        f.write(out)


def fio_job(exp, tier, rw, numjobs, iodepth, rwlabel, runno, rwmix=None):
    base = ("fio --name=%s --filename=/dev/vdb --rw=%s --bs=4k "
            "--numjobs=%d --iodepth=%d --ioengine=libaio --direct=1 "
            "--runtime=60 --time_based --group_reporting --output-format=json"
            % (exp, rw, numjobs, iodepth))
    if rwmix:
        base += " --rwmixread=%d" % rwmix
    out = run(base)
    save_raw("%s-t%d-%s-r%d.json" % (exp, tier, rwlabel, runno), out)
    append("%s_svm.jsonl" % exp.lower(),
           record(exp, tier, None, base, out, run=runno, extra={"rw": rwlabel}))
    print("[%s] t%d %s run%d done" % (exp, tier, rwlabel, runno), flush=True)


def run_m2(tier, workers):
    for label, rw in [("randrd", "randread"), ("randwt", "randwrite"),
                      ("seqrd", "read"), ("seqwt", "write")]:
        for runno in range(1, RUNS + 1):
            fio_job("M2", tier, rw, workers, 16, label, runno)


def run_m2f(tier):
    """M2 re-run with FIXED numjobs=4 (total inflight 64), only W varies.
    Matches the matrix's original M2 spec and the KVM baseline (m2_kvm,
    numjobs=4). Records to m2f_svm.jsonl / logs M2F-* so the old numjobs=W
    records in m2_svm.jsonl stay untouched for diffing."""
    for label, rw in [("randrd", "randread"), ("randwt", "randwrite"),
                      ("seqrd", "read"), ("seqwt", "write")]:
        for runno in range(1, RUNS + 1):
            fio_job("M2F", tier, rw, 4, 16, label, runno)


def run_m2g(tier):
    """M2 60s back-to-back re-run (2026-08-24): numjobs=4, iodepth=16, 60s,
    with host-anchor methodology. Records to m2g_svm.jsonl / logs M2G-*.
    Same shape as M2F but a fresh dataset tied to host/KVM anchors."""
    for label, rw in [("randrd", "randread"), ("randwt", "randwrite"),
                      ("seqrd", "read"), ("seqwt", "write")]:
        for runno in range(1, RUNS + 1):
            fio_job("M2G", tier, rw, 4, 16, label, runno)


def run_m3(tier, inflight):
    for runno in range(1, RUNS + 1):
        fio_job("M3", tier, "randrw", 1, inflight, "i%d" % inflight, runno, rwmix=70)


def run_m4(tier, batch):
    for iodepth in (16, 32, 64, 128, 256):
        for runno in range(1, RUNS + 1):
            fio_job("M4", tier, "randrw", 1, iodepth,
                    "b%d-d%d" % (batch, iodepth), runno, rwmix=70)


def run_macro(tier, workers):
    cmd = ("cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql "
           "--mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest "
           "--mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 "
           "--time=60 --threads=%d ./tpcc.lua run" % workers)
    for runno in range(1, RUNS + 1):
        out = run(cmd)
        save_raw("macro-t%d-w%d-r%d.log" % (tier, workers, runno), out)
        append("macro_svm.jsonl", record("Macro", tier,
               {"threads": workers, "tables": 1, "scale": 1, "time": 60},
               cmd, out, run=runno))
        print("[Macro] t%d w%d run%d done" % (tier, workers, runno), flush=True)


def run_app(tier, cores):
    cmd = ("export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64; "
           "export SPARK_HOME=/mnt/data/spark; "
           "export SPARK_LOCAL_DIRS=/mnt/data/spark-tmp; "
           "cd /home/wai && timeout 3600 $SPARK_HOME/bin/spark-submit "
           "--master local[%d] /home/wai/pagerank.py "
           "/mnt/data/wiki/wiki-Talk-1m.txt 10 %d" % (cores, cores))
    for runno in range(1, RUNS + 1):
        out = run(cmd)
        save_raw("app-t%d-c%d-r%d.log" % (tier, cores, runno), out)
        append("app_svm.jsonl", record("App", tier,
               {"cores": cores, "iterations": 10, "data": "wiki-Talk-1m"},
               cmd, out, run=runno))
        print("[App] t%d c%d run%d done" % (tier, cores, runno), flush=True)


def setup_phase_b():
    """mount data disk and wait for MySQL before Macro/App."""
    if not os.path.ismount("/mnt/data"):
        os.makedirs("/mnt/data", exist_ok=True)
        run("mount /dev/vdb /mnt/data")
    for _ in range(60):
        if "active" in run("systemctl is-active mysql"):
            return
        time.sleep(2)
    raise RuntimeError("mysql did not become active")


def main():
    phase, tier = sys.argv[1], int(sys.argv[2])
    if phase == "g":
        # M2 60s back-to-back re-run with host anchors (2026-08-24): tiers 4/5/0
        if tier in (4, 5, 0):
            run_m2g(tier)
        else:
            raise ValueError("phase g supports tiers 4/5/0 only")
    elif phase == "f":
        # M2 fixed-jobs (numjobs=4) re-run for worker-pool study: tiers 4/5/0
        if tier in (4, 5, 0):
            run_m2f(tier)
        else:
            raise ValueError("phase f supports tiers 4/5/0 only")
    elif phase == "a":
        if tier == 0:
            run_m2(0, 4)
            run_m3(0, 32)
        elif tier == 2:
            run_m4(2, 16)
        elif tier == 4:
            run_m2(4, 1)
        elif tier == 5:
            run_m2(5, 2)
        elif tier == 6:
            # I 16->32 re-run: full Phase A (M2/M3/M4) with the fixed binary.
            # Old t6 records (inflight_size=16) stay in the jsonl for diffing.
            run_m2(6, 4)
            run_m3(6, 16)
            run_m4(6, 16)
        elif tier == 7:
            run_m4(7, 1)
        elif tier == 8:
            run_m4(8, 4)
    elif phase == "b":
        setup_phase_b()
        if tier == 0:
            run_macro(0, 4)
            run_app(0, 4)
        elif tier == 4:
            run_macro(4, 1)
            run_app(4, 1)
        elif tier == 5:
            run_macro(5, 2)
            run_app(5, 2)
    print("ALL DONE", flush=True)


if __name__ == "__main__":
    main()
