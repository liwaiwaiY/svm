#!/bin/bash
# run_m4null.sh <l1s1|l16s16|l64s64|l1s16|l16s1|kvm>
# M4 batch decoupling on null_blk (zero-latency backend).
#   local_batch_n (L) vs stub_batch_n (S) decoupled, inflight=512 (never caps).
#   fio: randread 4K, jobs=1, iodepth {32,64,128,256}, 60s direct=1.
#   kvm: local virtio-blk on /dev/nullb0 (RTT/batch/inflight independent ref).
set -u
MODE=$1
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
BASE=$H/exp/results/m4null
OUT=$BASE/$MODE
mkdir -p $OUT
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=10 -p 2222 wai@127.0.0.1"
IODEPTHS=(32 64 128 256)
echo dxeqqghk | sudo -S chmod 666 /dev/nullb0 2>/dev/null

FIO() {
  env $SSH "echo wai | sudo -S fio --name=M4N --filename=/dev/vdb --rw=randread --bs=4k --numjobs=1 --iodepth=$1 --ioengine=libaio --direct=1 --runtime=60 --time_based --group_reporting --output-format=json" 2>/dev/null
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
j=d['jobs'][0]; rd=j['read']
print(f'  $tag: r={rd[\"iops\"]:.0f} clat={rd[\"clat_ns\"][\"mean\"]/1000:.0f}us')
"
}

echo "=== [$(date +%T)] M4NULL mode=$MODE: start ==="
if [ "$MODE" = "kvm" ]; then
  pkill -f "remote-stub" 2>/dev/null; pkill -f "qemu-system-x86_64" 2>/dev/null; sleep 2
  setsid $H/local_qemu/build/qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 4 -m 4096 \
    -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive0,bootindex=1 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -drive file=/dev/nullb0,if=none,id=drive1,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive1,serial=data-disk \
    -serial file:/tmp/kvm-guest.log -monitor none -display none \
    -qmp unix:/tmp/qmp.sock,server,nowait \
    > /tmp/kvm-local-out.log 2>&1 < /dev/null &
  wait_ssh
  for d in "${IODEPTHS[@]}"; do
    echo "### [$(date +%T)] KVM/nullb iodepth=$d"
    FIO $d > $OUT/d${d}.json
    parse $OUT/d${d}.json "KVM/nullb d=$d"
  done
else
  L=${MODE#l}; L=${L%s*}
  S=${MODE#*s}
  cat > $CONF <<EOF
# SVM M4NULL: W=4 I=512 L=$L S=$S M=512K (run_m4null.sh generated)
buf_pool=0
inflight_size=512
local_batch_n=$L
stub_batch_n=$S
local_batch_m=524288
stub_batch_m=524288
zc_send_min=1048576
workers=4
EOF
  echo "vr.conf -> M4NULL L=$L S=$S I=512 M=512K"
  pkill -f "remote-stub" 2>/dev/null; pkill -f "qemu-system-x86_64" 2>/dev/null; sleep 3
  setsid remote-stub \
    -smp 4 \
    -drive file=/dev/nullb0,if=none,id=drive1,format=raw,cache=none \
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
    echo "### [$(date +%T)] SVM-L${L}S${S}/nullb iodepth=$d"
    FIO $d > $OUT/d${d}.json
    parse $OUT/d${d}.json "L${L}S${S}/nullb d=$d"
  done
  cat > $CONF <<EOF
# SVM 参数档 0: Z=1048576 W=4 I=32 B=16 P=off (restored by run_m4null.sh)
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
echo "M4NULL_${MODE}_OK [$(date +%T)]"
