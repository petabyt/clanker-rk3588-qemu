#!/bin/sh
# Build from the committed U-Boot tree without touching its existing build.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp/opencode}/rk3568-uboot.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp/src" "$tmp/out"
git -C "$root/u-boot" archive HEAD | tar -x -C "$tmp/src"
cross=${CROSS_COMPILE:-aarch64-linux-gnu-}
make -s -C "$tmp/src" O="$tmp/out" CROSS_COMPILE="$cross" rock-3a-rk3568_defconfig
"$tmp/src/scripts/config" --file "$tmp/out/.config" \
    --disable MMC --disable USB --disable PCI --disable NET \
    --disable SCSI --disable NVME --disable ROCKCHIP_SFC
make -s -C "$tmp/src" O="$tmp/out" CROSS_COMPILE="$cross" -j"${JOBS:-4}" olddefconfig
make -s -C "$tmp/src" O="$tmp/out" CROSS_COMPILE="$cross" -j"${JOBS:-4}" u-boot.bin
mkdir -p "$root/build/rk3568"
cp "$tmp/out/u-boot.bin" "$root/build/rk3568/u-boot.bin"
printf 'Built %s from U-Boot %s\n' "$root/build/rk3568/u-boot.bin" \
    "$(git -C "$root/u-boot" rev-parse HEAD)"
