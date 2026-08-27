#!/bin/bash
# run_m5.sh - M5 stub in_sg merge sweep on blk/null_blk.
#   Goal: does merging one request's (elem's) in_sg 4K pages into a single
#   contiguous buffer help bandwidth / per-request latency at large bs?
#   Cells: bs{16K,64K,256K,1M,2M} x {SVM P=off, SVM P=on, KVM}
#   SVM: I=256, Z=0 (all-zc resp), stub_merge_m=4M, W=4, B=16.
#   fio randrw 7:3 direct=1 iodepth=1 runtime=30 (sync, no queueing).
#   Results -> exp/results/m5/<tag>.bs.json ; summary at end.
#   FORCE=1 re-runs cells that already have bs.json.
set -u
H=/home/waiai/svm
OUT=$H/exp/results/m5
RUNTIME=30
mkdir -p $OUT
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
SCP="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh scp -P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

run_cell() { # $1=tag $2=mode(svm|kvm) $3=P $4=bs
  local tag=$1 mode=$2 P=$3 bs=$4
  echo "### [$(date +%T)] $tag: mode=$mode P=$P bs=$bs"
  if [ -z "${FORCE:-}" ] && [ -s $OUT/$tag.bs.json ]; then
    echo "  SKIP $tag (bs.json present)"; return
  fi
  if [ "$mode" = "kvm" ]; then
    bash $H/exp/host_switch_kvm_null.sh >/dev/null 2>&1
  else
    bash $H/exp/host_switch_blk_null.sh 0 256 $P 4194304 >/dev/null 2>&1
  fi
  if ! env $SSH "true" 2>/dev/null; then echo "  SWITCH/SSH FAIL $tag"; exit 1; fi
  env $SCP $H/exp/guest_m1mem.sh wai@127.0.0.1:/home/wai/m1mem.sh 2>/dev/null
  if ! env $SSH "grep -q 'mkdir -p /tmp/m15' /home/wai/m1mem.sh" 2>/dev/null; then
    echo "  DEPLOY FAIL $tag"; exit 1
  fi
  env $SSH "echo wai | sudo -S sh -c 'rm -f /tmp/m15/bs.json /tmp/m15/progress.log; nohup bash /home/wai/m1mem.sh $bs $RUNTIME 1 >/dev/null 2>&1 & echo OK'" 2>/dev/null | grep -q OK || { echo "  LAUNCH FAIL $tag"; exit 1; }
  local waited=0 done=0
  while [ $waited -lt 120 ]; do
    if env $SSH "grep -q ALL_DONE /tmp/m15/progress.log" 2>/dev/null; then done=1; break; fi
    sleep 2; waited=$((waited+2))
  done
  [ $done -eq 1 ] || { echo "  WARN: ALL_DONE not seen for $tag"; }
  env $SCP wai@127.0.0.1:/tmp/m15/bs.json $OUT/$tag.bs.json 2>/dev/null
  if [ ! -s $OUT/$tag.bs.json ]; then echo "  WARN: bs.json empty/missing for $tag"; fi
}

# SVM P=off (merge=4M, zc=0 all, I=256, iodepth=1)
run_cell svm_poff_16k  svm 0 16k
run_cell svm_poff_64k  svm 0 64k
run_cell svm_poff_256k svm 0 256k
run_cell svm_poff_1m   svm 0 1m
run_cell svm_poff_2m   svm 0 2m
# SVM P=on
run_cell svm_pon_16k   svm 1 16k
run_cell svm_pon_64k   svm 1 64k
run_cell svm_pon_256k  svm 1 256k
run_cell svm_pon_1m    svm 1 1m
run_cell svm_pon_2m    svm 1 2m
# KVM baseline
run_cell kvm_16k       kvm 0 16k
run_cell kvm_64k       kvm 0 64k
run_cell kvm_256k      kvm 0 256k
run_cell kvm_1m        kvm 0 1m
run_cell kvm_2m        kvm 0 2m

echo ""
echo "=== M5 SUMMARY (bw in KiB/s, 1/iops in us, from bs.json) ==="
python3 - "$OUT" <<'PY'
import json, glob, os, sys
out = sys.argv[1]
rows = []
order = ["svm_poff_16k","svm_poff_64k","svm_poff_256k","svm_poff_1m","svm_poff_2m",
         "svm_pon_16k","svm_pon_64k","svm_pon_256k","svm_pon_1m","svm_pon_2m",
         "kvm_16k","kvm_64k","kvm_256k","kvm_1m","kvm_2m"]
for tag in order:
    f = os.path.join(out, tag + ".bs.json")
    if not os.path.exists(f):
        rows.append((tag, "MISSING", "", "", "", "")); continue
    d = json.load(open(f))
    j = d["jobs"][0]
    r, w = j.get("read", {}), j.get("write", {})
    ri = r.get("iops", 0); wi = w.get("iops", 0)
    rb = r.get("bw_bytes", 0); wb = w.get("bw_bytes", 0)
    rl = r.get("clat_ns", {}).get("mean", 0) / 1e3
    wl = w.get("clat_ns", {}).get("mean", 0) / 1e3
    tot_iops = ri + wi; tot_bw = (rb + wb) / 1024.0
    per_us = (1e6 / tot_iops) if tot_iops else 0
    rows.append((tag, "ok", "%.0f" % ri, "%.0f" % tot_iops, "%.0f" % tot_bw,
                 "%.1f" % per_us))
print("%-14s %-7s %8s %8s %9s %8s" % ("cell", "st", "rd_iops", "tot_iops", "bw_KiB/s", "1/iops_us"))
for r in rows:
    print("%-14s %-7s %8s %8s %9s %8s" % r)
PY
echo "M5_OK [$(date +%T)]"
