#!/bin/bash
# run_kvm_m2g.sh   KVM 60s back-to-back reference: 4 workloads x 3 runs x 60s
# on fio.raw with cache=none (O_DIRECT), matching M2G methodology.
set -u
H=/home/waiai/svm
OUT=$H/exp/results/m2g/kvm
mkdir -p $OUT
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"

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

for rw in randread randwrite read write; do
  case $rw in
    randread) lbl=randrd;; randwrite) lbl=randwt;; read) lbl=seqrd;; write) lbl=seqwt;;
  esac
  for r in 1 2 3; do
    env $SSH "echo wai | sudo -S fio --name=KVM --filename=/dev/vdb --rw=$rw --bs=4k --numjobs=4 --iodepth=16 --ioengine=libaio --direct=1 --runtime=60 --time_based --group_reporting --output-format=json" 2>/dev/null \
      > $OUT/kvm_${lbl}_r$r.json
    python3 -c "
import json
d=json.load(open('$OUT/kvm_${lbl}_r$r.json'))
k='read' if 'read' in '$rw' else 'write'
j=d['jobs'][0][k]
print(f'  KVM $lbl r$r: {j[\"iops\"]:.0f} iops clat={j[\"clat_ns\"][\"mean\"]/1000:.0f}us')
"
  done
done

env $SSH "echo wai | sudo -S poweroff" 2>/dev/null
sleep 8
pkill -f "qemu-system-x86_64" 2>/dev/null
echo "KVM_M2G_OK"
