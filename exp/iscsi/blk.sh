qemu-system-x86_64 \
 -enable-kvm -cpu host -smp 4 -m 4096 \
 -drive file=/home/waiai/SvmExp/local/system.raw,if=none,id=drive0,format=raw \
 -device virtio-blk-pci,drive=drive0,bootindex=1 \
 -drive file=/dev/sda,if=none,id=drive1,format=raw \
 -device virtio-blk-pci,drive=drive1 \
 -netdev user,id=net0,hostfwd=tcp::2222-:22 \
 -device virtio-net-pci,netdev=net0 \
 -nographic
