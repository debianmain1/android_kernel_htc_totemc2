#!/bin/bash
export USE_CCACHE=1
export CCACHE_DIR=/media/out/.ccache
ccache -M 60G
rm output/files/system/lib/modules/*
rm output/files/boot.img
rm output/boot.img-ramdisk.gz
make clean
make mrproper
make tc2_defconfig
make -j9
mkdir output
mkdir output/files/system
mkdir output/files/system/lib
mkdir output/files/system/lib/modules
find . -name '*.ko' -exec cp '{}' output/files/system/lib/modules \;
cd output
./mkbootfs ramdisk | gzip > boot.img-ramdisk.gz
NOW=$(date +"%m-%d-%y_%H:%M")
./mkbootimg --kernel ../arch/arm/boot/zImage --ramdisk boot.img-ramdisk.gz -o boot.img --base 80400000 --ramdisk_offset 81808000
mv boot.img files/


