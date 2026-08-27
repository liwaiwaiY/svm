#!/bin/bash
# run_m3rtt3.sh <kvm|i16|i32> - M3 RTT sweep on inflight window.
#   fio: randrw(70/30) bs=4k numjobs=1 iodepth=32 60s direct=1 (same as M3)
#   RTT 0.1/0.2/0.5/1/2ms (netem delay 0/50/200/450/950us one-way; RTT=100us+2*delay)
#   i16/i32: SVM t0 (W=4,B=16) with inflight_size=16/32 -> in-flight = window
#   kvm: local virtio-blk (RTT-independent) baseline, 0.1ms + 2ms x1
set -u
MODE=$1
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
BASE=$H/exp/results/m3rtt3
OUT=$BASE/$MODE
mkdir -p $OUT
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=10 -p 2222 wai@127.0.0.1"
TC() { echo dxeqqghk | sudo -S tc "$@" >/dev/null 2>&1; }
LABELS=(0p1ms 0p2ms 0p5ms 1ms 2ms)
DELAYS=(0 50 200 450 950)
FIO="echo wai | sudo -S fio --name=M3I --filename=/dev/vdb --rw=randrw --rwmixread=70 --bs=4k --numjobs=1 --iodepth=32 --ioengine=libaio --direct=1 --runtime=60 --time_based --group_reporting --output-format=json"

wait_ssh() {
  for i in $(seq 1 60); do
    if env $SSH "true" 2>/dev/null; then echo "  SSH up after $i"; return 0; fi
    sleep 3
  done
  echo "  SSH FAILED"; exit 1
}

echo "=== [$(date +%T)] M3RTT3 mode=$MODE: start ==="
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
  for lbl in 0p1ms 2ms; do
    echo "### [$(date +%T)] KVM $lbl (RTT-independent)"
    env $SSH "$FIO" 2>/dev/null > $OUT/rtt_${lbl}_r1.json
    python3 -c "
import json
raw=open('$OUT/rtt_${lbl}_r1.json').read()
d,_=json.JSONDecoder().raw_decode(raw)
j=d['jobs'][0]; rd=j['read']; wr=j['write']
print(f'  KVM $lbl: r={rd[\"iops\"]:.0f} w={wr[\"iops\"]:.0f} tot={rd[\"iops\"]+wr[\"iops\"]:.0f}')
"
  done
else
  I=${MODE#i}  # 16 or 32
  bash $H/exp/host_switch.sh a 0 || exit 1
  if [ "$MODE" = "i16" ]; then
    echo "=== [$(date +%T)] rewrite inflight_size=16 + restart stub/qemu ==="
    sed -i 's/inflight_size=32/inflight_size=16/' $CONF
    grep inflight $CONF
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
  fi
  for i in ${!DELAYS[@]}; do
    d=${DELAYS[$i]}; lbl=${LABELS[$i]}
    TC qdisc del dev lo root 2>/dev/null
    if [ $d -gt 0 ]; then
      TC qdisc add dev lo root netem delay ${d}us || { echo "TC_FAIL $lbl"; exit 2; }
    fi
    echo "### [$(date +%T)] SVM-I$I RTT $lbl (delay ${d}us)"
    for r in 1 2; do
      env $SSH "$FIO" 2>/dev/null > $OUT/rtt_${lbl}_r$r.json
      python3 -c "
import json
raw=open('$OUT/rtt_${lbl}_r$r.json').read()
d,_=json.JSONDecoder().raw_decode(raw)
j=d['jobs'][0]; rd=j['read']; wr=j['write']
print(f'  $lbl r$r: r={rd[\"iops\"]:.0f} w={wr[\"iops\"]:.0f} tot={rd[\"iops\"]+wr[\"iops\"]:.0f} clat={rd[\"clat_ns\"][\"mean\"]/1000:.0f}us')
"
    done
  done
  TC qdisc del dev lo root 2>/dev/null
fi

env $SSH "echo wai | sudo -S poweroff" 2>/dev/null
sleep 6
pkill -f "qemu-system-x86_64" 2>/dev/null; pkill -f "remote-stub" 2>/dev/null
echo "M3RTT3_${MODE}_OK [$(date +%T)]"
