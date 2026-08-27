#!/bin/bash
# run_m6_kvm.sh - M6 KVM baseline (补跑, 2026-08-27)
#   Mirrors M6 SVM q-sweep (exp/results/m5/q*): fio randread 4K, 1 job,
#   iodepth {1,16,32,64}, 60s, direct=1, data disk fio.raw cache=none (O_DIRECT),
#   host anchors before/after + host drop_caches per run.
#   Results -> exp/results/m5/kvm/
set -u
H=/home/waiai/svm
DATA=$H/exp/remote/fio.raw
OUT=$H/exp/results/m5/kvm
mkdir -p $OUT
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=10 -p 2222 wai@127.0.0.1"

wait_ssh() {
  for i in $(seq 1 60); do
    if env $SSH "true" 2>/dev/null; then echo "  SSH up after $i"; return 0; fi
    sleep 3
  done
  echo "  SSH FAILED"; exit 1
}

drop_caches() {
  echo dxeqqghk | sudo -S sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
}

host_anchor() {
  drop_caches
  fio --name=anchor --filename=$DATA --rw=randread --bs=4k --iodepth=32 \
      --ioengine=libaio --direct=1 --runtime=20 --time_based \
      --output-format=json 2>/dev/null > $OUT/anchor_$1.json
  python3 -c "
import json
d=json.load(open('$OUT/anchor_$1.json'))
print(f'  HOST anchor $1: iops={d[\"jobs\"][0][\"read\"][\"iops\"]:.0f}')
"
}

echo "=== [$(date +%T)] M6 KVM: start ==="
host_anchor before

pkill -f "remote-stub" 2>/dev/null; pkill -f "qemu-system-x86_64" 2>/dev/null; sleep 3
echo dxeqqghk | sudo -S rm -f $H/guest.log 2>/dev/null
setsid $H/local_qemu/build/qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 4096 \
  -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw,cache=none \
  -device virtio-blk-pci,drive=drive0,bootindex=1 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -drive file=$DATA,if=none,id=drive1,format=raw,cache=none \
  -device virtio-blk-pci,drive=drive1,serial=data-disk \
  -serial file:$H/guest.log -monitor none -display none \
  -qmp unix:/tmp/qmp.sock,server,nowait \
  > /tmp/kvm-m6-out.log 2>&1 < /dev/null &
wait_ssh

for d in 1 16 32 64; do
  drop_caches
  echo "### [$(date +%T)] KVM iodepth=$d"
  env $SSH "echo wai | sudo -S fio --name=M6KVM --filename=/dev/vdb --rw=randread --bs=4k --numjobs=1 --iodepth=$d --ioengine=libaio --direct=1 --runtime=60 --time_based --group_reporting --output-format=json" 2>/dev/null \
    > $OUT/d$d.json
  python3 -c "
import json
raw=open('$OUT/d$d.json').read()
d,_=json.JSONDecoder().raw_decode(raw)
rd=d['jobs'][0]['read']
p=rd['clat_ns']['percentile']
print(f'  KVM d$d: iops={rd[\"iops\"]:.0f} clat_mean={rd[\"clat_ns\"][\"mean\"]/1000:.0f}us p50={p[\"50.000000\"]/1000:.0f}us p95={p[\"95.000000\"]/1000:.0f}us p99={p[\"99.000000\"]/1000:.0f}us')
"
done

env $SSH "echo wai | sudo -S poweroff" 2>/dev/null
sleep 8
pkill -f "qemu-system-x86_64" 2>/dev/null
host_anchor after
echo "M6_KVM_OK [$(date +%T)]"
