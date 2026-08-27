#!/bin/bash
# run_m2fi.sh <I>   I in {64,128,256}
# Model check: W=4 fixed (tier 0), guest load fixed (numjobs=4 iodepth=16),
# vary only the SVM inflight window I. If per-vq ceiling ~35K is window-bound
# (I/latency), aggregate should scale up with I.
set -u
I=$1
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
RES=$H/exp/results/m2fi
G=/home/wai/svm_exp_out
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
SCP="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh scp -o StrictHostKeyChecking=no -P 2222"
mkdir -p $RES/logs

echo "### [$(date +%T)] M2FI I=$I: host_switch tier0"
bash $H/exp/host_switch.sh a 0 || { echo SWITCH_FAIL; exit 1; }

echo "### [$(date +%T)] M2FI I=$I: override inflight_size + restart"
sed -i "s/^inflight_size=.*/inflight_size=$I/" $CONF
sed -i "s|^# SVM 参数档 0:.*|# SVM 参数档 0 I=$I: W=4 (run_m2fi.sh)|" $CONF
pkill -f "remote-stub" 2>/dev/null
pkill -f "qemu-system-x86_64" 2>/dev/null
sleep 2
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
echo "restarted with inflight_size=$I"
for i in $(seq 1 60); do
  if env $SSH "true" 2>/dev/null; then echo "SSH up after ${i}"; break; fi
  sleep 3
done

env $SCP $H/exp/scripts/m2fi_runner.py wai@127.0.0.1:/home/wai/m2fi_runner.py 2>&1
env $SSH "echo wai | sudo -S pkill -f m2fi_runner.py 2>/dev/null; pkill -f '^fio ' 2>/dev/null; sleep 1; true"
env $SSH "echo wai | sudo -S bash -c 'nohup python3 /home/wai/m2fi_runner.py $I > $G/run-fi$I.log 2>&1 < /dev/null &' && echo LAUNCHED"

T=0
while [ $T -lt 60 ]; do
  if env $SSH "grep -q 'ALL DONE' $G/run-fi$I.log 2>/dev/null" 2>/dev/null; then
    break
  fi
  if ! pgrep -f "qemu-system-x86_64" >/dev/null || ! pgrep -f "remote-stub" >/dev/null; then
    echo PROC_DOWN
    exit 2
  fi
  sleep 20
  T=$((T+1))
done
if [ $T -ge 60 ]; then echo TIMEOUT; exit 3; fi

env $SSH "cat $G/m2fi_svm.jsonl 2>/dev/null" > $RES/m2fi_svm.jsonl
env $SSH "tar -C $G -cf - logs 2>/dev/null" | tar -C $RES -xf - 2>/dev/null
echo "jsonl lines: $(wc -l < $RES/m2fi_svm.jsonl)"
echo "TIER_OK"
