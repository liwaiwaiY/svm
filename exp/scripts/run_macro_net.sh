#!/bin/bash
# run_macro_net.sh <nvmeof|iscsi>
# Macro (sysbench TPCC) on network-storage backends; guest data disk vdb = host initiator device.
#   nvmeof: nvmet (tcp:4420, nqn.2026-08.svm:data) -> loop(direct-io) -> fio.raw      -> /dev/nvme1n1
#   iscsi : LIO block (tcp:3260, iqn.2026-08.com.svm:data) -> loop(direct-io) -> data_iscsi.raw -> /dev/sda
#   threads {1,2,4} x 3 runs, 60s. cache=none + drop_caches per run. 20s warmup per boot.
#   iscsi first run: host-side prep data_iscsi.raw (mkfs.ext4 + copy MySQL datadir from fio.raw).
#   Graceful guest shutdown (stop mysql -> poweroff) to avoid InnoDB crash recovery.
#   Output -> exp/results/macronet/$PROTO/
set -u
PROTO=$1
H=/home/waiai/svm
OUT=$H/exp/results/macronet/$PROTO
mkdir -p $OUT/logs
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
TS=(1 2 4)
DEV=/dev/nvme1n1
[ "$PROTO" = "iscsi" ] && DEV=/dev/sda
NVMET_NQN=nqn.2026-08.svm:data
ISCSI_IQN=iqn.2026-08.com.svm:data
ISCSI_INIT=iqn.2026-08.com.svm:init
S=/sys/kernel/config/nvmet

sudoc() { echo dxeqqghk | sudo -S bash -c "$1"; }

wait_ssh() {
  for i in $(seq 1 90); do
    if env $SSH "true" 2>/dev/null; then echo "  SSH up after $i"; return 0; fi
    sleep 3
  done
  echo "  SSH FAILED"; exit 1
}
wait_mysql() {
  for i in $(seq 1 120); do
    if env $SSH "mysqladmin -u sbtest -psbtest ping 2>/dev/null | grep -q alive" 2>/dev/null; then
      echo "  MySQL up after $i"; return 0
    fi
    sleep 3
  done
  echo "  MySQL FAILED"; exit 1
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
prep_iscsi_data() {
  # free target/backstore first (loop held open by LIO), then detach loops
  sudoc "umount /tmp/m_prep 2>/dev/null; umount /tmp/m_src 2>/dev/null"
  sudoc "iscsiadm -m node -T $ISCSI_IQN -p 127.0.0.1:3260 -u 2>/dev/null"
  sudoc "targetcli /iscsi delete $ISCSI_IQN 2>/dev/null"
  sudoc "targetcli /backstores/block delete svmmacro 2>/dev/null"
  sudoc "losetup -d \$(losetup -j $H/exp/iscsi/data_iscsi.raw 2>/dev/null | cut -d: -f1) 2>/dev/null"
  sudoc "losetup -d \$(losetup -j $H/exp/remote/fio.raw 2>/dev/null | cut -d: -f1) 2>/dev/null"
  mkdir -p /tmp/m_prep /tmp/m_src
  sudoc "rm -rf /tmp/m_prep/* /tmp/m_src/*"
  # already populated?
  if sudoc "mount -o loop,ro $H/exp/iscsi/data_iscsi.raw /tmp/m_prep && [ -d /tmp/m_prep/mysql/mysql ]"; then
    echo "  iscsi data already present, skip prep"
    sudoc "umount /tmp/m_prep"
    return 0
  fi
  sudoc "umount /tmp/m_prep 2>/dev/null"
  echo "  [$(date +%T)] iscsi: mkfs + copy MySQL datadir from fio.raw ..."
  sudoc "mkfs.ext4 -F -q $H/exp/iscsi/data_iscsi.raw" || { echo "  MKFS FAIL"; exit 1; }
  sudoc "mount -o loop $H/exp/iscsi/data_iscsi.raw /tmp/m_prep"
  sudoc "mount -o loop,ro $H/exp/remote/fio.raw /tmp/m_src" || { echo "  SRC MOUNT FAIL"; exit 1; }
  # cp -a preserves numeric uid/gid (guest mysql ids) from the source ext4 image
  sudoc "mkdir -p /tmp/m_prep/mysql && cp -a /tmp/m_src/mysql/. /tmp/m_prep/mysql/" || { echo "  COPY FAIL"; exit 1; }
  sudoc "umount /tmp/m_prep /tmp/m_src"
  echo "  iscsi data prep done [$(date +%T)]"
}
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
  sudoc "tc qdisc del dev lo root 2>/dev/null"
  setsid $H/local_qemu/build/qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 4 -m 4096 \
    -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive0,bootindex=1 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0 \
    -drive file=$DEV,if=none,id=drive1,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive1,serial=data-disk \
    -serial file:/tmp/macro-guest.log -monitor none -display none \
    -qmp unix:/tmp/qmp.sock,server,nowait \
    > /tmp/macro-local-out.log 2>&1 < /dev/null &
  wait_ssh
}

ensure_mysql_data() {
  # idempotent: mount vdb if not mounted (iscsi new UUID vs fstab), start mysql.
  env $SSH "echo wai | sudo -S bash -c '
    if ! grep -q /mnt/data /proc/mounts; then
      mount /dev/vdb /mnt/data || exit 1
    fi
    systemctl start mysql
  '" 2>/dev/null
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

echo "=== [$(date +%T)] MACRO-NET $PROTO (dev=$DEV): start ==="
if [ "$PROTO" = "nvmeof" ]; then
  setup_nvmeof
else
  prep_iscsi_data
  setup_iscsi
fi
[ -e $DEV ] || { echo "DEVICE $DEV MISSING"; exit 1; }
boot_guest
# sanity: raw read 128M through the protocol path
env $SSH "echo wai | sudo -S dd if=/dev/vdb of=/dev/null bs=1M count=128 2>&1 | tail -1" 2>/dev/null
ensure_mysql_data
wait_mysql
echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=20 --threads=2 ./tpcc.lua run" > /dev/null 2>&1
echo "  warmup done, starting runs [$(date +%T)]"
for t in "${TS[@]}"; do
  for r in 1 2 3; do
    echo dxeqqghk | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null
    env $SSH "cd /home/wai/sysbench-tpcc && sysbench --db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=3306 --mysql-user=sbtest --mysql-password=sbtest --mysql-db=tpcc --tables=1 --scale=1 --time=60 --threads=$t ./tpcc.lua run" 2>/dev/null \
      > $OUT/logs/$PROTO-t$t-r$r.log
    parse $OUT/logs/$PROTO-t$t-r$r.log "$PROTO t$t r$r"
  done
done
env $SSH "echo wai | sudo -S bash -c 'systemctl stop mysql; sleep 2; poweroff'" 2>/dev/null
sleep 12
pkill -f 'qemu-syste[m]' 2>/dev/null
if [ "$PROTO" = "nvmeof" ]; then teardown_nvmeof; else teardown_iscsi; fi
echo "MACRO_NET_${PROTO}_OK [$(date +%T)]"
