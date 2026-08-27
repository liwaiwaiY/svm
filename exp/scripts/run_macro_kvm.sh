#!/bin/bash
# run_macro_kvm.sh - Macro (sysbench TPCC) on KVM, data on /dev/vdb (data disk).
#   MySQL datadir migrated to /mnt/data (vdb). cache=none + drop_caches per run.
#   threads {1,2,4} x 3 runs, 60s each. Output -> exp/results/macrokvm/
set -u
H=/home/waiai/svm
OUT=$H/exp/results/macrokvm
mkdir -p $OUT/logs
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
TS=(1 2 4)

pkill -f "qemu-system-x86_64" 2>/dev/null; pkill -f "remote-stub" 2>/dev/null; sleep 2
setsid $H/local_qemu/build/qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 4096 \
  -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw,cache=none \
  -device virtio-blk-pci,drive=drive0,bootindex=1 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -drive file=$H/exp/remote/fio.raw,if=none,id=drive1,format=raw,cache=none \
  -device virtio-blk-pci,drive=drive1,serial=data-disk \
  -serial file:/tmp/kvm-guest.log -monitor none -display none \
  -qmp unix:/tmp/qmp.sock,server,nowait \
  > /tmp/kvm-local-out.log 2>&1 < /dev/null &
for i in $(seq 1 60); do
  if env $SSH "true" 2>/dev/null; then echo "SSH up after $i"; break; fi
  sleep 3
done
env $SSH "echo wai | sudo -S bash -c 'grep -q /mnt/data /proc/mounts && systemctl is-active mysql'" 2>/dev/null

echo "=== [$(date +%T)] KVM Macro TPCC (data on vdb): start ==="
for t in "${TS[@]}"; do
  for r in 1 2 3; do
    echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
    env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=60 --threads=$t ./tpcc.lua run" 2>/dev/null \
      > $OUT/logs/kvm-t$t-r$r.log
    python3 -c "
import re
s=open('$OUT/logs/kvm-t$t-r$r.log').read()
txn=re.search(r'transactions:\s+(\d+)\s+\(([\d.]+) per sec', s)
lat=re.search(r'avg:\s+([\d.]+)', s)
p95=re.search(r'95th percentile:\s+([\d.]+)', s)
mx=re.search(r'max:\s+([\d.]+)', s)
print(f'  KVM t$t r$r: txn={txn.group(1)} ({txn.group(2)}/s) avg={lat.group(1)}ms p95={p95.group(1)}ms max={mx.group(1)}ms')
" 2>/dev/null || echo "  KVM t$t r$r: PARSE FAIL"
  done
done
env $SSH "echo wai | sudo -S poweroff" 2>/dev/null
sleep 8
pkill -f "qemu-system-x86_64" 2>/dev/null
echo "KVM_MACRO_OK [$(date +%T)]"
