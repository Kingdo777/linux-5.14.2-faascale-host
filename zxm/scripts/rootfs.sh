#!/bin/bash

KERNEL_DIR=/home/kingdo/CLionProjects/linux_kernel_5_10
CORE_COUNT=$(nproc)

ARCH=x86_64
Release=ubuntu
Version=20

rootfs_image=$KERNEL_DIR/zxm/rootfs/rootfs_${Release}-${Version}_${ARCH}.ext4
rootfs_path=$KERNEL_DIR/zxm/rootfs/rootfs_${Release}-${Version}_${ARCH}

update_rootfs() {
  if [ ! -f "$rootfs_image" ]; then
    echo "rootfs image is not present..., pls run build_rootfs"
  else
    echo "update rootfs ..."

    mkdir -p "$rootfs_path"
    echo "mount ext4 image $rootfs_image into $rootfs_path"
    mount -t ext4 "$rootfs_image" "$rootfs_path" -o loop
    echo "install kernel, modules and headers"
    cd $KERNEL_DIR && make install INSTALL_PATH="${rootfs_path}/boot" -j "${CORE_COUNT}"
    cd $KERNEL_DIR && make modules_install INSTALL_MOD_PATH="${rootfs_path}" -j "${CORE_COUNT}"
    cd $KERNEL_DIR && make headers_install INSTALL_HDR_PATH="${rootfs_path}/usr" -j "${CORE_COUNT}"
    sleep 5s

    umount "$rootfs_path"
    rm -rf "$rootfs_path"
  fi

}

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

check_root() {
  if [ "$(id -u)" != "0" ]; then
    echo "superuser privileges are required to run"
    echo "sudo $0 build_rootfs"
    exit 1
  fi
}

check_img() {
  if [ ! -f "$rootfs_image" ]; then
    echo "can't find rootfs image: $rootfs_image"
    exit 1
  fi
}

check_no_img() {
  if [ -f "$rootfs_image" ]; then
    echo "can't find rootfs image: $rootfs_image"
    exit 1
  fi
}

check_root

case $1 in
create)
  check_no_img ;;
update)
  check_img
  update_rootfs ;;
esac


