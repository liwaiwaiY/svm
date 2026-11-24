#!/bin/bash

# start minimized remote_stub
# REMOTE_IP: address the local qemu connects to (stub binds INADDR_ANY)

REMOTE_IP="${REMOTE_IP:-127.0.0.1}"

remote-stub \
-smp 4 \
-object cryptodev-backend-builtin,id=cryptodev0 \
-device virtio-crypto-pci,cryptodev=cryptodev0,remote-stub="${REMOTE_IP}@5553" \
-nographic

# -object cryptodev-backend-builtin,id=cryptodev0 -device virtio-crypto-pci,cryptodev=cryptodev0,remote-stub=5553 -nographic
