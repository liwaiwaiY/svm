#!/bin/bash
# run_m1m5_blk.sh - M1(zc threshold) + M5(inflight x buf_pool) on blk/null_blk
#   Load: fio randrw 7:3, iodepth=64, runtime=30, bs {16K,64K,256K,1M,2M}
#   (4M hangs: 1026 descs > 1024 limit). null_blk backend (/dev/nullb0).
#   M1: zc {0,64K,4M} x bs, I=256, P=on, 1 rep each combo
#   M5: zc=1M x bs x I {64,32,16} x P {off,on}, 1 rep each
#   Results -> exp/results/m1m5/<tag>/rep<r>/bs<bs>.json
set -u
H=/home/waiai/svm
OUT=$H/exp/results/m1m5
RUNTIME=30
IODEPTH=64
mkdir -p $OUT
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
SCP="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh scp -P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

# deploy guest runner once (verify, retry once); must run AFTER first switch
deploy() {
  env $SCP $H/exp/guest_m15.sh wai@127.0.0.1:/home/wai/m15run.sh 2>/dev/null
  env $SSH "test -f /home/wai/m15run.sh" 2>/dev/null
}

run_config() { # $1=tag $2=Z $3=I $4=P $5=reps
  local tag=$1 Z=$2 I=$3 P=$4 reps=$5
  echo "### [$(date +%T)] $tag: Z=$Z I=$I P=$P reps=$reps"
  bash $H/exp/host_switch_blk_null.sh $Z $I $P >/dev/null 2>&1
  if ! env $SSH "true" 2>/dev/null; then echo "  SWITCH/SSH FAIL $tag"; return 1; fi
  if [ -z "${DEPLOYED:-}" ]; then
    deploy || deploy || { echo "  DEPLOY FAIL"; return 1; }
    DEPLOYED=1
    echo "  guest runner deployed"
  fi
  local r
  for r in $(seq 1 $reps); do
    env $SSH "echo wai | sudo -S sh -c 'rm -f /tmp/m15/progress.log; nohup bash /home/wai/m15run.sh $RUNTIME $IODEPTH >/dev/null 2>&1 & echo OK'" 2>/dev/null | grep -q OK || { echo "  LAUNCH FAIL rep$r"; return 1; }
    local waited=0
    while [ $waited -lt 720 ]; do
      if env $SSH "grep -q ALL_DONE /tmp/m15/progress.log" 2>/dev/null; then break; fi
      sleep 5; waited=$((waited+5))
    done
    env $SSH "cat /tmp/m15/progress.log" 2>/dev/null | sed 's/^/  /'
    local d=$OUT/$tag/rep$r
    mkdir -p $d
    for bs in 16k 64k 256k 1m 2m; do
      env $SCP wai@127.0.0.1:/tmp/m15/bs$bs.json $d/bs$bs.json 2>/dev/null
    done
  done
}

# M1: zc sweep, I=256, P=on, 1 rep
run_config m1_z0_i256_p1     0       256 1 1
run_config m1_z64k_i256_p1   65536   256 1 1
run_config m1_z4m_i256_p1    4194304 256 1 1

# M5: inflight sweep x buf_pool, zc=1M, 1 rep
run_config m5_z1m_i64_p0  1048576 64 0 1
run_config m5_z1m_i64_p1  1048576 64 1 1
run_config m5_z1m_i32_p0  1048576 32 0 1
run_config m5_z1m_i32_p1  1048576 32 1 1
run_config m5_z1m_i16_p0  1048576 16 0 1
run_config m5_z1m_i16_p1  1048576 16 1 1

# summary table
echo ""
echo "=== SUMMARY ==="
echo "tag              rep bs      read_iops  write_iops  r_bwMB/s  mean_us"
for tag in m1_z0_i256_p1 m1_z64k_i256_p1 m1_z4m_i256_p1 \
           m5_z1m_i64_p0 m5_z1m_i64_p1 m5_z1m_i32_p0 m5_z1m_i32_p1 m5_z1m_i16_p0 m5_z1m_i16_p1; do
  for rep in $(seq 1 $(ls $OUT/$tag 2>/dev/null | wc -l)); do
    for bs in 16k 64k 256k 1m 2m; do
      f=$OUT/$tag/rep$rep/bs$bs.json
      [ -f "$f" ] || continue
      python3 -c "
import json,sys
d=json.load(open('$f'))['jobs'][0]
r,w=d['read'],d['write']
print('%-15s %-3s %-5s r=%9.0f w=%9.0f %8.1f %8.0f'%('$tag','$rep','$bs',r['iops'],w['iops'],(r['bw_bytes']+w['bw_bytes'])/1048576,r['clat_ns']['mean']/1000))
"
    done
  done
done
echo "M1M5_OK [$(date +%T)]"
