#!/bin/bash
# guest_m15.sh - deployed to guest /home/wai/m15run.sh by run_m1m5_blk.sh
# Usage: m15run.sh [runtime] [iodepth]
# Runs the 5 blocksize fio benchmarks (randrw 7:3) sequentially.
# Emits "BS_START bs=<name>" to progress.log before each fio so the host can
# sample process memory in sync with each blocksize segment.
# Outputs json per bs to /tmp/m15/, progress to /tmp/m15/progress.log.
set -u
RUNTIME=${1:-30}
IODEPTH=${2:-64}
mkdir -p /tmp/m15
rm -f /tmp/m15/bs*.json /tmp/m15/progress.log
for bs in 16k 64k 256k 1m 2m; do
  echo "BS_START bs=$bs" >> /tmp/m15/progress.log
  fio --name=m15 --filename=/dev/vdb --rw=randrw --rwmixread=70 \
      --bs=$bs --numjobs=1 --iodepth=$IODEPTH --ioengine=libaio --direct=1 \
      --runtime=$RUNTIME --time_based --group_reporting --output-format=json \
      > /tmp/m15/bs$bs.json 2>&1
  if [ -s /tmp/m15/bs$bs.json ]; then
    iops=$(grep -o '"iops": [0-9.]*' /tmp/m15/bs$bs.json | head -1)
    echo "bs=$bs done: $iops" >> /tmp/m15/progress.log
  else
    echo "bs=$bs FAILED (empty json)" >> /tmp/m15/progress.log
  fi
done
echo ALL_DONE >> /tmp/m15/progress.log
