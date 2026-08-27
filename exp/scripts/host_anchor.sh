#!/bin/bash
# host_anchor.sh <tag>   e.g. t0a t0b t5a ...
# Host bare-disk anchor: randrd 60s + randwt 60s (O_DIRECT, drop_caches),
# written to exp/results/m2g/anchors/ as JSON files.
set -u
TAG=$1
H=/home/waiai/svm
OUT=$H/exp/results/m2g/anchors
mkdir -p $OUT
SUDO_A="SUDO_ASKPASS=/tmp/sudo_askpass.sh sudo -A"

echo "### [$(date +%T)] host anchor $TAG"
env $SUDO_A sh -c 'echo 3 > /proc/sys/vm/drop_caches'
fio --name=anchor_${TAG}_randrd --filename=$H/exp/remote/fio.raw --rw=randread \
    --bs=4k --numjobs=4 --iodepth=16 --ioengine=libaio --direct=1 \
    --runtime=60 --time_based --group_reporting --output-format=json 2>/dev/null \
    > $OUT/host_${TAG}_randrd.json
python3 -c "
import json
raw=open('$OUT/host_${TAG}_randrd.json').read()
d,_=json.JSONDecoder().raw_decode(raw)
j=d['jobs'][0]['read']
print(f'  anchor $TAG randrd: {j[\"iops\"]:.0f} iops clat={j[\"clat_ns\"][\"mean\"]/1000:.0f}us')
"
env $SUDO_A sh -c 'echo 3 > /proc/sys/vm/drop_caches'
fio --name=anchor_${TAG}_randwt --filename=$H/exp/remote/fio.raw --rw=randwrite \
    --bs=4k --numjobs=4 --iodepth=16 --ioengine=libaio --direct=1 \
    --runtime=60 --time_based --group_reporting --output-format=json 2>/dev/null \
    > $OUT/host_${TAG}_randwt.json
python3 -c "
import json
raw=open('$OUT/host_${TAG}_randwt.json').read()
d,_=json.JSONDecoder().raw_decode(raw)
j=d['jobs'][0]['write']
print(f'  anchor $TAG randwt: {j[\"iops\"]:.0f} iops clat={j[\"clat_ns\"][\"mean\"]/1000:.0f}us')
"
echo "ANCHOR_OK $TAG"
