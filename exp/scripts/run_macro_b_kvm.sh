#!/bin/bash
# run_macro_b_kvm.sh - KVM Macro anchor rerun, SAME conditions as nvmeof/iscsi macro-b:
#   single boot, threads {1,2,4} x 3 runs 60s, 20s warmup (threads=2),
#   cache=none + drop_caches per run, graceful shutdown (stop mysql -> poweroff).
#   Output -> exp/results/macro-b/kvm/
set -u
H=/home/waiai/svm
OUT=$H/exp/results/macro-b/kvm
mkdir -p $OUT/logs
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
TS=(1 2 4)

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
parse() {
  python3 -c "
import re
s=open('$1').read()
txn=re.search(r'transactions:\s+(\d+)\s+\(([\d.]+) per sec', s)
lat=re.search(r'avg:\s+([\d.]+)', s)
p95=re.search(r'95th percentile:\s+([\d.]+)', s)
mx=re.search(r'max:\s+([\d.]+)', s)
print(f'  $2: txn={txn.group(1)} ({txn.group(2)}/s) avg={lat.group(1)}ms p95={p95.group(1)}ms max={mx.group(1)}ms')
" 2>/dev/null || echo "  $2: PARSE FAIL"
}

echo "=== [$(date +%T)] KVM Macro-B (data on fio.raw): start ==="
pkill -f 'qemu-syste[m]' 2>/dev/null; pkill -f 'remote-[s]tub' 2>/dev/null; sleep 2
setsid $H/local_qemu/build/qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 4096 \
  -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw,cache=none \
  -device virtio-blk-pci,drive=drive0,bootindex=1 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -drive file=$H/exp/remote/fio.raw,if=none,id=drive1,format=raw,cache=none \
  -device virtio-blk-pci,drive=drive1,serial=data-disk \
  -serial file:/tmp/macro-b-kvm-guest.log -monitor none -display none \
  -qmp unix:/tmp/qmp.sock,server,nowait \
  > /tmp/macro-b-kvm-out.log 2>&1 < /dev/null &
wait_ssh
wait_mysql
echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=20 --threads=2 ./tpcc.lua run" > /dev/null 2>&1
echo "  warmup done, starting runs [$(date +%T)]"
for t in "${TS[@]}"; do
  for r in 1 2 3; do
    echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
    env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=60 --threads=$t ./tpcc.lua run" 2>/dev/null \
      > $OUT/logs/kvm-t$t-r$r.log
    parse $OUT/logs/kvm-t$t-r$r.log "KVM t$t r$r"
  done
done
env $SSH "echo wai | sudo -S bash -c 'systemctl stop mysql; sleep 2; poweroff'" 2>/dev/null
sleep 12
pkill -f 'qemu-syste[m]' 2>/dev/null
echo "MACRO_B_KVM_OK [$(date +%T)]"
