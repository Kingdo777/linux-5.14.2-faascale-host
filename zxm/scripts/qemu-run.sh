#!/bin/bash

KERNEL_DIR=/home/kingdo/CLionProjects/linux_kernel_5_14_2
QEMU_DIR=/home/kingdo/CLionProjects/qemu

ARCH=x86_64
Release=ubuntu
Version=22

rootfs_image=$KERNEL_DIR/ROOTFS-IMG/rootfs_${Release}-${Version}_${ARCH}.ext4
kernel_image=$KERNEL_DIR/arch/x86/boot/bzImage
net_scripts=/home/kingdo/CLionProjects/linux_kernel_5_10/zxm/scripts/network

check_root() {
  if [ "$(id -u)" != "0" ]; then
    echo "superuser privileges are required to run"
    echo "sudo $0 build_rootfs"
    exit 1
  fi
}

check_img() {
  if [ ! -f "$kernel_image" ]; then
    echo "can't find kernel image: $kernel_image"
    exit 1
  fi

  if [ ! -f "$rootfs_image" ]; then
    echo "can't find rootfs image: $rootfs_image"
    exit 1
  fi
}

specific_qemu_args="-m 16G -smp 4 --enable-kvm -cpu host"

check_img
check_root

case $1 in
run) ;;

run_qmp_sock)
  specific_qemu_args=$(
    cat <<EOF
  -qmp unix:/tmp/qmp.sock,server=on,wait=off
  -m 8G -smp 4 --enable-kvm -cpu host
EOF
  )
  ;;
run_virtio_mem)
  specific_qemu_args=$(
    cat <<EOF
  -m 256M,maxmem=8448M
  -smp 4 --enable-kvm -cpu host
  -object memory-backend-ram,id=vmem0,size=8G,prealloc=off
  -device virtio-mem-pci,id=vm0,memdev=vmem0,node=0,requested-size=0,block-size=4M,prealloc=off
  -qmp unix:/tmp/qmp.sock,server=on,wait=off
EOF
  )
  ;;
run_numa)
  specific_qemu_args=$(
    cat <<EOF
  -m 4160M,maxmem=12352M
  -smp sockets=2,cores=2
  --enable-kvm -cpu host
  -object memory-backend-ram,id=mem0,size=64M
  -object memory-backend-ram,id=mem1,size=4G
  -numa node,nodeid=0,cpus=0-1,memdev=mem0
  -numa node,nodeid=1,cpus=2-3,memdev=mem1
  -object memory-backend-ram,id=vmem0,size=8G,prealloc=off
  -device virtio-mem-pci,id=vm0,memdev=vmem0,node=0,requested-size=4M,prealloc=on
  -qmp unix:/tmp/qmp.sock,server=on,wait=off
EOF
  )
  ;;
debug)
  specific_qemu_args="-m 16G -s -S"
  ;;

esac

set -x
$QEMU_DIR/build/qemu-system-x86_64 \
  -nographic -kernel $kernel_image \
  -append "noinintrd console=ttyS0 root=/dev/vda rw loglevel=8 nokaslr mminit_loglevel=7" \
  -device virtio-balloon \
  -drive if=none,file=$rootfs_image,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
  -nic tap,ifname=tap0,model=virtio-net-pci,script=$net_scripts/ifup.sh,downscript=$net_scripts/ifdown.sh \
  $specific_qemu_args

#  -append "noinintrd console=ttyS0 root=/dev/sda rw loglevel=8 nokaslr" \
#  -drive file=$rootfs_image,index=0,media=disk,format=raw \

# How to share Dir: https://www.linux-kvm.org/page/9p_virtio
#  -fsdev local,security_model=passthrough,id=fsdev0,path=/home/kingdo/GolandProjects -device virtio-9p-pci,id=fs0,fsdev=fsdev0,mount_tag=go_project \
#  -fsdev local,security_model=passthrough,id=fsdev1,path=/home/kingdo/GolandProjects -device virtio-9p-pci,id=fs1,fsdev=fsdev1,mount_tag=python_project \

# -nic user,model=virtio-net-pci \
# -nic user,hostfwd=tcp::8080-:8080 \
# hostfwd=[tcp|udp]:[hostaddr]:hostport-[guestaddr]:guestport