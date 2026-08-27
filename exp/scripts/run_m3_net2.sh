#!/bin/bash
# run_m3_net2.sh <nvmeof|iscsi>
# M3 RTT sweep for network storage protocols, SAME calibration as SVM M3RTT3:
#   RTT = 100us (loopback) + 2*delay; delay 0/50/200/450/950us -> RTT 0.1/0.2/0.5/1/2ms
#   fio: randrw(70/30) bs=4k numjobs=1 iodepth=32 60s direct=1 (M3 standard)
#   2 runs per point. Guest virtio-blk backend = host initiator device.
set -u
PROTO=$1
H=/home/waiai/svm
DEV=/dev/nvme1n1
[ "$PROTO" = "iscsi" ] && DEV=/dev/sda
RES=$H/exp/results/m3net2/$PROTO
mkdir -p $RES
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=10 -p 2222 wai@127.0.0.1"
TC() { echo dxeqqghk | sudo -S tc "$@" >/dev/null 2>&1; }
LABELS=(0p1ms 0p2ms 0p5ms 1ms 2ms)
DELAYS=(0 50 200 450 950)

echo "=== [$(date +%T)] M3NET2 $PROTO (dev=$DEV): start ==="
pkill -f "qemu-system-x86_64" 2>/dev/null; pkill -f "remote-stub" 2>/dev/null; sleep 2
echo dxeqqghk | sudo -S chmod 666 $DEV 2>/dev/null
setsid $H/local_qemu/build/qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 4096 \
  -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw,cache=none \
  -device virtio-blk-pci,drive=drive0,bootindex=1 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -drive file=$DEV,if=none,id=drive1,format=raw,cache=none \
  -device virtio-blk-pci,drive=drive1,serial=data-disk \
  -serial file:/tmp/net-guest.log -monitor none -display none \
  -qmp unix:/tmp/qmp.sock,server,nowait \
  > /tmp/net-local-out.log 2>&1 < /dev/null &
for i in $(seq 1 60); do
  if env $SSH "true" 2>/dev/null; then echo "  SSH up after $i"; break; fi
  sleep 3
  if [ $i -eq 60 ]; then echo "SSH FAILED"; exit 1; fi
done

# backend is now DIO (loop direct-io) reading the physical disk directly:
# no page cache involved, so no prep warm-up is needed.

for i in ${!DELAYS[@]}; do
  d=${DELAYS[$i]}; lbl=${LABELS[$i]}
  TC qdisc del dev lo root 2>/dev/null
  if [ $d -gt 0 ]; then
    TC qdisc add dev lo root netem delay ${d}us || { echo "TC_FAIL $lbl"; exit 2; }
  fi
  echo "### [$(date +%T)] $PROTO RTT $lbl (delay ${d}us)"
  for r in 1 2; do
    env $SSH "echo wai | sudo -S fio --name=M3NET --filename=/dev/vdb --rw=randrw --rwmixread=70 --bs=4k --numjobs=1 --iodepth=32 --ioengine=libaio --direct=1 --runtime=60 --time_based --group_reporting --output-format=json" 2>/dev/null \
      > $RES/rtt_${lbl}_r$r.json
    python3 -c "
import json
raw=open('$RES/rtt_${lbl}_r$r.json').read()
d,_=json.JSONDecoder().raw_decode(raw)
j=d['jobs'][0]; rd=j['read']; wr=j['write']
print(f'  $lbl r$r: r={rd[\"iops\"]:.0f} w={wr[\"iops\"]:.0f} tot={rd[\"iops\"]+wr[\"iops\"]:.0f} clat={rd[\"clat_ns\"][\"mean\"]/1000:.0f}us')
"
  done
done
TC qdisc del dev lo root 2>/dev/null
env $SSH "echo wai | sudo -S poweroff" 2>/dev/null
sleep 6
pkill -f "qemu-system-x86_64" 2>/dev/null
echo "M3NET2_${PROTO}_OK [$(date +%T)]"
