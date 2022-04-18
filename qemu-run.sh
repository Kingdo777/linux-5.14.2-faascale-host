make -j6 vmlinux bzImage

qemu-system-x86_64 \
  -kernel arch/x86_64/boot/bzImage \
  -nographic \
  -append "console=ttyS0 nokaslr" \
  -device virtio-balloon \
  -initrd ramdisk.img \
  -m 1024 \
  --enable-kvm \
  -cpu host
