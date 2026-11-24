#!/bin/bash

# start minimized remote_stub
# REMOTE_IP: address the local qemu connects to (stub binds INADDR_ANY)
# DATA_IMG:  data image on THIS machine

REMOTE_IP="${REMOTE_IP:-127.0.0.1}"
DATA_IMG="${DATA_IMG:-$HOME/svm/exp/local/kvm/data.raw}"

remote-stub \
-smp 4 \
-drive file="$DATA_IMG",if=none,id=drive1,format=raw,cache=none \
-device virtio-blk-pci,drive=drive1,serial=data-disk,remote-stub="${REMOTE_IP}@5552" \
-nographic
