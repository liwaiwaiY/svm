#!/bin/bash
# run_m2g.sh <tier>   tier in {4,5,0}
# M2 60s back-to-back re-run (2026-08-24): numjobs=4, iodepth=16, 60s,
# host-anchor methodology. Orchestrates: host_switch -> deploy runner ->
# guest phase-g -> wait ALL DONE -> snapshot m2g jsonl + logs.
set -u
TIER=$1
H=/home/waiai/svm
RES=$H/exp/results/m2g
G=/home/wai/svm_exp_out
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
SCP="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh scp -o StrictHostKeyChecking=no -P 2222"
mkdir -p $RES/logs

echo "### [$(date +%T)] M2G tier $TIER: host_switch"
bash $H/exp/host_switch.sh a $TIER || { echo SWITCH_FAIL; exit 1; }

echo "### [$(date +%T)] M2G tier $TIER: deploy runner"
env $SCP $H/exp/scripts/svm_runner.py wai@127.0.0.1:/home/wai/svm_runner.py 2>&1

echo "### [$(date +%T)] M2G tier $TIER: launch"
env $SSH "echo wai | sudo -S pkill -f svm_runner.py 2>/dev/null; pkill -f '^fio ' 2>/dev/null; rm -f $G/run-g$TIER.log; sleep 1; true"
env $SSH "echo wai | sudo -S bash -c 'nohup python3 /home/wai/svm_runner.py g $TIER > $G/run-g$TIER.log 2>&1 < /dev/null &' && echo LAUNCHED"

# wait for completion (12 fio runs x 60s + margins, up to 30 min)
T=0
while [ $T -lt 90 ]; do
  if env $SSH "grep -q 'ALL DONE' $G/run-g$TIER.log 2>/dev/null" 2>/dev/null; then
    break
  fi
  if ! pgrep -f "qemu-system-x86_64" >/dev/null || ! pgrep -f "remote-stub" >/dev/null; then
    echo "### [$(date +%T)] M2G tier $TIER: QEMU/STUB DOWN"
    env $SSH "tail -5 $G/run-g$TIER.log 2>/dev/null" 2>/dev/null
    echo PROC_DOWN
    exit 2
  fi
  sleep 20
  T=$((T+1))
done
if [ $T -ge 90 ]; then
  echo "### [$(date +%T)] M2G tier $TIER: TIMEOUT"
  env $SSH "tail -5 $G/run-g$TIER.log 2>/dev/null" 2>/dev/null
  echo TIMEOUT
  exit 3
fi
echo "### [$(date +%T)] M2G tier $TIER: guest DONE"

# per-tier snapshot (guest jsonl accumulates across tiers)
env $SSH "cat $G/m2g_svm.jsonl 2>/dev/null" > $RES/m2g_t$TIER.jsonl
env $SSH "tar -C $G -cf - logs 2>/dev/null" | tar -C $RES -xf - 2>/dev/null
echo "jsonl lines: $(wc -l < $RES/m2g_t$TIER.jsonl)"
echo "TIER_OK"
