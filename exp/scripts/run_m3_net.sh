#!/bin/bash
# run_m3_net.sh <proto>
#   proto: nvmeof | iscsi
# Runs M3 fio (randrw 70/30, bs=4k, 1job, iodepth=32, direct=1, 60s) in guest
# with host-loopback RTT injection via tc netem on lo.
# RTT mapping: nominal RTT {0,50,100,150}us -> netem delay {0,25,50,75}us (delay=RTT/2).
set -u
PROTO=$1
H=/home/waiai/svm
G=/home/wai/svm_exp_out
RES=$H/exp/results/network/logs
mkdir -p $RES
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"

# nominal RTT -> netem one-way delay
declare -A DELAY=([0]=0 [50]=25 [100]=50 [150]=75)

for RTT in 0 50 100 150; do
  D=${DELAY[$RTT]}
  if [ "$D" = "0" ]; then
    echo 'dxeqqghk' | sudo -S tc qdisc del dev lo root 2>/dev/null
  else
    echo 'dxeqqghk' | sudo -S tc qdisc replace dev lo root netem delay ${D}us 2>/dev/null
  fi
  sleep 1
  echo "### [$(date +%T)] $PROTO RTT=$RTT delay=${D}us"
  for RUN in 1 2 3; do
    env $SSH "rm -f $G/m3-$PROTO-rtt$RTT-r$RUN.json $G/m3-$PROTO-rtt$RTT-r$RUN.done"
    env $SSH "echo wai | sudo -S bash -c 'nohup fio --name=M3 --filename=/dev/vdb --rw=randrw --rwmixread=70 --bs=4k --numjobs=1 --iodepth=32 --ioengine=libaio --direct=1 --runtime=60 --time_based --group_reporting --output-format=json --output=$G/m3-$PROTO-rtt$RTT-r$RUN.json >/dev/null 2>&1; echo DONE > $G/m3-$PROTO-rtt$RTT-r$RUN.done'" 2>/dev/null
    # wait for completion (fio 60s + margin)
    OK=0
    for i in $(seq 1 36); do
      if env $SSH "test -f $G/m3-$PROTO-rtt$RTT-r$RUN.done" 2>/dev/null; then OK=1; break; fi
      sleep 5
    done
    if [ $OK = 1 ]; then
      env $SSH "cat $G/m3-$PROTO-rtt$RTT-r$RUN.json" > $RES/m3-$PROTO-rtt$RTT-r$RUN.json
      echo "  collected rtt=$RTT run=$RUN"
    else
      echo "  TIMEOUT rtt=$RTT run=$RUN"
    fi
  done
done

echo 'dxeqqghk' | sudo -S tc qdisc del dev lo root 2>/dev/null
echo "### [$(date +%T)] $PROTO done"
