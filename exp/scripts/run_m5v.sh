#!/bin/bash
# run_m5v.sh - M5 verification: SVM Q=64 I=128 iodepth=64
#   Window slack test: I=128 (>= 2Q) should recover the d=64 drop seen at I=64.
#   fio: randread 4K, 1 job, iodepth=64, 60s direct=1, x2 runs.
set -u
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
DATA=$H/exp/remote/fio.raw
OUT=$H/exp/results/m5/q64i128
mkdir -p $OUT
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=10 -p 2222 wai@127.0.0.1"

wait_ssh() {
  for i in $(seq 1 60); do
    if env $SSH "true" 2>/dev/null; then echo "  SSH up after $i"; return 0; fi
    sleep 3
  done
  echo "  SSH FAILED"; exit 1
}

host_anchor() {
  fio --name=anchor --filename=$DATA --rw=randread --bs=4k --iodepth=32 \
      --ioengine=libaio --direct=1 --runtime=20 --time_based \
      --output-format=json 2>/dev/null > $OUT/anchor_$1.json
  python3 -c "
import json
d=json.load(open('$OUT/anchor_$1.json'))
print(f'  HOST anchor $1: iops={d[\"jobs\"][0][\"read\"][\"iops\"]:.0f}')
"
}

echo "=== [$(date +%T)] M5V Q=64 I=128 iodepth=64: start ==="
host_anchor before
cat > $CONF <<EOF
# SVM M5V: W=4 I=128 L=S=32 M=512K Q=64 (run_m5v.sh generated)
buf_pool=0
inflight_size=128
local_batch_n=32
stub_batch_n=32
local_batch_m=524288
stub_batch_m=524288
stub_queue_max=64
zc_send_min=1048576
workers=4
EOF
echo "vr.conf -> M5V Q=64 I=128 L=S=32 M=512K"
pkill -f "remote-stub" 2>/dev/null; pkill -f "qemu-system-x86_64" 2>/dev/null; sleep 3
setsid remote-stub \
  -smp 4 \
  -drive file=$DATA,if=none,id=drive1,format=raw,cache=none \
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
for r in 1 2; do
  echo "### [$(date +%T)] SVM-Q64/I128 iodepth=64 run$r"
  env $SSH "echo wai | sudo -S fio --name=M5V --filename=/dev/vdb --rw=randread --bs=4k --numjobs=1 --iodepth=64 --ioengine=libaio --direct=1 --runtime=60 --time_based --group_reporting --output-format=json" 2>/dev/null \
    > $OUT/d64_r$r.json
  python3 -c "
import json
raw=open('$OUT/d64_r$r.json').read()
d,_=json.JSONDecoder().raw_decode(raw)
rd=d['jobs'][0]['read']
print(f'  run$r: iops={rd[\"iops\"]:.0f} clat={rd[\"clat_ns\"][\"mean\"]/1000:.0f}us')
"
done
cat > $CONF <<EOF
# SVM 参数档 0: Z=1048576 W=4 I=32 B=16 P=off (restored by run_m5v.sh)
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
host_anchor after
echo "M5V_OK [$(date +%T)]"
