#!/bin/bash
# host_switch.sh <phase> <tier>
#   phase: a -> stub serves scratch fio.raw (fio experiments)
#          b -> stub serves data.raw (Macro/App, needs spark/wiki data)
# tier:  0/2/4/5/6/7/8
# Writes vr.conf for the tier, restarts stub + local qemu, waits for SSH.
#
# zc_send_min is pinned at 1MB for every tier. The local side sends the
# guest's out buffers with MSG_ZEROCOPY; the kernel keeps referencing those
# pages until the peer ACKs (TCP delayed ACK on loopback = up to 40ms), so
# the used-ring push must be deferred until the zc completion. That deferral
# showed up as a ~40ms latency plateau on ~1% of 4KB requests (tier6 crash,
# 41ms @ p99 with Z=4096). Copy-sending 4K requests costs a negligible
# memcpy and pushes immediately; the per-request TCP latency fix (NODELAY)
# handles the rest. >=1MB sends still get MSG_ZEROCOPY (none in these tiers).

set -u
PHASE=$1
TIER=$2
CONF=/home/waiai/svm/local_qemu/hw/virtio-remote/vr.conf
# keep core dumps for crash forensics (systemd-coredump pattern)
ulimit -c unlimited

case $TIER in
  0) Z=1048576; W=4; I=32; B=16 ;;
  2) Z=1048576; W=4; I=32; B=16 ;;
  4) Z=1048576; W=1; I=32; B=16 ;;
  5) Z=1048576; W=2; I=32; B=16 ;;
  6) Z=1048576; W=4; I=32; B=16 ;;  # I 16->32: window==iodepth(16) caused full-pipeline stall on disk hiccup (63ms p99.9); I=32 restores disk-native tail
  7) Z=1048576; W=4; I=32; B=1  ;;
  8) Z=1048576; W=4; I=32; B=4  ;;
  *) echo "bad tier $TIER"; exit 1 ;;
esac

cat > $CONF <<EOF
# SVM 参数档 $TIER: Z=${Z} W=${W} I=${I} B=${B} P=off (host_switch.sh generated)
buf_pool=0
inflight_size=$I
local_batch_n=$B
stub_batch_n=$B
zc_send_min=$Z
workers=$W
EOF
echo "vr.conf -> tier $TIER: Z=$Z W=$W I=$I B=$B"

# stop current stub + local qemu
pkill -f "remote-stub" 2>/dev/null
pkill -f "qemu-system-x86_64" 2>/dev/null
sleep 2

if [ "$PHASE" = "a" ]; then
    DISK=/home/waiai/svm/exp/remote/fio.raw
else
    DISK=/home/waiai/svm/exp/local/kvm/data.raw
fi

# start stub
setsid remote-stub \
  -smp 4 \
  -drive file=$DISK,if=none,id=drive1,format=raw,cache=none \
  -device virtio-blk-pci,drive=drive1,serial=data-disk,remote-stub=127.0.0.1@5552 \
  -display none -monitor none -serial file:/tmp/stub-blk.log \
  > /tmp/stub-out.log 2>&1 < /dev/null &
echo "stub started (disk=$DISK)"

# start local qemu
setsid /home/waiai/svm/local_qemu/build/qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 4096 \
  -drive file=/home/waiai/svm/exp/local/system.raw,if=none,id=drive0,format=raw \
  -device virtio-blk-pci,drive=drive0 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -drive file=/home/waiai/svm/exp/local/svm/dummy.raw,if=none,id=drive1,format=raw \
  -device virtio-blk-pci,drive=drive1,serial=data-disk,remote-machine=127.0.0.1@5552 \
  -serial file:guest.log -monitor none -display none \
  -qmp unix:/tmp/qmp.sock,server,nowait \
  > /tmp/local-out.log 2>&1 < /dev/null &
echo "local qemu started"

# wait for ssh
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
