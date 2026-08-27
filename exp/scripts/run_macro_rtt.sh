#!/bin/bash
# run_macro_rtt.sh <kvm|svm|nvmeof|iscsi>
# Macro (sysbench TPCC) rerun with synthetic loopback RTT (tc netem delay ${DELAY_US:-50}us each way
# on host lo -> ~50us baseline + 100us = ~150us effective RTT, "near-rack" data center).
#   kvm    : local virtio-blk on fio.raw (NOT hit by lo netem) -> anchor
#   svm    : local qemu <-> stub over 127.0.0.1:5552 (hit by netem), tiers t4/t5/t0 (W=threads)
#   nvmeof : nvmet -> loop(direct-io) -> fio.raw -> /dev/nvme1n1 (hit by netem)
#   iscsi  : LIO block -> loop(direct-io) -> data_iscsi.raw -> /dev/sda (hit by netem)
# threads {1,2,4} x 3 runs, 60s, cache=none + drop_caches per run, 20s warmup per boot.
# RTT is injected AFTER boot + MySQL ready (0-RTT boot keeps InnoDB recovery fast), then
# removed at each shutdown. Graceful shutdown (stop mysql -> poweroff). Output -> exp/results/macrortt/$PROTO/
set -u
DELAY_US=${DELAY_US:-50}
PROTO=$1
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
OUT=$H/exp/results/macrortt/$PROTO
mkdir -p $OUT/logs
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
TS=(${TS_ONLY:-1 2 4})
DEV=/dev/nvme1n1
[ "$PROTO" = "iscsi" ] && DEV=/dev/sda
NVMET_NQN=nqn.2026-08.svm:data
ISCSI_IQN=iqn.2026-08.com.svm:data
ISCSI_INIT=iqn.2026-08.com.svm:init
S=/sys/kernel/config/nvmet

sudoc() { echo dxeqqghk | sudo -S bash -c "$1"; }

# lo netem: ${DELAY_US}us each way. Del first so it is idempotent.
add_netem() {
  sudoc "tc qdisc del dev lo root 2>/dev/null; tc qdisc add dev lo root netem delay ${DELAY_US}us"
  echo "  [$(date +%T)] lo netem: delay ${DELAY_US}us (eff RTT ~$((50+2*DELAY_US))us)"
}
del_netem() { sudoc "tc qdisc del dev lo root 2>/dev/null"; }

wait_ssh() {
  for i in $(seq 1 60); do
    if env $SSH "true" 2>/dev/null; then echo "  SSH up after $i"; return 0; fi
    sleep 3
  done
  echo "  SSH FAILED"; exit 1
}
wait_mysql() {
  for i in $(seq 1 200); do
    if env $SSH "mysql -h127.0.0.1 -P3306 -u sbtest -psbtest --connect-timeout=5 -N -e 'SELECT COUNT(*) FROM item1' tpcc 2>/dev/null | grep -qE '^[0-9]+$'" 2>/dev/null; then
      echo "  MySQL up (InnoDB ready) after $i"; return 0
    fi
    sleep 3
  done
  echo "  MySQL FAILED"; exit 1
}
parse() {
  python3 -c "
import re
s=open('$1').read()
txn=re.search(r'transactions:\s+(\d+)\s+\(([\d.]+) per sec', s)
lat=re.search(r'avg:\s+([\d.]+)', s)
p95=re.search(r'95th percentile:\s+([\d.]+)', s)
mx=re.search(r'max:\s+([\d.]+)', s)
print(f'  $2: txn={txn.group(1)} ({txn.group(2)}/s) avg={lat.group(1)}ms p95={p95.group(1)}ms max={mx.group(1)}ms')
" 2>/dev/null || echo "  $2: PARSE FAIL"
}
shutdown_guest() {
  if env $SSH "true" 2>/dev/null; then
    timeout 90 env $SSH "echo wai | sudo -S bash -c 'systemctl stop mysql; sleep 2; poweroff'" 2>/dev/null
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

# ---------------- guest ----------------
boot_guest() {
  pkill -f 'qemu-syste[m]' 2>/dev/null; pkill -f 'remote-[s]tub' 2>/dev/null; sleep 2
  setsid $H/local_qemu/build/qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 4 -m 4096 \
    -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive0,bootindex=1 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0 \
    -drive file=$DEV,if=none,id=drive1,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive1,serial=data-disk \
    -serial file:/tmp/macrortt-guest.log -monitor none -display none \
    -qmp unix:/tmp/qmp.sock,server,nowait \
    > /tmp/macrortt-local-out.log 2>&1 < /dev/null &
  wait_ssh
}

ensure_mysql_data() {
  env $SSH "echo wai | sudo -S bash -c '
    if ! grep -q /mnt/data /proc/mounts; then
      mount /dev/vdb /mnt/data || exit 1
    fi
    systemctl start mysql
  '" 2>/dev/null
}

run_bench() {
  # $1 = tag prefix for log name, $2 = threads
  echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
  env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=60 --threads=$2 ./tpcc.lua run" 2>/dev/null \
    > $OUT/logs/$1-r$3.log
  parse $OUT/logs/$1-r$3.log "$1 r$3"
}

echo "=== [$(date +%T)] MACRO-RTT $PROTO (eff RTT ~$((50+2*DELAY_US))us): start ==="

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
    -serial file:/tmp/macrortt-guest.log -monitor none -display none \
    -qmp unix:/tmp/qmp.sock,server,nowait \
    > /tmp/macrortt-local-out.log 2>&1 < /dev/null &
  wait_ssh
  wait_mysql
  add_netem
  env $SSH "echo wai | sudo -S dd if=/dev/vdb of=/dev/null bs=1M count=128 2>&1 | tail -1" 2>/dev/null
  echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
  env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=20 --threads=2 ./tpcc.lua run" > /dev/null 2>&1
  echo "  warmup done, runs [$(date +%T)]"
  for t in "${TS[@]}"; do
    for r in 1 2 3; do
      run_bench "kvm-t$t" $t $r
    done
  done
  shutdown_guest
elif [ "$PROTO" = "svm" ]; then
  declare -A W=( [t4]=1 [t5]=2 [t0]=4 )
  declare -A T=( [t4]=1 [t5]=2 [t0]=4 )
  TIERS=(t4 t5 t0)
  for tier in "${TIERS[@]}"; do
    cat > $CONF <<EOF
# SVM Macro-RTT $tier: W=${W[$tier]} I=32 B=16 (run_macro_rtt.sh generated)
buf_pool=0
inflight_size=32
local_batch_n=16
stub_batch_n=16
local_batch_m=65536
stub_batch_m=65536
zc_send_min=1048576
workers=${W[$tier]}
EOF
    echo "vr.conf -> $tier W=${W[$tier]}"
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
    wait_mysql
    add_netem
    env $SSH "echo wai | sudo -S dd if=/dev/vdb of=/dev/null bs=1M count=128 2>&1 | tail -1" 2>/dev/null
    echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
    env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=20 --threads=${T[$tier]} ./tpcc.lua run" > /dev/null 2>&1
    echo "  $tier warmup done, runs [$(date +%T)]"
    for r in 1 2 3; do
      run_bench "svm-$tier" ${T[$tier]} $r
    done
  done
  shutdown_guest
  cat > $CONF <<EOF
# SVM 参数档 0: Z=1048576 W=4 I=32 B=16 P=off (restored by run_macro_rtt.sh)
buf_pool=0
inflight_size=32
local_batch_n=16
stub_batch_n=16
zc_send_min=1048576
workers=4
EOF
else
  if [ "$PROTO" = "nvmeof" ]; then
    setup_nvmeof
  else
    setup_iscsi
  fi
  [ -e $DEV ] || { echo "DEVICE $DEV MISSING"; exit 1; }
  boot_guest
  ensure_mysql_data
  wait_mysql
  add_netem
  env $SSH "echo wai | sudo -S dd if=/dev/vdb of=/dev/null bs=1M count=128 2>&1 | tail -1" 2>/dev/null
  echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
  env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=20 --threads=2 ./tpcc.lua run" > /dev/null 2>&1
  echo "  warmup done, runs [$(date +%T)]"
  for t in "${TS[@]}"; do
    for r in 1 2 3; do
      run_bench "$PROTO-t$t" $t $r
    done
  done
  shutdown_guest
  if [ "$PROTO" = "nvmeof" ]; then teardown_nvmeof; else teardown_iscsi; fi
fi

del_netem
echo "MACRO_RTT_${PROTO}_OK [$(date +%T)]"
