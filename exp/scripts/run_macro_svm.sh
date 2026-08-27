#!/bin/bash
# run_macro_svm.sh - Macro (sysbench TPCC) on SVM, data on remote disk fio.raw (vdb).
#   MySQL datadir /mnt/data on guest vdb = stub fio.raw. cache=none + drop_caches per run.
#   t0 (W=4,threads=4) / t4 (W=1,threads=1) / t5 (W=2,threads=2) x 3 runs.
#   Output -> exp/results/macrosvm/
set -u
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
OUT=$H/exp/results/macrosvm
mkdir -p $OUT/logs
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"

# tier -> workers threads
declare -A W=( [t0]=4 [t4]=1 [t5]=2 )
declare -A T=( [t0]=4 [t4]=1 [t5]=2 )

wait_ssh() {
  for i in $(seq 1 60); do
    if env $SSH "true" 2>/dev/null; then echo "  SSH up after $i"; return 0; fi
    sleep 3
  done
  echo "  SSH FAILED"; exit 1
}

wait_mysql() {
  for i in $(seq 1 120); do
    if env $SSH "mysqladmin -u sbtest -psbtest ping 2>/dev/null | grep -q alive" 2>/dev/null; then
      echo "  MySQL up after $i"; return 0
    fi
    sleep 3
  done
  echo "  MySQL FAILED"; exit 1
}

echo "=== [$(date +%T)] SVM Macro TPCC (data on remote fio.raw): start ==="
for tier in t0 t4 t5; do
  cat > $CONF <<EOF
# SVM Macro $tier: W=${W[$tier]} I=32 B=16 (run_macro_svm.sh generated)
buf_pool=0
inflight_size=32
local_batch_n=16
stub_batch_n=16
local_batch_m=65536
stub_batch_m=65536
zc_send_min=1048576
workers=${W[$tier]}
EOF
  echo "vr.conf -> $tier W=${W[$tier]}"
  # graceful shutdown of previous guest (if any): stop mysql first, then poweroff.
  # pkill-ing qemu = power cut -> InnoDB crash recovery on next boot (very slow on W=1).
  if env $SSH "true" 2>/dev/null; then
    env $SSH "echo wai | sudo -S bash -c 'systemctl stop mysql; sleep 2; poweroff'" 2>/dev/null
    sleep 12
  fi
  pkill -f "remote-stub" 2>/dev/null; pkill -f "qemu-system-x86_64" 2>/dev/null; sleep 3
  setsid remote-stub \
    -smp 4 \
    -drive file=$H/exp/remote/fio.raw,if=none,id=drive1,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive1,serial=data-disk,remote-stub=127.0.0.1@5552 \
    -display none -monitor none -serial file:/tmp/stub-blk.log \
    > /tmp/stub-out.log 2>&1 < /dev/null &
  setsid $H/local_qemu/build/qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 4 -m 4096 \
    -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw \
    -device virtio-blk-pci,drive=drive0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -drive file=$H/exp/local/svm/dummy.raw,if=none,id=drive1,format=raw \
    -device virtio-blk-pci,drive=drive1,serial=data-disk,remote-machine=127.0.0.1@5552 \
    -serial file:guest.log -monitor none -display none \
    -qmp unix:/tmp/qmp.sock,server,nowait \
    > /tmp/local-out.log 2>&1 < /dev/null &
  wait_ssh
  wait_mysql
  # 20s warmup per tier (cold buffer pool / InnoDB recovery settle), not counted
  env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=20 --threads=${T[$tier]} ./tpcc.lua run" > /dev/null 2>&1
  for r in 1 2 3; do
    echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
    env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=60 --threads=${T[$tier]} ./tpcc.lua run" 2>/dev/null \
      > $OUT/logs/svm-$tier-r$r.log
    python3 -c "
import re
s=open('$OUT/logs/svm-$tier-r$r.log').read()
txn=re.search(r'transactions:\s+(\d+)\s+\(([\d.]+) per sec', s)
lat=re.search(r'avg:\s+([\d.]+)', s)
p95=re.search(r'95th percentile:\s+([\d.]+)', s)
mx=re.search(r'max:\s+([\d.]+)', s)
print(f'  SVM $tier r$r: txn={txn.group(1)} ({txn.group(2)}/s) avg={lat.group(1)}ms p95={p95.group(1)}ms max={mx.group(1)}ms')
" 2>/dev/null || echo "  SVM $tier r$r: PARSE FAIL"
  done
done
env $SSH "echo wai | sudo -S poweroff" 2>/dev/null
sleep 8
pkill -f "qemu-system-x86_64" 2>/dev/null; pkill -f "remote-stub" 2>/dev/null
cat > $CONF <<EOF
# SVM 参数档 0: Z=1048576 W=4 I=32 B=16 P=off (restored by run_macro_svm.sh)
buf_pool=0
inflight_size=32
local_batch_n=16
stub_batch_n=16
zc_send_min=1048576
workers=4
EOF
echo "SVM_MACRO_OK [$(date +%T)]"
