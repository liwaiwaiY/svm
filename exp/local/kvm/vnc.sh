#!/bin/bash

# start minimized local_qemu
# ssh: 0.0.0.0 with port 2222 on host to vm port 22

qemu-system-x86_64 \
-enable-kvm -cpu host -smp 4 -m 4096 \
-drive file=~/SvmExp/local/system.raw,if=none,id=drive0,format=raw \
-device virtio-blk-pci,drive=drive0 \
-netdev user,id=net0,hostfwd=tcp::2222-:22 \
-device virtio-net-pci,netdev=net0 \
-vnc :0 -serial mon:stdio \
-nographic
