#!/bin/bash
# run_m4c.sh - M4C: highlight LOCAL send batch on the write path.
#   W=1 (2 vqs -> 1 send worker, sendmsg serialized on the critical path),
#   jobs=2, randwrite 4K, iodepth=32 (in-flight 64), I=128 (window slack).
#   L = local_batch_n in {1,4,16,32,64}; S=64 (stub send fully batched, no confound);
#   batch_m=512K; Q unlimited (stub recv not capped). One run per L.
set -u
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
OUT=$H/exp/results/m4c
mkdir -p $OUT
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=10 -p 2222 wai@127.0.0.1"
LS=(1 4 16 32 64)
echo dxeqqghk | sudo -S chmod 666 /dev/nullb0 2>/dev/null

wait_ssh() {
  for i in $(seq 1 60); do
    if env $SSH "true" 2>/dev/null; then echo "  SSH up after $i"; return 0; fi
    sleep 3
  done
  echo "  SSH FAILED"; exit 1
}

echo "=== [$(date +%T)] M4C W=1 jobs=2 randwrite4K d=32 I=128: start ==="
for L in "${LS[@]}"; do
  cat > $CONF <<EOF
# SVM M4C: W=1 I=128 L=$L S=64 M=512K (run_m4c.sh generated)
buf_pool=0
inflight_size=128
local_batch_n=$L
stub_batch_n=64
local_batch_m=524288
stub_batch_m=524288
zc_send_min=1048576
workers=1
EOF
  echo "vr.conf -> M4C L=$L W=1 I=128 S=64"
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
  echo "### [$(date +%T)] SVM-L$L/W1 randwrite4K jobs=2 d=32"
  env $SSH "echo wai | sudo -S fio --name=M4C --filename=/dev/vdb --rw=randwrite --bs=4k --numjobs=2 --iodepth=32 --ioengine=libaio --direct=1 --runtime=60 --time_based --group_reporting --output-format=json" 2>/dev/null \
    > $OUT/l$L.json
  python3 -c "
import json
raw=open('$OUT/l$L.json').read()
d,_=json.JSONDecoder().raw_decode(raw)
wr=d['jobs'][0]['write']
print(f'  L=$L: iops={wr[\"iops\"]:.0f} 1/iops={1000000/wr[\"iops\"]:.2f}us clat={wr[\"clat_ns\"][\"mean\"]/1000:.0f}us')
"
done
cat > $CONF <<EOF
# SVM 参数档 0: Z=1048576 W=4 I=32 B=16 P=off (restored by run_m4c.sh)
buf_pool=0
inflight_size=32
local_batch_n=16
stub_batch_n=16
zc_send_min=1048576
workers=4
EOF
echo "vr.conf restored to tier-0"
env $SSH "echo wai | sudo -S poweroff" 2>/dev/null
sleep 6
pkill -f "qemu-system-x86_64" 2>/dev/null; pkill -f "remote-stub" 2>/dev/null
echo "M4C_OK [$(date +%T)]"
