#!/bin/bash
rm output/system/lib/modules/*
make clean
make mrproper
make tc2_defconfig
make -j9
mkdir output
mkdir output/system
mkdir output/system/lib
mkdir output/system/lib/modules
find . -name '*.ko' -exec cp '{}' output/system/lib/modules \;
