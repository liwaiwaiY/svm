#!/bin/bash
# run_m1mem.sh - M1 memory-usage sweep on blk/null_blk.
#   Cells: SVM zc{0,4M} x bs{64K,512K,1M,2M}  +  KVM same bs.
#   P=off, I=256, W=4, B=16. fio randrw 7:3, iodepth=1, runtime=30.
#   Per cell: single boot, one fio, 60 mem samples @0.5s (RSS + skmem).
#   Results -> exp/results/m1mem/<tag>/samples.txt + bs.json
#   FORCE=1 re-runs cells that already have samples.
set -u
H=/home/waiai/svm
OUT=$H/exp/results/m1mem
RUNTIME=30
IODEPTH=1
mkdir -p $OUT
SSH="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2222 wai@127.0.0.1"
SCP="DISPLAY=:0 SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=/tmp/askpass.sh scp -P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

pids() { # $1=mode -> sets local_pid, stub_pid
  local mode=$1
  local_pid= stub_pid=
  if [ "$mode" = "kvm" ]; then
    local_pid=$(pgrep -f 'qemu-system-x86_64' | head -1)
  else
    local_pid=$(pgrep -f 'remote-machine=127.0.0.1@5552' | head -1)
    stub_pid=$(pgrep -f 'remote-stub' | head -1)
  fi
}

rss() { # $1=pid -> kB
  [ -n "$1" ] && awk '/VmRSS/{print $2}' /proc/$1/status 2>/dev/null || echo 0
}

skmem_total() { # $@ = pids; sum rmem_alloc(1) + wmem_queued(6) over their TCP sockets
  # ss -m skmem:(r<rmem>,rb<rcvbuf>,t,tb,f,w<wmem_queued>,o,...) -> fields 1,6
  # (wmem_queued == Send-Q; rmem_alloc == Recv-Q). skmem line is AFTER the
  # ESTAB line -> grep -A1 is required.
  local tot=0 pid inner rv wv
  for pid in "$@"; do
    [ -z "$pid" ] && continue
    while read -r line; do
      case "$line" in
        *skmem:*)
          inner=${line#*skmem:(}; inner=${inner%)}
          rv=$(echo "$inner" | cut -d, -f1); rv=${rv#r}
          wv=$(echo "$inner" | cut -d, -f6); wv=${wv#w}
          tot=$((tot + rv + wv)) ;;
      esac
    done < <(ss -m -tnp 2>/dev/null | grep -A1 "pid=$pid")
  done
  echo $tot
}

# sample 60 points @0.5s (30s window) after BS_START appears
sample_cell() { # $1=outfile $2=mode
  local f=$1 mode=$2 waited=0 samples=0
  : > $f
  # wait for BS_START (fio about to run)
  while [ $waited -lt 60 ]; do
    env $SSH "grep -q BS_START /tmp/m15/progress.log" 2>/dev/null && break
    sleep 0.5; waited=$((waited+1))
  done
  pids $mode
  while [ $samples -lt 60 ]; do
    echo "$(date +%s.%N) $(rss $local_pid) $(rss $stub_pid) $(skmem_total $local_pid $stub_pid)" >> $f
    sleep 0.5; samples=$((samples+1))
  done
  awk '{a+=$2;b+=$3;c+=$4; if($2>m2)m2=$2; if($3>m3)m3=$3; if($4>m4)m4=$4; n++}
       END{printf "# local_rss kB: max=%d mean=%.0f | stub_rss kB: max=%d mean=%.0f | skmem B: max=%d mean=%.0f | samples=%d\n",
           m2,a/n,m3,b/n,m4,c/n,n}' $f >> $f
}

run_cell() { # $1=tag $2=mode(svm|kvm) $3=Z $4=bs
  local tag=$1 mode=$2 Z=$3 bs=$4
  echo "### [$(date +%T)] $tag: mode=$mode Z=$Z bs=$bs"
  if [ -z "${FORCE:-}" ] && [ -s $OUT/$tag.samples ] && [ "$(grep -vc '^#' $OUT/$tag.samples)" -ge 60 ]; then
    echo "  SKIP $tag (samples already present)"; return
  fi
  if [ "$mode" = "kvm" ]; then
    bash $H/exp/host_switch_kvm_null.sh >/dev/null 2>&1
  else
    bash $H/exp/host_switch_blk_null.sh $Z 256 0 >/dev/null 2>&1
  fi
  if ! env $SSH "true" 2>/dev/null; then echo "  SWITCH/SSH FAIL $tag"; exit 1; fi
  # re-deploy + verify every cell (guest /tmp is tmpfs, wiped per reboot)
  env $SCP $H/exp/guest_m1mem.sh wai@127.0.0.1:/home/wai/m1mem.sh 2>/dev/null
  if ! env $SSH "grep -q 'mkdir -p /tmp/m15' /home/wai/m1mem.sh" 2>/dev/null; then
    echo "  DEPLOY FAIL (verify failed)"; exit 1
  fi
  echo "  guest runner deployed"
  pids $mode
  { echo "# baseline (idle): local_rss_kB=$(rss $local_pid) stub_rss_kB=$(rss $stub_pid) skmem_B=$(skmem_total $local_pid $stub_pid)"; } > $OUT/$tag.baseline
  if [ "$mode" = "kvm" ]; then
    env $SSH "echo wai | sudo -S sh -c 'echo 3 > /proc/sys/vm/drop_caches'" >/dev/null 2>&1
  fi
  env $SSH "echo wai | sudo -S sh -c 'rm -f /tmp/m15/progress.log; nohup bash /home/wai/m1mem.sh $bs $RUNTIME $IODEPTH >/dev/null 2>&1 & echo OK'" 2>/dev/null | grep -q OK || { echo "  LAUNCH FAIL"; exit 1; }
  sample_cell $OUT/$tag.samples $mode
  # wait ALL_DONE
  local waited=0 done=0
  while [ $waited -lt 90 ]; do
    if env $SSH "grep -q ALL_DONE /tmp/m15/progress.log" 2>/dev/null; then done=1; break; fi
    sleep 2; waited=$((waited+2))
  done
  [ $done -eq 1 ] || echo "  WARN: ALL_DONE not seen (fio may not have run)"
  env $SCP wai@127.0.0.1:/tmp/m15/bs.json $OUT/$tag.bs.json 2>/dev/null
  if [ ! -s $OUT/$tag.bs.json ]; then echo "  WARN: bs.json empty/missing"; fi
}

# SVM (P=off, I=256): zc {0, 4M} x bs {64K, 512K, 1M, 2M}
run_cell svm_z0_64k   svm 0        64k
run_cell svm_z0_512k  svm 0        512k
run_cell svm_z0_1m    svm 0        1m
run_cell svm_z0_2m    svm 0        2m
run_cell svm_z4m_64k  svm 4194304  64k
run_cell svm_z4m_512k svm 4194304  512k
run_cell svm_z4m_1m   svm 4194304  1m
run_cell svm_z4m_2m   svm 4194304  2m
# KVM baseline
run_cell kvm_64k      kvm 0        64k
run_cell kvm_512k     kvm 0        512k
run_cell kvm_1m       kvm 0        1m
run_cell kvm_2m       kvm 0        2m

# long-format CSV for boxplots: tag,bs,mode,local_kB,stub_kB,skmem_B
CSV=$OUT/points.csv
echo "tag,bs,mode,local_rss_kB,stub_rss_kB,skmem_B" > $CSV
for tag in svm_z0_64k svm_z0_512k svm_z0_1m svm_z0_2m svm_z4m_64k svm_z4m_512k svm_z4m_1m svm_z4m_2m kvm_64k kvm_512k kvm_1m kvm_2m; do
  bs=$(echo $tag | sed 's/.*_//'); mode=$([ "${tag#kvm}" != "$tag" ] && echo kvm || echo svm)
  while read -r ts l s k; do
    case "$ts" in \#*) continue;; esac
    echo "$tag,$bs,$mode,$l,$s,$k" >> $CSV
  done < $OUT/$tag.samples
done

echo ""
echo "=== M1MEM SUMMARY ==="
for tag in svm_z0_64k svm_z0_512k svm_z0_1m svm_z0_2m svm_z4m_64k svm_z4m_512k svm_z4m_1m svm_z4m_2m kvm_64k kvm_512k kvm_1m kvm_2m; do
  echo "== $tag =="
  cat $OUT/$tag.baseline
  grep '^#' $OUT/$tag.samples
  r=$(grep -o '"iops": [0-9.]*' $OUT/$tag.bs.json 2>/dev/null | head -1)
  [ -n "$r" ] && echo "  $r"
done
echo "points -> $CSV"
echo "M1MEM_OK [$(date +%T)]"
