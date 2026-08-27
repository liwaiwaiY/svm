#!/bin/bash
# KVM baseline re-run on fio.raw with cache=none (fair comparison with SVM)
# boots local qemu (no remote stub), runs fio in guest on /dev/vdb
set -u
H=/home/waiai/svm

pkill -f "qemu-system-x86_64" 2>/dev/null
sleep 2

# start KVM guest with fio.raw as the data disk
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
echo "KVM guest starting..."

for i in $(seq 1 60); do
  if DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh \
     ssh -o StrictHostKeyChecking=no -o ConnectTimeout=3 -o BatchMode=no \
     -p 2222 wai@127.0.0.1 "true" 2>/dev/null; then
    echo "guest SSH up after ${i} tries"
    break
  fi
  sleep 3
done

for rw in randread read randwrite write; do
  label=${rw%read}; [ "$rw" = "read" ] && label=seqrd
  [ "$rw" = "write" ] && label=seqwt
  [ "$rw" = "randread" ] && label=randrd
  [ "$rw" = "randwrite" ] && label=randwt
  echo "=== KVM-ref $label ==="
  DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh \
    ssh -o StrictHostKeyChecking=no -p 2222 wai@127.0.0.1 \
    "echo wai | sudo -S fio --name=M2K --filename=/dev/vdb --rw=$rw --bs=4k --numjobs=4 --iodepth=16 --ioengine=libaio --direct=1 --runtime=60 --time_based --group_reporting --output-format=json" 2>/dev/null \
    > /tmp/kvm_ref_$label.json
  python3 -c "
import json,sys
d=json.load(open('/tmp/kvm_ref_$label.json'))
k='read' if 'read' in '$rw' else 'write'
j=d['jobs'][0][k]
p=j.get('clat_ns',{}).get('percentile',{})
p99=p.get('99.000000',0)/1000
print(f'  {j[\"iops\"]:.0f} iops clat={j[\"clat_ns\"][\"mean\"]/1000:.0f}us p99={p99:.0f}us')
"
done

echo "=== done, shutting down guest cleanly ==="
DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh \
  ssh -o StrictHostKeyChecking=no -p 2222 wai@127.0.0.1 "echo wai | sudo -S poweroff" 2>/dev/null
sleep 10
pkill -f "qemu-system-x86_64" 2>/dev/null
echo OK
