#!/bin/bash
# host_switch_kvm_null.sh - KVM baseline on null_blk backend (/dev/nullb0).
#   Single qemu (no remote stub); data disk = nullb0, cache=none (O_DIRECT).
#   Waits for guest SSH (2222). Used by run_m1mem.sh.
set -u
H=/home/waiai/svm

echo dxeqqghk | sudo -S chmod 666 /dev/nullb0 2>/dev/null

pkill -f "remote-stub" 2>/dev/null
pkill -f "qemu-system-x86_64" 2>/dev/null
sleep 3

setsid $H/local_qemu/build/qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 4096 \
  -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw \
  -device virtio-blk-pci,drive=drive0 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -drive file=/dev/nullb0,if=none,id=drive1,format=raw,cache=none \
  -device virtio-blk-pci,drive=drive1,serial=data-disk \
  -serial file:guest.log -monitor none -display none \
  > /tmp/kvm-out.log 2>&1 < /dev/null &
echo "kvm qemu started (nullb0)"

for i in $(seq 1 60); do
  if DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh \
     ssh -o StrictHostKeyChecking=no -o ConnectTimeout=3 -o BatchMode=no \
     -p 2222 wai@127.0.0.1 "true" 2>/dev/null; then
    echo "guest SSH up after ${i} tries"
    exit 0
  fi
  sleep 3
done
echo "ERROR: guest SSH did not come up"
exit 1
