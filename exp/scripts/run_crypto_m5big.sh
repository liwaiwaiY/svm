#!/bin/bash
# run_crypto_m5big.sh - M5 big-block followup: does buf_pool gain grow with block size?
#   aes_bench (AF_ALG cbc(aes)) blocks {32K,128K,512K,1M} x2, P=off (tier0) vs P=on (tier9).
#   Output -> exp/results/crypto/aesbench/{off,on}/
set -u
H=/home/waiai/svm
OUT=$H/exp/results/crypto/aesbench
mkdir -p $OUT/off $OUT/on
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
SCP="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh scp -P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

run_blocks() {  # $1=off|on  $2=tier
  echo "### [$(date +%T)] P=$1 tier $2: switch"
  bash $H/exp/host_switch_crypto.sh $2 || { echo SWITCH_FAIL; exit 1; }
  echo "### deploy aes_bench"
  env $SCP $H/exp/aes_bench.c wai@127.0.0.1:/home/wai/ >/dev/null 2>&1
  env $SSH "gcc -O2 -o /home/wai/aes_bench /home/wai/aes_bench.c -lpthread" 2>&1 || { echo GCC_FAIL; exit 1; }
  # ensure aesni is unloaded so cbc(aes) resolves to virtio_crypto (priority 150)
  env $SSH "echo wai | sudo -S modprobe -r aesni_intel 2>/dev/null; lsmod | grep -c aesni" 2>/dev/null
  echo "### [$(date +%T)] run blocks"
  # block -> iters (aim ~1-3s per run at SVM crypto speed)
  declare -A IT=( [32768]=2000 [131072]=500 [524288]=200 [1048576]=100 )
  for bs in 32768 131072 524288 1048576; do
    for r in 1 2; do
      env $SSH "/home/wai/aes_bench $bs ${IT[$bs]} 1 20" 2>/dev/null > $OUT/$1/bs${bs}-r$r.log
      grep -q '^RESULT' $OUT/$1/bs${bs}-r$r.log && \
        echo "  $1 bs=$bs r$r: $(grep '^RESULT' $OUT/$1/bs${bs}-r$r.log)" || \
        echo "  $1 bs=$bs r$r: PARSE FAIL"
    done
  done
}

run_blocks off 0
run_blocks on 9

# summary table
echo ""
echo "=== SUMMARY (MBps, mean of 2) ==="
for bs in 32768 131072 524288 1048576; do
  off=$(grep -h '^RESULT' $OUT/off/bs${bs}-r*.log | awk '{for(i=1;i<=NF;i++){if($i~/MBps=/){split($i,a,"="); s+=a[2]; n++}}} END{if(n)printf "%.2f", s/n; else print "NA"}')
  on=$(grep -h '^RESULT' $OUT/on/bs${bs}-r*.log | awk '{for(i=1;i<=NF;i++){if($i~/MBps=/){split($i,a,"="); s+=a[2]; n++}}} END{if(n)printf "%.2f", s/n; else print "NA"}')
  echo "  bs=$bs: off=$off on=$on"
done

# restore tier 0 conf
bash $H/exp/host_switch_crypto.sh 0 >/dev/null 2>&1 || true
echo "M5BIG_OK [$(date +%T)]"
