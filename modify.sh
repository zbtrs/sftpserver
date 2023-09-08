#!/bin/bash

source env.sh
make
cp gesftpserver ~/ospp/userapps/apps/build/rootfs/usr/libexec/sftp-server
cp gesftpserver ~/ospp/userapps/prebuilt/qemu-virt64-riscv/sftp-server
cd ~/ospp/userapps
source env.sh
cd apps
xmake smart-image -o ../prebuilt/qemu-virt64-riscv/fat32.img -f fat -s 512M -v
