#!/bin/bash

export ARCH=arm
export CROSS_COMPILE=$(pwd)/arm-eabi-ubertc_7.0-2016.04/bin/arm-eabi-

mkdir output

# Touchwiz hlteskt Kernel
make -C $(pwd) O=output msm8974_sec_defconfig VARIANT_DEFCONFIG=msm8974_sec_hlte_skt_defconfig SELINUX_DEFCONFIG=selinux_defconfig
make -j64 -C $(pwd) O=output

cp output/arch/arm/boot/zImage $(pwd)/gugu0das/Touchwiz/kernel_hlteskt

# Clean
./cleanup.sh

# Create Output Directory
mkdir output

# Cyanogenmod / LineageOS hlteskt Kernel
make -C $(pwd) O=output cm_msm8974_sec_defconfig VARIANT_DEFCONFIG=msm8974_sec_hlte_skt_defconfig SELINUX_DEFCONFIG=selinux_defconfig
make -j64 -C $(pwd) O=output

cp output/arch/arm/boot/zImage $(pwd)/gugu0das/Cyanogenmod_13.0/kernel_hlteskt

# Clean
./cleanup.sh

# Create Output Directory
mkdir output

# Touchwiz hlteeur Kernel
make -C $(pwd) O=output msm8974_sec_defconfig VARIANT_DEFCONFIG=msm8974_sec_hlte_eur_defconfig SELINUX_DEFCONFIG=selinux_defconfig
make -j64 -C $(pwd) O=output

cp output/arch/arm/boot/zImage $(pwd)/gugu0das/Touchwiz/kernel_hlteeur

# Clean
./cleanup.sh

# Create Output Directory
mkdir output

# Cyanogenmod / LineageOS hlteeur Kernel
make -C $(pwd) O=output cm_msm8974_sec_defconfig VARIANT_DEFCONFIG=msm8974_sec_hlte_eur_defconfig SELINUX_DEFCONFIG=selinux_defconfig
make -j64 -C $(pwd) O=output

cp output/arch/arm/boot/zImage $(pwd)/gugu0das/Cyanogenmod_13.0/kernel_hlteeur

# Make Boot.img
./gugu0das/mkboot.sh