#!/bin/bash
# run_app_rtt.sh <kvm|svm|nvmeof|iscsi>
# App (Spark pagerank) with synthetic loopback RTT (tc netem delay ${DELAY_US:-50}us each way
# on host lo -> ~50us baseline + 100us = ~150us effective RTT).
#   kvm    : local virtio-blk on fio.raw (NOT hit by lo netem) -> anchor
#   svm    : local qemu <-> stub over 127.0.0.1:5552 (hit by netem), W=4 I=256 B=16
#   nvmeof : nvmet -> loop(direct-io) -> fio.raw -> /dev/nvme1n1 (hit by netem)
#   iscsi  : LIO block -> loop(direct-io) -> data_iscsi.raw -> /dev/sda (hit by netem)
# RTT injected AFTER boot (0-RTT boot avoids MySQL InnoDB recovery slowdown; App does not
# use MySQL anyway). cores {1,2,4} x 1 run, 10 iterations, drop_caches per run.
# Graceful shutdown. Output -> exp/results/app-rtt/$PROTO/
set -u
DELAY_US=${DELAY_US:-50}
PROTO=$1
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
OUT=${APP_RTT_OUT:-$H/exp/results/app-rtt/$PROTO}
mkdir -p $OUT/logs
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
CS=(1 2 4)
DEV=/dev/nvme1n1
[ "$PROTO" = "iscsi" ] && DEV=/dev/sda
NVMET_NQN=nqn.2026-08.svm:data
ISCSI_IQN=iqn.2026-08.com.svm:data
ISCSI_INIT=iqn.2026-08.com.svm:init
S=/sys/kernel/config/nvmet

sudoc() { echo dxeqqghk | sudo -S bash -c "$1"; }

add_netem() {
  sudoc "tc qdisc del dev lo root 2>/dev/null; tc qdisc add dev lo root netem delay ${DELAY_US}us"
  echo "  [$(date +%T)] lo netem: delay ${DELAY_US}us (eff RTT ~$((50+2*DELAY_US))us)"
}
del_netem() { sudoc "tc qdisc del dev lo root 2>/dev/null"; }

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

shutdown_guest() {
  if env $SSH "true" 2>/dev/null; then
    timeout 90 env $SSH "echo wai | sudo -S bash -c 'sync; poweroff'" 2>/dev/null
    sleep 20
  fi
  pkill -9 -f "qemu-syste[m]" 2>/dev/null; pkill -9 -f "remote-[s]tub" 2>/dev/null; sleep 3
  del_netem
}

# ---------------- nvmeof target ----------------
setup_nvmeof() {
  sudoc "modprobe nvme_tcp nvme_fabrics nvmet" >/dev/null 2>&1
  sudoc "nvme disconnect -n $NVMET_NQN 2>/dev/null"
  sudoc "rm -f $S/ports/1/subsystems/$NVMET_NQN; rmdir $S/ports/1/subsystems $S/ports/1/referrals 2>/dev/null; rmdir $S/ports/1/ana_groups/1 2>/dev/null; rmdir $S/ports/1/ana_groups $S/ports/1 2>/dev/null; echo 0 > $S/subsystems/$NVMET_NQN/namespaces/1/enable 2>/dev/null; rmdir $S/subsystems/$NVMET_NQN/namespaces/1 2>/dev/null; rmdir $S/subsystems/$NVMET_NQN 2>/dev/null"
  sudoc "losetup -d \$(losetup -j $H/exp/remote/fio.raw 2>/dev/null | cut -d: -f1) 2>/dev/null"
  LOOP=$(echo dxeqqghk | sudo -S losetup -f 2>/dev/null | tr -d '\r')
  echo "  nvmeof loop=$LOOP"
  sudoc "losetup --direct-io=on $LOOP $H/exp/remote/fio.raw" || { echo "  LOOP DIO FAIL"; exit 1; }
  sudoc "mkdir -p $S/subsystems/$NVMET_NQN && echo 1 > $S/subsystems/$NVMET_NQN/attr_allow_any_host && mkdir -p $S/subsystems/$NVMET_NQN/namespaces/1 && echo -n $LOOP > $S/subsystems/$NVMET_NQN/namespaces/1/device_path && echo 1 > $S/subsystems/$NVMET_NQN/namespaces/1/enable"
  sudoc "mkdir -p $S/ports/1 && echo tcp > $S/ports/1/addr_trtype && echo ipv4 > $S/ports/1/addr_adrfam && echo 127.0.0.1 > $S/ports/1/addr_traddr && echo 4420 > $S/ports/1/addr_trsvcid && ln -s $S/subsystems/$NVMET_NQN $S/ports/1/subsystems/$NVMET_NQN"
  sudoc "nvme connect -t tcp -n $NVMET_NQN -a 127.0.0.1 -s 4420" || { echo "  NVME CONNECT FAIL"; exit 1; }
  sleep 1; sudoc "chmod 666 $DEV"
}
teardown_nvmeof() {
  sudoc "nvme disconnect -n $NVMET_NQN 2>/dev/null"
  sudoc "rm -f $S/ports/1/subsystems/$NVMET_NQN; rmdir $S/ports/1/subsystems $S/ports/1/referrals 2>/dev/null; rmdir $S/ports/1/ana_groups/1 2>/dev/null; rmdir $S/ports/1/ana_groups $S/ports/1 2>/dev/null; echo 0 > $S/subsystems/$NVMET_NQN/namespaces/1/enable 2>/dev/null; rmdir $S/subsystems/$NVMET_NQN/namespaces/1 2>/dev/null; rmdir $S/subsystems/$NVMET_NQN 2>/dev/null; losetup -d \$(losetup -j $H/exp/remote/fio.raw 2>/dev/null | cut -d: -f1) 2>/dev/null"
}

# ---------------- iscsi target ----------------
setup_iscsi() {
  sudoc "modprobe target_core_mod iscsi_target_mod" >/dev/null 2>&1
  sudoc "iscsiadm -m node -T $ISCSI_IQN -p 127.0.0.1:3260 -u 2>/dev/null"
  sudoc "targetcli /iscsi delete $ISCSI_IQN 2>/dev/null"
  sudoc "targetcli /backstores/block delete svmmacro 2>/dev/null"
  sudoc "losetup -d \$(losetup -j $H/exp/iscsi/data_iscsi.raw 2>/dev/null | cut -d: -f1) 2>/dev/null"
  LOOP=$(echo dxeqqghk | sudo -S losetup -f 2>/dev/null | tr -d '\r')
  echo "  iscsi loop=$LOOP"
  sudoc "losetup --direct-io=on $LOOP $H/exp/iscsi/data_iscsi.raw" || { echo "  LOOP DIO FAIL"; exit 1; }
  sudoc "targetcli /backstores/block create name=svmmacro dev=$LOOP"
  sudoc "targetcli /iscsi create $ISCSI_IQN"
  sudoc "targetcli /iscsi/$ISCSI_IQN/tpg1/luns create /backstores/block/svmmacro"
  sudoc "targetcli /iscsi/$ISCSI_IQN/tpg1/acls create $ISCSI_INIT"
  sudoc "echo 'InitiatorName=$ISCSI_INIT' > /etc/iscsi/initiatorname.iscsi"
  sudoc "systemctl restart iscsid"
  sudoc "iscsiadm -m discovery -t sendtargets -p 127.0.0.1:3260"
  sudoc "iscsiadm -m node -T $ISCSI_IQN -p 127.0.0.1:3260 -l" || { echo "  ISCSI LOGIN FAIL"; exit 1; }
  sleep 1; sudoc "chmod 666 $DEV"
}
teardown_iscsi() {
  sudoc "iscsiadm -m node -T $ISCSI_IQN -p 127.0.0.1:3260 -u 2>/dev/null"
  sudoc "targetcli /iscsi delete $ISCSI_IQN 2>/dev/null"
  sudoc "targetcli /backstores/block delete svmmacro 2>/dev/null"
  sudoc "losetup -d \$(losetup -j $H/exp/iscsi/data_iscsi.raw 2>/dev/null | cut -d: -f1) 2>/dev/null"
}

run_app() {
  echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
  env $SSH "export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64; export SPARK_HOME=/mnt/data/spark; export SPARK_LOCAL_DIRS=/mnt/data/spark-tmp; cd /home/wai && timeout 3600 \$SPARK_HOME/bin/spark-submit --master local[$2] /home/wai/pagerank.py /mnt/data/wiki/wiki-Talk-1m.txt 10 $2" 2>/dev/null \
    > $OUT/logs/$1-c$2-r1.log
  parse $OUT/logs/$1-c$2-r1.log "$1 c$2 r1"
}

echo "=== [$(date +%T)] APP-RTT $PROTO (eff RTT ~$((50+2*DELAY_US))us): start ==="

if [ "$PROTO" = "kvm" ]; then
  pkill -f 'qemu-syste[m]' 2>/dev/null; pkill -f 'remote-[s]tub' 2>/dev/null; sleep 2
  setsid $H/local_qemu/build/qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 4 -m 4096 \
    -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive0,bootindex=1 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -drive file=$H/exp/remote/fio.raw,if=none,id=drive1,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive1,serial=data-disk \
    -serial file:/tmp/app-rtt-guest.log -monitor none -display none \
    -qmp unix:/tmp/qmp.sock,server,nowait \
    > /tmp/app-rtt-out.log 2>&1 < /dev/null &
  wait_ssh
  mount_data
  env $SSH "echo wai | sudo -S chown -R wai:wai /mnt/data/spark-tmp" 2>/dev/null
  add_netem
  env $SSH "echo wai | sudo -S dd if=/dev/vdb of=/dev/null bs=1M count=128 2>&1 | tail -1" 2>/dev/null
  for c in "${CS[@]}"; do run_app "kvm" $c; done
  shutdown_guest
elif [ "$PROTO" = "svm" ]; then
  cat > $CONF <<EOF
# SVM App-RTT: W=4 I=256 B=16 (run_app_rtt.sh generated)
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
  shutdown_guest
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
  add_netem
  env $SSH "echo wai | sudo -S dd if=/dev/vdb of=/dev/null bs=1M count=128 2>&1 | tail -1" 2>/dev/null
  for c in "${CS[@]}"; do run_app "svm" $c; done
  shutdown_guest
  cat > $CONF <<EOF
# SVM 参数档 0: Z=1048576 W=4 I=32 B=16 P=off (restored by run_app_rtt.sh)
buf_pool=0
inflight_size=32
local_batch_n=16
stub_batch_n=16
zc_send_min=1048576
workers=4
EOF
else
  if [ "$PROTO" = "nvmeof" ]; then setup_nvmeof; else setup_iscsi; fi
  [ -e $DEV ] || { echo "DEVICE $DEV MISSING"; exit 1; }
  pkill -f 'qemu-syste[m]' 2>/dev/null; pkill -f 'remote-[s]tub' 2>/dev/null; sleep 2
  setsid $H/local_qemu/build/qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 4 -m 4096 \
    -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive0,bootindex=1 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0 \
    -drive file=$DEV,if=none,id=drive1,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive1,serial=data-disk \
    -serial file:/tmp/app-rtt-guest.log -monitor none -display none \
    -qmp unix:/tmp/qmp.sock,server,nowait \
    > /tmp/app-rtt-out.log 2>&1 < /dev/null &
  wait_ssh
  mount_data
  env $SSH "echo wai | sudo -S chown -R wai:wai /mnt/data/spark-tmp" 2>/dev/null
  add_netem
  env $SSH "echo wai | sudo -S dd if=/dev/vdb of=/dev/null bs=1M count=128 2>&1 | tail -1" 2>/dev/null
  for c in "${CS[@]}"; do run_app "$PROTO" $c; done
  shutdown_guest
  if [ "$PROTO" = "nvmeof" ]; then teardown_nvmeof; else teardown_iscsi; fi
fi

del_netem
echo "APP_RTT_${PROTO}_OK [$(date +%T)]"
