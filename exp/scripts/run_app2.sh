#!/bin/bash
# run_app2.sh <xgb|als> <kvm|svm|nvmeof|iscsi>
# IO 密集应用四协议对比（数据盘大文件流式加载，超 guest 4GB 内存）。
#   xgb : xgboost 训练 HIGGS.csv（7.5GB，数据超内存）
#   als : implicit ALS 训练 ratings_big.csv（4.5GB，全量扫描+采样训练）
# 协议后端与 run_app_rtt.sh 相同。默认无 RTT；DELAY_US>0 时环境就绪后注入 lo netem。
# cores {1,2,4} x 1 run，每轮 drop_caches，优雅关机。
# Output -> exp/results/app2/$APP-$PROTO/
set -u
APP=$1
PROTO=$2
DELAY_US=${DELAY_US:-0}
H=/home/waiai/svm
CONF=$H/local_qemu/hw/virtio-remote/vr.conf
OUT=${OUT_DIR:-$H/exp/results/app2/$APP-$PROTO}
mkdir -p $OUT/logs
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
CS=(1 2 4)
DEV=/dev/nvme1n1
[ "$PROTO" = "iscsi" ] && DEV=/dev/sda
NVMET_NQN=nqn.2026-08.svm:data
ISCSI_IQN=iqn.2026-08.com.svm:data
ISCSI_INIT=iqn.2026-08.com.svm:init
S=/sys/kernel/config/nvmet
DATA=${APP_DATA:-/mnt/data/$( [ "$APP" = "xgb" ] && echo HIGGS.csv || echo ratings_big.bin )}

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
train=re.search(r'train_s=([\d.]+)', s)
total=re.search(r'total_s=([\d.]+)', s)
print('  $2: links=%s build=%ss train=%ss total=%ss' % (
  links.group(1) if links else '?',
  build.group(1) if build else '?',
  train.group(1) if train else '?',
  total.group(1) if total else '?'))
" 2>/dev/null || echo "  $2: PARSE FAIL"
}

shutdown_guest() {
  if env $SSH "true" 2>/dev/null; then
    timeout 90 env $SSH "echo wai | sudo -S bash -c 'sync; poweroff'" 2>/dev/null
    sleep 20
  fi
  sudoc "pkill -9 -f 'qemu-syste[m]' 2>/dev/null; pkill -9 -f 'remote-[s]tub' 2>/dev/null"; sleep 3
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
  env $SSH "echo wai | sudo -S sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'" 2>/dev/null
  # SVM 远程盘(数据盘)写回风暴会卡死 stub 队列（ALS-BIN 已证），故 build 的 npy scratch
  # 放本地系统盘 /home/wai；guest / 只剩 ~520M，先 vacuum journal 腾 ~700M（X.npy 672M+y.npy 24M）
  env $SSH "echo wai | sudo -S journalctl --vacuum-size=100M >/dev/null 2>&1; df -h / | tail -1" 2>/dev/null
  # 部署训练脚本到 guest（幂等覆盖）；APP_SCRIPT 可覆盖默认（als → als_train_bin.py）
  local SCRIPT=${APP_SCRIPT:-$( [ "$APP" = "als" ] && echo als_train_bin.py || echo ${APP}_train.py )}
  env DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh scp -q -o StrictHostKeyChecking=no -P 2222 $H/exp/$SCRIPT wai@127.0.0.1:/home/wai/ 2>/dev/null
  [ "$APP" = "xgb" ] && env DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh scp -q -o StrictHostKeyChecking=no -P 2222 $H/exp/xgb_parse_seg.py wai@127.0.0.1:/home/wai/ 2>/dev/null
  env $SSH "cd /home/wai && timeout 3600 python3 $SCRIPT $DATA $2 ${ITERS:-20}" \
    > $OUT/logs/$PROTO-c$2-r1.log 2>&1
  parse $OUT/logs/$PROTO-c$2-r1.log "$PROTO c$2 r1"
}

echo "=== [$(date +%T)] APP2 $APP $PROTO (data=$DATA, delay=${DELAY_US}us): start ==="

if [ "$PROTO" = "kvm" ]; then
  sudoc "pkill -f 'qemu-syste[m]' 2>/dev/null; pkill -f 'remote-[s]tub' 2>/dev/null"; sleep 2
  sudoc "rm -f $H/guest.log /tmp/app2-guest.log /tmp/app2-out.log"
  setsid $H/local_qemu/build/qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 4 -m 4096 \
    -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive0,bootindex=1 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -drive file=$H/exp/remote/fio.raw,if=none,id=drive1,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive1,serial=data-disk \
    -serial file:$H/guest.log -monitor none -display none \
    -qmp unix:/tmp/qmp.sock,server,nowait \
    > /tmp/app2-out.log 2>&1 < /dev/null &
  wait_ssh
  mount_data
  [ $DELAY_US -gt 0 ] && add_netem
  env $SSH "echo wai | sudo -S dd if=/dev/vdb of=/dev/null bs=1M count=128 2>&1 | tail -1" 2>/dev/null
  for c in "${CS[@]}"; do run_app "kvm" $c; done
  shutdown_guest
elif [ "$PROTO" = "svm" ]; then
  cat > $CONF <<EOF
# SVM App2: W=4 I=256 B=16 (run_app2.sh generated)
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
  [ $DELAY_US -gt 0 ] && add_netem
  env $SSH "echo wai | sudo -S dd if=/dev/vdb of=/dev/null bs=1M count=128 2>&1 | tail -1" 2>/dev/null
  for c in "${CS[@]}"; do run_app "svm" $c; done
  shutdown_guest
  cat > $CONF <<EOF
# SVM 参数档 0: Z=1048576 W=4 I=32 B=16 P=off (restored by run_app2.sh)
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
  sudoc "pkill -f 'qemu-syste[m]' 2>/dev/null; pkill -f 'remote-[s]tub' 2>/dev/null"; sleep 2
  # qemu 需 sudo：直接访问 /dev/nvme1n1（或 /dev/sda）块设备，TRAE 沙箱拦截非特权访问
  # 注意 < /dev/null 必须在 bash -c 外部：字符串内部的 < /dev/null 会覆盖管道，sudo 读不到密码
  # serial 写到 $H/guest.log 而非 /tmp：sudo qemu 打开 /tmp 残留文件会被沙箱拦（Permission denied）
  sudoc "rm -f $H/guest.log /tmp/app2-out.log"
  setsid bash -c "echo dxeqqghk | sudo -S $H/local_qemu/build/qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 4 -m 4096 \
    -drive file=$H/exp/local/system.raw,if=none,id=drive0,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive0,bootindex=1 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0 \
    -drive file=$DEV,if=none,id=drive1,format=raw,cache=none \
    -device virtio-blk-pci,drive=drive1,serial=data-disk \
    -serial file:$H/guest.log -monitor none -display none \
    -qmp unix:/tmp/qmp.sock,server,nowait \
    > /tmp/app2-out.log 2>&1" < /dev/null &
  wait_ssh
  mount_data
  [ $DELAY_US -gt 0 ] && add_netem
  env $SSH "echo wai | sudo -S dd if=/dev/vdb of=/dev/null bs=1M count=128 2>&1 | tail -1" 2>/dev/null
  for c in "${CS[@]}"; do run_app "$PROTO" $c; done
  shutdown_guest
  if [ "$PROTO" = "nvmeof" ]; then teardown_nvmeof; else teardown_iscsi; fi
fi

del_netem
echo "APP2_${APP}_${PROTO}_OK [$(date +%T)]"
