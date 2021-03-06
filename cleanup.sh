#!/bin/bash

# Clean Build Files
make mrproper
rm -r -f output/
rm -f gugu0das/Cyanogenmod_13.0/ramdisk.cpio.gz
rm -f gugu0das/Cyanogenmod_13.0/boot.img
rm -f gugu0das/Touchwiz/ramdisk.cpio.gz
rm -f gugu0das/Touchwiz/boot.img
rm -f gugu0das/MIUI8/ramdisk.cpio.gz
rm -f gugu0das/MIUI8/boot.img
rm -f gugu0das/gugu0das_kernel-CM-13.0-jactivelteskt-Release-2/boot.img
rm -f gugu0das/gugu0das_kernel-TW-M-jactivelteskt-Release-2/boot.img
rm -f gugu0das/gugu0das_kernel-MIUI8-M-jactivelteskt-Release-2/boot.img
rm -f gugu0das_kernel-CM-13.0-jactivelteskt-Release-2.zip
rm -f gugu0das_kernel-TW-M-jactivelteskt-Release-2.zip
rm -f gugu0das_kernel-MIUI8-M-jactivelteskt-Release-2.zip
