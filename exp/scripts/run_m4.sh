#!/bin/bash
# run_m4.sh <b1|b16|b32|kvm> - M4 batch size sweep (2026-08-24 redo).
#   fio: randrw(70/30) bs=4k numjobs=1 iodepth in {1,16,32,64,128} 60s direct=1, x2
#   RTT: native loopback (no netem), ~0.1ms
#   SVM modes: batch_n=1/16/32 (local+stub), inflight_size=256 (window never caps
#              iodepth<=128), batch_m=512K (so batch_n is the binding flush
#              condition - default 64K would split a 32x4K batch at ~15 reqs)
#   kvm: local virtio-blk baseline (no TCP/batch/inflight)
set -u
MODE=$1
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
BASE=$H/exp/results/m4new
OUT=$BASE/$MODE
mkdir -p $OUT
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=10 -p 2222 wai@127.0.0.1"
IODEPTHS=(1 16 32 64 128)

FIO() {
  env $SSH "echo wai | sudo -S fio --name=M4 --filename=/dev/vdb --rw=randrw --rwmixread=70 --bs=4k --numjobs=1 --iodepth=$1 --ioengine=libaio --direct=1 --runtime=60 --time_based --group_reporting --output-format=json" 2>/dev/null
}

wait_ssh() {
  for i in $(seq 1 60); do
    if env $SSH "true" 2>/dev/null; then echo "  SSH up after $i"; return 0; fi
    sleep 3
  done
  echo "  SSH FAILED"; exit 1
}

parse() {
  local f=$1 tag=$2
  python3 -c "
import json
raw=open('$f').read()
d,_=json.JSONDecoder().raw_decode(raw)
j=d['jobs'][0]; rd=j['read']; wr=j['write']
print(f'  $tag: r={rd[\"iops\"]:.0f} w={wr[\"iops\"]:.0f} tot={rd[\"iops\"]+wr[\"iops\"]:.0f} clat={rd[\"clat_ns\"][\"mean\"]/1000:.0f}us')
"
}

echo "=== [$(date +%T)] M4 mode=$MODE: start ==="
if [ "$MODE" = "kvm" ]; then
  pkill -f "remote-stub" 2>/dev/null; pkill -f "qemu-system-x86_64" 2>/dev/null; sleep 2
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
  wait_ssh
  for d in "${IODEPTHS[@]}"; do
    echo "### [$(date +%T)] KVM iodepth=$d"
    for r in 1 2; do
      FIO $d > $OUT/d${d}_r$r.json
      parse $OUT/d${d}_r$r.json "KVM d=$d r$r"
    done
  done
else
  B=${MODE#b}
  cat > $CONF <<EOF
# SVM M4: W=4 I=256 B=$B M=512K (run_m4.sh generated)
buf_pool=0
inflight_size=256
local_batch_n=$B
stub_batch_n=$B
local_batch_m=524288
stub_batch_m=524288
zc_send_min=1048576
workers=4
EOF
  echo "vr.conf -> M4 B=$B I=256 M=512K"
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
  for d in "${IODEPTHS[@]}"; do
    echo "### [$(date +%T)] SVM-B$B iodepth=$d"
    for r in 1 2; do
      FIO $d > $OUT/d${d}_r$r.json
      parse $OUT/d${d}_r$r.json "B$B d=$d r$r"
    done
  done
  # restore tier-0 default config so subsequent experiments start clean
  cat > $CONF <<EOF
# SVM 参数档 0: Z=1048576 W=4 I=32 B=16 P=off (restored by run_m4.sh)
buf_pool=0
inflight_size=32
local_batch_n=16
stub_batch_n=16
zc_send_min=1048576
workers=4
EOF
  echo "vr.conf restored to tier-0"
fi

env $SSH "echo wai | sudo -S poweroff" 2>/dev/null
sleep 6
pkill -f "qemu-system-x86_64" 2>/dev/null; pkill -f "remote-stub" 2>/dev/null
echo "M4_${MODE}_OK [$(date +%T)]"
