#!/bin/bash
# run_app_svm.sh - App (Spark pagerank) on SVM, data on remote fio.raw via stub.
#   Fixed tier: W=4, inflight=256, batch=16 (user-specified). SPARK_HOME + wiki on /mnt/data.
#   cores {1,2,4} x 1 run, 10 iterations, drop_caches per run, graceful shutdown.
#   Output -> exp/results/app-net/svm/
set -u
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
OUT=$H/exp/results/app-net/svm
mkdir -p $OUT/logs
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
CS=(1 2 4)

wait_ssh() {
  for i in $(seq 1 90); do
    if env $SSH "true" 2>/dev/null; then echo "  SSH up after $i"; return 0; fi
    sleep 3
  done
  echo "  SSH FAILED"; exit 1
}

mount_data() {
  env $SSH "echo wai | sudo -S bash -c '
    if ! grep -q /mnt/data /proc/mounts; then mount /dev/vdb /mnt/data || exit 1; fi
  '" 2>/dev/null
}

parse() {
  python3 -c "
import re
s=open('$1').read()
links=re.search(r'links=(\d+)', s)
build=re.search(r'build_s=([\d.]+)', s)
sort=re.search(r'sort_s=([\d.]+)', s)
iter=re.search(r'iter_s=([\d.,]+)', s)
total=re.search(r'total_s=([\d.]+)', s)
it=iter.group(1).split(',') if iter else []
print('  $2: links=%s build=%ss sort=%ss iter_sum=%ss total=%ss' % (
  links.group(1) if links else '?',
  build.group(1) if build else '?',
  sort.group(1) if sort else '?',
  '%.1f' % sum(map(float, it)) if it else '?',
  total.group(1) if total else '?'))
" 2>/dev/null || echo "  $2: PARSE FAIL"
}

echo "=== [$(date +%T)] APP SVM (W=4 I=256 B=16, data on remote fio.raw): start ==="
cat > $CONF <<EOF
# SVM App: W=4 I=256 B=16 (run_app_svm.sh generated)
buf_pool=0
inflight_size=256
local_batch_n=16
stub_batch_n=16
local_batch_m=65536
stub_batch_m=65536
zc_send_min=1048576
workers=4
EOF
echo "vr.conf -> W=4 I=256 B=16"
if env $SSH "true" 2>/dev/null; then
  env $SSH "echo wai | sudo -S bash -c 'systemctl stop mysql; sleep 2; poweroff'" 2>/dev/null
  sleep 12
fi
pkill -f "remote-[s]tub" 2>/dev/null; pkill -f "qemu-syste[m]" 2>/dev/null; sleep 3
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
wait_ssh
mount_data
env $SSH "echo wai | sudo -S chown -R wai:wai /mnt/data/spark-tmp" 2>/dev/null
env $SSH "echo wai | sudo -S dd if=/dev/vdb of=/dev/null bs=1M count=128 2>&1 | tail -1" 2>/dev/null
for c in "${CS[@]}"; do
  echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
  env $SSH "export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64; export SPARK_HOME=/mnt/data/spark; export SPARK_LOCAL_DIRS=/mnt/data/spark-tmp; cd /home/wai && timeout 3600 \$SPARK_HOME/bin/spark-submit --master local[$c] /home/wai/pagerank.py /mnt/data/wiki/wiki-Talk-1m.txt 10 $c" 2>/dev/null \
    > $OUT/logs/svm-c$c-r1.log
  parse $OUT/logs/svm-c$c-r1.log "SVM c$c r1"
done
env $SSH "echo wai | sudo -S bash -c 'sync; poweroff'" 2>/dev/null
sleep 12
pkill -f "qemu-syste[m]" 2>/dev/null; pkill -f "remote-[s]tub" 2>/dev/null
cat > $CONF <<EOF
# SVM 参数档 0: Z=1048576 W=4 I=32 B=16 P=off (restored by run_app_svm.sh)
buf_pool=0
inflight_size=32
local_batch_n=16
stub_batch_n=16
zc_send_min=1048576
workers=4
EOF
echo "APP_SVM_OK [$(date +%T)]"
