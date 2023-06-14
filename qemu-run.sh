#!/bin/bash

CURRENT_DIR=$PWD
CORE_COUNT=$(nproc)

ARCH=x86_64
Release=ubuntu
Version=22

kernel_build=$CURRENT_DIR/rootfs_${Release}-${Version}_${ARCH}/usr/src/linux/
rootfs_path=$CURRENT_DIR/rootfs_${Release}-${Version}_${ARCH}
rootfs_image=$CURRENT_DIR/ROOTFS-IMG/rootfs_${Release}-${Version}_${ARCH}.ext4

if [ $# -ne 1 ]; then
  echo "Usage: $0 [arg]"
  echo "build: build the kernel image."
  echo "update_rootfs: update kernel modules for rootfs image, need root privilege."
  echo "run: run ubuntu system, need root privilege."
  echo "debug: enable gdb debug server."
fi

check_root() {
  if [ "$(id -u)" != "0" ]; then
    echo "superuser privileges are required to run"
    echo "sudo $0 build_rootfs"
    exit 1
  fi
}

check_img() {
  if [ ! -f "$CURRENT_DIR"/arch/x86/boot/bzImage ]; then
    echo "can't find kernel image, pls run build command firstly!!"
    exit 1
  fi

  if [ ! -f "$rootfs_image" ]; then
    echo "can't find rootfs image : $rootfs_image"
    exit 1
  fi
}

make_kernel_image() {
  echo "start build kernel image..."
  ./scripts/config -e DEBUG_INFO \
    -e GDB_SCRIPTS \
    -e CONFIG_DEBUG_SECTION_MISMATCH \
    -d CONFIG_RANDOMIZE_BASE \
    -e CONFIG_VIRTIO_BLK
  make -j "$CORE_COUNT"
}

update_rootfs() {
  if [ ! -f "$rootfs_image" ]; then
    echo "rootfs image is not present..., pls run build_rootfs"
  else
    echo "update rootfs ..."

    mkdir -p "$rootfs_path"
    echo "mount ext4 image $rootfs_path into $rootfs_path"
    mount -t ext4 "$rootfs_image" "$rootfs_path" -o loop

    echo "install kernel, modules and headers"
    export INSTALL_PATH=$CURRENT_DIR/${rootfs_path}/boot/
    export INSTALL_MOD_PATH=$CURRENT_DIR/${rootfs_path}/
    export INSTALL_HDR_PATH=$CURRENT_DIR/${rootfs_path}/usr/
    make install
    make modules_install -j "$CORE_COUNT"
    make headers_install

    umount "$rootfs_path"
    rm -rf "$rootfs_path"
  fi

}

# add fellow content to /home/kingdo/CLionProjects/qemu_6_1_0/etc/qemu-ifup
##!/bin/sh
#set -x
#if [ -n "$1" ];then
#    ip tuntap add $1 mode tap user `whoami`
#    ip addr add 172.16.0.1/24 dev $1
#    ip link set $1 up
#    iptables -t nat -A POSTROUTING -o ens8f0 -j MASQUERADE
#    iptables -A FORWARD -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
#    iptables -A FORWARD -i tap0 -o ens8f0 -j ACCEPT
#    iptables -A FORWARD -i ens8f0 -j ACCEPT
#    iptables -A PREROUTING -i ens8f0 -p tcp --dport 8080 -j DNAT --to-destination 172.16.0.2
#    sleep 0.5s
#    exit 0
#else
#    echo "Error: no interface specified"
#    exit 1
#fi

# add fellow content to /home/kingdo/CLionProjects/qemu_6_1_0/etc/qemu-ifdown
##!/bin/sh
#set -x
#if [ -n "$1" ];then
#    ip link del tap0
#    sleep 0.5s
#    exit 0
#else
#    echo "Error: no interface specified"
#    exit 1
#fi

run_qemu() {
  # How to share Dir: https://www.linux-kvm.org/page/9p_virtio
  /home/kingdo/CLionProjects/qemu/build/qemu-system-x86_64 \
    -nographic -kernel arch/x86/boot/bzImage \
    -append "noinintrd console=ttyS0 root=/dev/vda rootfstype=ext4 rw loglevel=8 nokaslr" \
    -device virtio-balloon \
    -drive if=none,file=rootfs.ext4,id=hd0,format=raw -device virtio-blk-pci,drive=hd0 \
    -nic tap,ifname=tap0,model=virtio-net-pci \
    $QEMU_ARGS

  #  -fsdev local,security_model=passthrough,id=fsdev0,path=/home/kingdo/GolandProjects -device virtio-9p-pci,id=fs0,fsdev=fsdev0,mount_tag=go_project \
  #  -fsdev local,security_model=passthrough,id=fsdev1,path=/home/kingdo/GolandProjects -device virtio-9p-pci,id=fs1,fsdev=fsdev1,mount_tag=python_project \
  #  -nic tap,ifname=tap0,model=virtio-net-pci \
  #  -nic user,hostfwd=tcp::8080-:8080 \

  # -nic user,model=virtio-net-pci \
  # -nic user,hostfwd=tcp::8080-:8080 \
  # hostfwd=[tcp|udp]:[hostaddr]:hostport-[guestaddr]:guestport
}

case $1 in
build)
  make_kernel_image
  ;;

update_rootfs)
  check_img
  check_root
  update_rootfs
  ;;

run)
  check_img
  check_root
  #    QEMU_ARGS="-m 8G -smp 4 --enable-kvm -cpu host"

  # Use qmp.sock
#    QEMU_ARGS=$(
#      cat <<EOF
#        -qmp unix:qmp.sock,server=on,wait=off
#        -m 8G -smp 4 --enable-kvm -cpu host
#EOF
#    )

  # Enable virtio-mem
#  QEMU_ARGS=$(
#    cat <<EOF
#        -m 256M,maxmem=8448M
#        -smp 4 --enable-kvm -cpu host
#        -device virtio-balloon \
#        -object memory-backend-ram,id=vmem0,size=8G,prealloc=off
#        -device virtio-mem-pci,id=vm0,memdev=vmem0,node=0,requested-size=0,block-size=4M,prealloc=on
#        -qmp unix:qmp.sock,server=on,wait=off
#EOF
#  )
  # NUMA
    QEMU_ARGS=$(
      cat <<EOF
          -m 4160M,maxmem=12352M
          -smp sockets=2,cores=2
          --enable-kvm -cpu host
          -object memory-backend-ram,id=mem0,size=64M
          -object memory-backend-ram,id=mem1,size=4G
          -numa node,nodeid=0,cpus=0-1,memdev=mem0
          -numa node,nodeid=1,cpus=2-3,memdev=mem1
          -object memory-backend-ram,id=vmem0,size=8G,prealloc=off
          -device virtio-mem-pci,id=vm0,memdev=vmem0,node=0,requested-size=4M,prealloc=off
          -qmp unix:qmp.sock,server=on,wait=off
EOF
    )
  run_qemu
  ;;

debug)
  check_img
  check_root
  echo "Enable qemu debug server"
  QEMU_ARGS="-m 1024 -s -S"
  run_qemu
  ;;
esac

#sudo /home/kingdo/CLionProjects/qemu_6_1_0/build/qemu-system-x86_64 \
#  -kernel arch/x86_64/boot/bzImage \
#  -nographic \
#  -append "console=ttyS0 nokaslr" \
#  -device virtio-balloon \
#  -initrd ramdisk.img \
#  -m 8192 \
#  -smp 4  \
#  --enable-kvm \
#  -cpu host

create_img() {
  if [ ! -f "$rootfs_image" ]; then
    echo "making image..."
    dd if=/dev/zero of="$rootfs_image" bs=1G count=50
    mkfs.ext4 "$rootfs_image"
    mkdir -p tmpfs
    echo "copy data into rootfs..."
    mount -t ext4 "$rootfs_image" tmpfs/ -o loop
    cp -af "$rootfs_path"/* tmpfs/
    umount tmpfs
  fi
}
