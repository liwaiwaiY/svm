#!/bin/bash
# run_tier.sh <phase> <tier>
#   phase: a (fio, scratch fio.raw) | b (macro+app, data.raw)
#   tier:  0/2/4/5/6/7/8
# Orchestrates: host_switch (vr.conf + restart) -> guest runner (nohup) -> wait -> copy jsonl/logs back.
set -u
PHASE=$1
TIER=$2
H=/home/waiai/svm
RES=$H/exp_results
G=/home/wai/svm_exp_out
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"

echo "### [$(date +%T)] $PHASE tier $TIER: host_switch"
if [ "${SKIP_SWITCH:-0}" != "1" ]; then
  bash $H/exp/host_switch.sh $PHASE $TIER || { echo "SWITCH_FAIL"; exit 1; }
else
  echo "### [$(date +%T)] skip host_switch (env already at $PHASE tier $TIER)"
fi

echo "### [$(date +%T)] $PHASE tier $TIER: launch guest runner"
if [ "${SKIP_LAUNCH:-0}" != "1" ]; then
  # kill any stray runner/fio first
  env $SSH "echo wai | sudo -S pkill -f svm_runner.py 2>/dev/null; pkill -f '^fio ' 2>/dev/null; sleep 1; true"
  env $SSH "echo wai | sudo -S bash -c 'nohup python3 /home/wai/svm_runner.py $PHASE $TIER > $G/run-$PHASE$TIER.log 2>&1 < /dev/null &' && echo LAUNCHED"
else
  echo "### [$(date +%T)] skip launch (runner already running)"
fi

# wait for completion (up to 90 min) with qemu/stub liveness check
T=0
while [ $T -lt 270 ]; do
  if env $SSH "grep -q 'ALL DONE' $G/run-$PHASE$TIER.log 2>/dev/null" 2>/dev/null; then
    break
  fi
  if ! pgrep -f "qemu-system-x86_64" >/dev/null || ! pgrep -f "remote-stub" >/dev/null; then
    echo "### [$(date +%T)] $PHASE tier $TIER: QEMU/STUB DOWN (check guest log)"
    env $SSH "tail -5 $G/run-$PHASE$TIER.log 2>/dev/null" 2>/dev/null
    echo "PROC_DOWN"
    exit 2
  fi
  sleep 20
  T=$((T+1))
done
if [ $T -ge 270 ]; then
  echo "### [$(date +%T)] $PHASE tier $TIER: TIMEOUT"
  env $SSH "tail -5 $G/run-$PHASE$TIER.log 2>/dev/null" 2>/dev/null
  echo "TIMEOUT"
  exit 3
fi
echo "### [$(date +%T)] $PHASE tier $TIER: guest DONE"

# copy results back
mkdir -p $RES $RES/logs_svm
env $SSH "cat $G/m2_svm.jsonl 2>/dev/null" > $RES/m2_svm.jsonl
env $SSH "cat $G/m3_svm.jsonl 2>/dev/null" > $RES/m3_svm.jsonl
env $SSH "cat $G/m4_svm.jsonl 2>/dev/null" > $RES/m4_svm.jsonl
mkdir -p $RES/macro $RES/app
env $SSH "cat $G/macro_svm.jsonl 2>/dev/null" > $RES/macro/svm.jsonl
env $SSH "cat $G/app_svm.jsonl 2>/dev/null" > $RES/app/svm.jsonl
env $SSH "tar -C $G -cf - logs" | tar -C $RES/logs_svm -xf - 2>/dev/null
echo "### [$(date +%T)] $PHASE tier $TIER: copied. jsonl lines:"
wc -l $RES/m2_svm.jsonl $RES/m3_svm.jsonl $RES/m4_svm.jsonl $RES/macro/svm.jsonl $RES/app/svm.jsonl 2>/dev/null
echo "TIER_OK"
