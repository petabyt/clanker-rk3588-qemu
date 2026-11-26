#!/bin/sh
# Direct-load test image from committed U-Boot, without altering its checkout.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp/opencode}/rk3528-uboot.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp/src" "$tmp/out"
git -C "$root/u-boot" archive HEAD | tar -x -C "$tmp/src"
cross=${CROSS_COMPILE:-aarch64-linux-gnu-}
make -s -C "$tmp/src" O="$tmp/out" CROSS_COMPILE="$cross" rock-2-rk3528_defconfig
"$tmp/src/scripts/config" --file "$tmp/out/.config" \
    --disable MMC --disable USB --disable PCI --disable NET \
    --disable SCSI --disable NVME --disable ROCKCHIP_SFC --disable BOOTSTD \
    --disable SARADC_ROCKCHIP --disable BUTTON_ADC --disable CMD_ADC
make -s -C "$tmp/src" O="$tmp/out" CROSS_COMPILE="$cross" -j"${JOBS:-4}" olddefconfig
make -s -C "$tmp/src" O="$tmp/out" CROSS_COMPILE="$cross" -j"${JOBS:-4}" u-boot.bin
mkdir -p "$root/build/rk3528"
cp "$tmp/out/u-boot.bin" "$root/build/rk3528/u-boot.bin"
cp "$tmp/out/u-boot" "$root/build/rk3528/u-boot"
printf 'Built RK3528 U-Boot from %s\n' "$(git -C "$root/u-boot" rev-parse HEAD)"
