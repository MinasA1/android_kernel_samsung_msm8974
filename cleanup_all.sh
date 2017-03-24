#!/bin/bash

# Clean Build Files
make mrproper
rm -r -f output/
rm -r -f armeb-linux-gnueabihf-linaro_6.2.1-2016.11/
rm -r -f arm-eabi-ubertc_7.0-2016.04/
rm -f gugu0das/Touchwiz/ramdisk.cpio.gz
rm -f gugu0das/Touchwiz/boot_kltekor.img
rm -f gugu0das/Touchwiz/kernel_kltekor
rm -f gugu0das/Touchwiz/boot_klteeur.img
rm -f gugu0das/Touchwiz/kernel_klteeur
rm -f gugu0das/gugu0das_kernel-NX_TW-M-kltekor-Release-2-BetaProgram_2017.03.24/boot.img
rm -f gugu0das_kernel-NX_TW-M-kltekor-Release-2-BetaProgram_2017.03.24.zip
rm -f gugu0das/gugu0das_kernel-NX_TW-M-klteeur-Release-2-BetaProgram_2017.03.24/boot.img
rm -f gugu0das_kernel-NX_TW-M-klteeur-Release-2-BetaProgram_2017.03.24.zip
