#!/bin/bash
# host_switch_crypto.sh <tier>
#   Switches to the crypto SVM environment (virtio-crypto over remote stub,
#   port 5553). Writes vr.conf, restarts stub + local qemu, waits for SSH.
#   Tiers follow the experiment matrix doc (2026-08-19):
#     M1 zc threshold -> tier 0/1/2/3 (Z varies: 4K/0/64K/512K)
#     M5 buf pool     -> tier 0/9 (P off/on)
#   All other params fixed: W=4 I=32 B=16.
set -u
TIER=$1
CONF=/home/waiai/svm/local_qemu/hw/virtio-remote/vr.conf
ulimit -c unlimited

case $TIER in
  0) Z=4096;    W=4; I=32; B=16; P=0 ;;
  1) Z=0;       W=4; I=32; B=16; P=0 ;;
  2) Z=65536;   W=4; I=32; B=16; P=0 ;;
  3) Z=524288;  W=4; I=32; B=16; P=0 ;;
  9) Z=4096;    W=4; I=32; B=16; P=1 ;;
  *) echo "bad tier $TIER"; exit 1 ;;
esac

cat > $CONF <<EOF
# SVM crypto 参数档 $TIER: Z=${Z} W=${W} I=${I} B=${B} P=${P} (host_switch_crypto.sh)
buf_pool=$P
inflight_size=$I
local_batch_n=$B
stub_batch_n=$B
zc_send_min=$Z
workers=$W
EOF
echo "vr.conf -> crypto tier $TIER: Z=$Z W=$W I=$I B=$B P=$P"

pkill -f "remote-stub" 2>/dev/null
pkill -f "qemu-system-x86_64" 2>/dev/null
sleep 2

# stub (crypto device only, port 5553)
setsid remote-stub \
  -smp 4 \
  -object cryptodev-backend-builtin,id=cryptodev0 \
  -device virtio-crypto-pci,cryptodev=cryptodev0,remote-stub=127.0.0.1@5553 \
  -display none -monitor none -serial file:/tmp/stub-crypto.log \
  > /tmp/stub-crypto-out.log 2>&1 < /dev/null &
echo "crypto stub started (5553)"

# local qemu (system disk + crypto device, ssh 2222)
setsid /home/waiai/svm/local_qemu/build/qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 4096 \
  -drive file=/home/waiai/svm/exp/local/system.raw,if=none,id=drive0,format=raw \
  -device virtio-blk-pci,drive=drive0 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -object cryptodev-backend-builtin,id=cryptodev0 \
  -device virtio-crypto-pci,cryptodev=cryptodev0,remote-machine=127.0.0.1@5553 \
  -serial file:guest.log -monitor none -display none \
  -qmp unix:/tmp/qmp.sock,server,nowait \
  > /tmp/local-crypto-out.log 2>&1 < /dev/null &
echo "local qemu (crypto) started"

for i in $(seq 1 60); do
  if DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh \
     ssh -o StrictHostKeyChecking=no -o ConnectTimeout=3 -o BatchMode=no \
     -p 2222 wai@127.0.0.1 "true" 2>/dev/null; then
    echo "guest SSH up after ${i} tries"
    exit 0
  fi
  sleep 3
done
echo "ERROR: guest SSH did not come up"
exit 1
