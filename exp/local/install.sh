qemu-system-x86_64 \
-enable-kvm -cpu host -smp 4 -m 4096 \
-drive file=/home/$USER/svm/exp/local/system.raw,if=none,id=drive0,format=raw \
-device virtio-blk-pci,drive=drive0 \
-netdev user,id=net0 \
-device virtio-net-pci,netdev=net0 \
-cdrom /home/$USER/svm/exp/local/ubuntu-26.04-live-server-amd64.iso \
-boot order=d \
-vnc :0 -serial mon:stdio

