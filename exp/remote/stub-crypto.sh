#!/bin/bash

# start minimized remote_stub

remote-stub \
-smp 4 \
-object cryptodev-backend-builtin,id=cryptodev0 \
-device virtio-crypto-pci,cryptodev=cryptodev0,remote-stub=127.0.0.1@5553 \
-nographic

# -object cryptodev-backend-builtin,id=cryptodev0 -device virtio-crypto-pci,cryptodev=cryptodev0,remote-stub=5553 -nographic
