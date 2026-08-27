#!/bin/bash
# host_switch_blk_null.sh <Z> <I> <P> [<M>]
#   Switch to the blk SVM environment with null_blk backend (/dev/nullb0).
#   W=4 B=16 fixed. P in {0,1}. Z in bytes (0 = all-zc).
#   M = stub_merge_m in bytes (0 = per-sg in buffers, original). Default 0.
#   Stub (5552) + local qemu, waits for guest SSH (2222).
set -u
Z=$1
I=$2
P=$3
M=${4:-0}
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
ulimit -c unlimited

cat > $CONF <<EOF
# SVM blk/null_blk: Z=${Z} W=4 I=${I} B=16 P=${P} M=${M} (host_switch_blk_null.sh generated)
buf_pool=$P
inflight_size=$I
local_batch_n=16
stub_batch_n=16
stub_merge_m=$M
zc_send_min=$Z
workers=4
EOF
echo "vr.conf -> blk/null_blk: Z=$Z I=$I P=$P M=$M"

echo dxeqqghk | sudo -S chmod 666 /dev/nullb0 2>/dev/null

pkill -f "remote-stub" 2>/dev/null
pkill -f "qemu-system-x86_64" 2>/dev/null
sleep 3

setsid remote-stub \
  -smp 4 \
  -drive file=/dev/nullb0,if=none,id=drive1,format=raw,cache=none \
  -device virtio-blk-pci,drive=drive1,serial=data-disk,remote-stub=127.0.0.1@5552 \
  -display none -monitor none -serial file:/tmp/stub-blk.log \
  > /tmp/stub-out.log 2>&1 < /dev/null &
echo "stub started (nullb0)"

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
echo "local qemu started"

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
