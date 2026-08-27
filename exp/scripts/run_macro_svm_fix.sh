#!/bin/bash
# run_macro_svm_fix.sh - re-run SVM Macro tiers t0 (W=4) and t5 (W=2) after
#   tpcc_run.lua delivery nil-fix. t4 (W=1) data already valid (macrosvm/svm-t4-r*.log).
#   Graceful shutdown between tiers. Output -> exp/results/macrosvm/
set -u
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
OUT=$H/exp/results/macrosvm
mkdir -p $OUT/logs
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"

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
shutdown_guest() {
  if env $SSH "true" 2>/dev/null; then
    env $SSH "echo wai | sudo -S bash -c 'systemctl stop mysql; sleep 2; poweroff'" 2>/dev/null
    sleep 12
  fi
  pkill -f "remote-stub" 2>/dev/null; pkill -f "qemu-system-x86_64" 2>/dev/null; sleep 3
}
boot_svm() {
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
  # 20s warmup (not counted)
  env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=20 --threads=$2 ./tpcc.lua run" > /dev/null 2>&1
}

run3() {  # $1=tier $2=threads
  for r in 1 2 3; do
    echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
    env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=60 --threads=$2 ./tpcc.lua run" 2>/dev/null \
      > $OUT/logs/svm-$1-r$r.log
    python3 -c "
import re
s=open('$OUT/logs/svm-$1-r$r.log').read()
txn=re.search(r'transactions:\s+(\d+)\s+\(([\d.]+) per sec', s)
lat=re.search(r'avg:\s+([\d.]+)', s)
p95=re.search(r'95th percentile:\s+([\d.]+)', s)
mx=re.search(r'max:\s+([\d.]+)', s)
print(f'  SVM $1 r$r: txn={txn.group(1)} ({txn.group(2)}/s) avg={lat.group(1)}ms p95={p95.group(1)}ms max={mx.group(1)}ms')
" 2>/dev/null || echo "  SVM $1 r$r: PARSE FAIL"
  done
}

echo "=== [$(date +%T)] SVM Macro fix re-run (t0 W=4, t5 W=2): start ==="

# t0: current guest already W=4 (tier-0). Reuse it.
cat > $CONF <<EOF
# SVM Macro t0: W=4 I=32 B=16 (run_macro_svm_fix.sh)
buf_pool=0
inflight_size=32
local_batch_n=16
stub_batch_n=16
zc_send_min=1048576
workers=4
EOF
echo "vr.conf -> t0 W=4"
if ! env $SSH "true" 2>/dev/null; then boot_svm t0 4; fi
wait_mysql
echo "### t0 (W=4, threads=4)"
run3 t0 4

# t5: graceful shutdown, boot W=2
shutdown_guest
cat > $CONF <<EOF
# SVM Macro t5: W=2 I=32 B=16 (run_macro_svm_fix.sh)
buf_pool=0
inflight_size=32
local_batch_n=16
stub_batch_n=16
zc_send_min=1048576
workers=2
EOF
echo "vr.conf -> t5 W=2"
boot_svm t5 2
echo "### t5 (W=2, threads=2)"
run3 t5 2

shutdown_guest
cat > $CONF <<EOF
# SVM 参数档 0: Z=1048576 W=4 I=32 B=16 P=off (restored)
buf_pool=0
inflight_size=32
local_batch_n=16
stub_batch_n=16
zc_send_min=1048576
workers=4
EOF
echo "SVM_MACRO_FIX_OK [$(date +%T)]"
