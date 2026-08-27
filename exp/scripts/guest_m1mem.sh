#!/bin/bash
# guest_m1mem.sh - single-bs fio for M1 mem boxplot.
#   Usage: m1mem.sh <bs> <runtime> [iodepth]
#   randrw 7:3 on /dev/vdb, direct=1. Emits BS_START then ALL_DONE to
#   /tmp/m15/progress.log so the host can sync 0.5s mem sampling.
set -u
BS=${1:?bs required}
RUNTIME=${2:-30}
IODEPTH=${3:-1}
mkdir -p /tmp/m15
rm -f /tmp/m15/bs.json /tmp/m15/progress.log
echo "BS_START bs=$BS" > /tmp/m15/progress.log
fio --name=m1mem --filename=/dev/vdb --rw=randrw --rwmixread=70 \
    --bs=$BS --numjobs=1 --iodepth=$IODEPTH --ioengine=libaio --direct=1 \
    --runtime=$RUNTIME --time_based --group_reporting --output-format=json \
    > /tmp/m15/bs.json 2>&1
echo ALL_DONE >> /tmp/m15/progress.log
