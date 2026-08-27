#!/bin/bash
# stall_watch.sh: run a long fio in the guest; on I/O stall (in-flight frozen),
# gdb-dump both local qemu and stub thread stacks, then kill fio.
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"

# launch 300s randrw stress (reads+writes, 64 in-flight)
env $SSH "echo wai | sudo -S pkill -9 fio 2>/dev/null; echo wai | sudo -S bash -c 'nohup fio --name=STRESS --filename=/dev/vdb --rw=randrw --rwmixread=70 --bs=4k --numjobs=4 --iodepth=16 --ioengine=libaio --direct=1 --runtime=300 --time_based --group_reporting > /tmp/stress.log 2>&1 < /dev/null &' && echo STRESS_LAUNCHED"

Q=$(pgrep -f "qemu-system-x86_64" | head -1)
S=$(pgrep -f "remote-stub" | head -1)
echo "qemu=$Q stub=$S"

prev=""
FROZEN=0
for i in $(seq 1 80); do
  sleep 5
  # sample: reads + in-flight for vdb
  stats=$(env $SSH "grep vdb /proc/diskstats" 2>/dev/null)
  read_ios=$(echo "$stats" | awk '{print $4}')
  wr_ios=$(echo "$stats" | awk '{print $8}')
  inflight=$(echo "$stats" | awk '{print $12}')
  fio_alive=$(env $SSH "pgrep -c fio" 2>/dev/null)
  if [ "$fio_alive" = "0" ] || [ -z "$fio_alive" ]; then
    echo "[$i] fio finished (no stall). reads=$read_ios"
    exit 0
  fi
  cur="${read_ios}:${wr_ios}"
  if [ "$cur" = "$prev" ]; then
    FROZEN=$((FROZEN+1))
  else
    FROZEN=0
  fi
  if [ $FROZEN -ge 2 ]; then
    echo "[$i] STALL DETECTED: reads=$read_ios writes=$wr_ios inflight=$inflight"
    echo "=== dumping thread stacks ==="
    gdb -p $Q -batch -ex "set pagination off" -ex "thread apply all bt 12" 2>/dev/null > /tmp/qemu_stall.txt
    gdb -p $S -batch -ex "set pagination off" -ex "thread apply all bt 12" 2>/dev/null > /tmp/stub_stall.txt
    echo "dumped to /tmp/qemu_stall.txt /tmp/stub_stall.txt"
    env $SSH "echo wai | sudo -S pkill -9 fio" 2>/dev/null
    exit 1
  fi
  prev=$cur
  if [ $((i % 6)) -eq 0 ]; then echo "[$i] reads=$read_ios writes=$wr_ios inflight=$inflight"; fi
done
echo "no stall in window"
exit 0
