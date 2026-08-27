#!/bin/bash
# run_m2g_rest.sh - runs the remaining M2G pipeline after tier 0 completes:
#   t5 -> anchor -> t4 -> anchor -> KVM x3 -> final anchor
set -u
cd /home/waiai/svm/exp
set -e
echo "=== [$(date +%T)] t5 (W=2) ==="
bash run_m2g.sh 5
bash host_anchor.sh t5a
echo "=== [$(date +%T)] t4 (W=1) ==="
bash run_m2g.sh 4
bash host_anchor.sh t4a
echo "=== [$(date +%T)] KVM 60s x3 ==="
bash host_anchor.sh kvm_before
bash run_kvm_m2g.sh
bash host_anchor.sh end
echo "=== M2G_ALL_OK [$(date +%T)] ==="
