#!/bin/bash
# run_m3rtt.sh - M3 RTT sweep on SVM t0 (W=4): fio 4K randrw(70/30) iodepth=32
# numjobs=1 60s direct=1, with tc netem delay injected on host lo (one-way).
#   RTT = 100us (loopback) + 2*delay
#   delay: 0/50/200/450/950/2450/4950/9950us -> RTT 0.1/0.2/0.5/1/2/5/10/20ms
# 2 runs per point, host randrd anchors before/after.
set -u
H=/home/waiai/svm
OUT=$H/exp/results/m3rtt
mkdir -p $OUT
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=10 -p 2222 wai@127.0.0.1"
TC() { echo dxeqqghk | sudo -S tc "$@" >/dev/null 2>&1; }
DROPC() { echo dxeqqghk | sudo -S sh -c 'echo 3 > /proc/sys/vm/drop_caches' >/dev/null 2>&1; }
LABELS=(0p1ms 0p2ms 0p5ms 1ms 2ms 5ms 10ms 20ms)
DELAYS=(0 50 200 450 950 2450 4950 9950)

echo "=== [$(date +%T)] start SVM t0 ==="
bash $H/exp/host_switch.sh a 0 || { echo SWITCH_FAIL; exit 1; }

echo "=== [$(date +%T)] host anchor before ==="
DROPC
fio --name=before --filename=$H/exp/remote/fio.raw --rw=randread --bs=4k \
    --numjobs=4 --iodepth=16 --ioengine=libaio --direct=1 --runtime=60 \
    --time_based --group_reporting --output-format=json 2>/dev/null > $OUT/anchor_before.json

for i in ${!DELAYS[@]}; do
  d=${DELAYS[$i]}; lbl=${LABELS[$i]}
  # (re)install netem
  TC qdisc del dev lo root 2>/dev/null
  if [ $d -gt 0 ]; then
    TC qdisc add dev lo root netem delay ${d}us || { echo "TC_FAIL $lbl"; exit 2; }
  fi
  RPING=$(ping -c3 -q 127.0.0.1 2>/dev/null | tail -1 | grep -oP '=\K[0-9.]+/[0-9.]+/[0-9.]+' || echo "?")
  echo "### [$(date +%T)] RTT $lbl (delay ${d}us, ping avg/mdev=$RPING)"
  for r in 1 2; do
    env $SSH "echo wai | sudo -S fio --name=M3RTT --filename=/dev/vdb --rw=randrw --rwmixread=70 --bs=4k --numjobs=1 --iodepth=32 --ioengine=libaio --direct=1 --runtime=60 --time_based --group_reporting --output-format=json" 2>/dev/null \
      > $OUT/rtt_${lbl}_r$r.json
    python3 -c "
import json
raw=open('$OUT/rtt_${lbl}_r$r.json').read()
d,_=json.JSONDecoder().raw_decode(raw)
j=d['jobs'][0]
r=j['read']; w=j['write']
tot=r['iops']+w['iops']
print(f'  RTT $lbl r$r: r={r[\"iops\"]:.0f} w={w[\"iops\"]:.0f} tot={tot:.0f} clat={r[\"clat_ns\"][\"mean\"]/1000:.0f}us')
"
  done
done

TC qdisc del dev lo root 2>/dev/null
echo "=== [$(date +%T)] host anchor after ==="
DROPC
fio --name=after --filename=$H/exp/remote/fio.raw --rw=randread --bs=4k \
    --numjobs=4 --iodepth=16 --ioengine=libaio --direct=1 --runtime=60 \
    --time_based --group_reporting --output-format=json 2>/dev/null > $OUT/anchor_after.json

# anchor summary
python3 -c "
import json
for tag in ('before','after'):
    raw=open('$OUT/anchor_$tag.json').read()
    d,_=json.JSONDecoder().raw_decode(raw)
    j=d['jobs'][0]['read']
    print(f'anchor $tag randrd: {j[\"iops\"]:.0f} clat={j[\"clat_ns\"][\"mean\"]/1000:.0f}us')
"
echo "M3RTT_OK [$(date +%T)]"
