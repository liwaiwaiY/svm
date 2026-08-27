#!/bin/bash
# run_crypto_tier.sh <mode> <tier>
#   mode: kvm (local backend, no stub) | svm (tier 0/1/2/3/9)
#   Runs guest-side crypto_bench.sh (openssl speed afalg aes-128-cbc x3),
#   waits for ALL DONE, copies raw log, appends parsed result to jsonl.
set -u
MODE=$1
TIER=${2:-0}
H=/home/waiai/svm
RES=$H/exp/results/crypto
LOGSD=$RES/logs
mkdir -p $RES $LOGSD
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
SCP="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh scp -P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

echo "### [$(date +%T)] crypto $MODE tier $TIER: switch/start"
if [ "$MODE" = "kvm" ]; then
  pkill -f "remote-stu[b]" 2>/dev/null
  pkill -f "qemu-system-x86_[6]4" 2>/dev/null
  sleep 2
  setsid /home/waiai/svm/local_qemu/build/qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 4 -m 4096 \
    -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw \
    -device virtio-blk-pci,drive=drive0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -object cryptodev-backend-builtin,id=cryptodev0 \
    -device virtio-crypto-pci,cryptodev=cryptodev0 \
    -serial file:/tmp/local-kvm-out.log -monitor none -display none \
    -qmp unix:/tmp/qmp.sock,server,nowait \
    > /tmp/local-crypto-kvm.log 2>&1 < /dev/null &
  echo "KVM local qemu (crypto local backend) started"
else
  bash $H/exp/host_switch_crypto.sh $TIER || { echo SWITCH_FAIL; exit 1; }
fi

echo "### [$(date +%T)] wait SSH"
UP=0
for i in $(seq 1 60); do
  if env $SSH "true" 2>/dev/null; then UP=1; break; fi
  sleep 3
done
if [ "$UP" != "1" ]; then echo "SSH_TIMEOUT"; exit 2; fi
echo "### [$(date +%T)] SSH up, deploy bench + launch"
env $SCP $H/exp/scripts/crypto_bench.sh wai@127.0.0.1:/home/wai/ >/dev/null 2>&1
env $SSH "rm -f /home/wai/svm_exp_out/crypto-$TIER.log /home/wai/svm_exp_out/crypto-$TIER.run; nohup bash /home/wai/crypto_bench.sh $TIER > /home/wai/svm_exp_out/crypto-$TIER.run 2>&1 < /dev/null & echo LAUNCHED"

echo "### [$(date +%T)] wait ALL DONE (up to 25 min)"
T=0
while [ $T -lt 150 ]; do
  if env $SSH "grep -q 'ALL DONE' /home/wai/svm_exp_out/crypto-$TIER.log 2>/dev/null" 2>/dev/null; then break; fi
  if ! pgrep -f "qemu-system-x86_64" >/dev/null; then
    echo "QEMU_DOWN"; env $SSH "tail -5 /home/wai/svm_exp_out/crypto-$TIER.run 2>/dev/null" 2>/dev/null
    exit 3
  fi
  sleep 10; T=$((T+1))
done
if [ $T -ge 150 ]; then echo "TIMEOUT"; env $SSH "tail -5 /home/wai/svm_exp_out/crypto-$TIER.run 2>/dev/null" 2>/dev/null; exit 4; fi
echo "### [$(date +%T)] DONE, collect"
env $SSH "cat /home/wai/svm_exp_out/crypto-$TIER.log" > $LOGSD/crypto-$MODE-$TIER.log
echo "COLLECTED $LOGSD/crypto-$MODE-$TIER.log"
wc -l $LOGSD/crypto-$MODE-$TIER.log
