#!/bin/bash
# crypto_bench.sh <tier> - M1/M5 crypto SVM benchmark (guest side)
#   Runs `openssl speed -engine afalg -elapsed aes-128-cbc` 3 times.
#   Prereq: aesni_intel unloaded so cbc(aes) resolves to virtio_crypto_aes_cbc.
#   Output: /home/wai/svm_exp_out/crypto-<tier>.log (with "ALL DONE" marker)
set -u
TIER=$1
OUT=/home/wai/svm_exp_out
mkdir -p "$OUT"
LOG=$OUT/crypto-$TIER.log
: > "$LOG"

echo "=== tier=$TIER $(date -Is) host=$(hostname) ===" >> "$LOG"
# sanity: aesni must stay unloaded, else aesni(prio 300) beats virtio(150)
if lsmod | grep -q aesni_intel; then
  echo "WARN: aesni_intel present, unloading" >> "$LOG"
  echo wai | sudo -S modprobe -r aesni_intel 2>> "$LOG"
fi
sleep 1
echo "--- cbc(aes) impls (top = highest priority, chosen by kernel) ---" >> "$LOG"
grep -A4 "cbc(aes)" /proc/crypto | grep -E "name|driver|priority" | head -8 >> "$LOG"
echo "--- lsmod virtio/aesni ---" >> "$LOG"
lsmod | grep -E "virtio_crypto|aesni" >> "$LOG"

for k in 1 2 3; do
  echo "=== RUN $k $(date +%T) ===" >> "$LOG"
  openssl speed -engine afalg -elapsed aes-128-cbc >> "$LOG" 2>&1
done
echo "ALL DONE tier=$TIER" >> "$LOG"
