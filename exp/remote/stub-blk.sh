#!/bin/bash

# start minimized remote_stub

remote-stub \
-smp 4 \
-drive file=~/svm/exp/local/kvm/data.raw,if=none,id=drive1,format=raw,cache=none \
-device virtio-blk-pci,drive=drive1,serial=data-disk,remote-stub=127.0.0.1@5552 \
-nographic
