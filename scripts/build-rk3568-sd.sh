#!/bin/sh
# Experimental RK3568 SD image; input DDR binary is not redistributed.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ddr=${1:?usage: build-rk3568-sd.sh DDR_BINARY}
ddr=$(realpath "$ddr")
expected=acb4e054298f0f0c8471dd3d45de07a1c77b67e687ec6e77ede365a087f1d95a
actual=$(sha256sum "$ddr" | cut -d ' ' -f 1)
[ "$actual" = "$expected" ] || { echo 'Unexpected DDR binary SHA-256' >&2; exit 1; }
python3 -c 'import elftools' || {
    echo 'Building the FIT requires Python pyelftools (set PYTHONPATH if needed)' >&2
    exit 1
}
tmp=$(mktemp -d "${TMPDIR:-/tmp/opencode}/rk3568-sd.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp/atf" "$tmp/uboot" "$tmp/out"
git -C "$root/atf" archive HEAD | tar -x -C "$tmp/atf"
git -C "$root/u-boot" archive HEAD | tar -x -C "$tmp/uboot"
# QEMU has no RK3568 core warm-reset/remap. Do not alter real hardware sources.
sed -i 's/static uint32_t need_en_crypto = 1;/static uint32_t need_en_crypto = 0;/' \
    "$tmp/atf/plat/rockchip/rk3568/drivers/pmu/pmu.c"
grep -q 'static uint32_t need_en_crypto = 0;' \
    "$tmp/atf/plat/rockchip/rk3568/drivers/pmu/pmu.c"
cross=${CROSS_COMPILE:-aarch64-linux-gnu-}
make -s -C "$tmp/atf" BUILD_BASE="$tmp/atf-build" PLAT=rk3568 \
    ERROR_DEPRECATED=0 CROSS_COMPILE="$cross" -j"${JOBS:-4}" bl31
# Only SDMMC0 is modelled; omit the unused eMMC and SDMMC2 nodes.
cat >> "$tmp/uboot/dts/upstream/src/arm64/rockchip/rk3568-rock-3a.dts" <<'EOF'
&sdhci { status = "disabled"; };
&sdmmc2 { status = "disabled"; };
EOF
make -s -C "$tmp/uboot" O="$tmp/out" CROSS_COMPILE="$cross" rock-3a-rk3568_defconfig
"$tmp/uboot/scripts/config" --file "$tmp/out/.config" \
    --disable USB --disable PCI --disable NET --disable SCSI --disable NVME \
    --disable ROCKCHIP_SFC
make -s -C "$tmp/uboot" O="$tmp/out" CROSS_COMPILE="$cross" olddefconfig
make -s -C "$tmp/uboot" O="$tmp/out" CROSS_COMPILE="$cross" \
    BL31="$tmp/atf-build/rk3568/release/bl31/bl31.elf" ROCKCHIP_TPL="$ddr" \
    -j"${JOBS:-4}"
mkdir -p "$root/build/rk3568"
python3 - "$tmp/out/u-boot-rockchip.bin" "$root/build/rk3568/sd.img" <<'PY'
import pathlib, sys
payload = pathlib.Path(sys.argv[1]).read_bytes()
if len(payload) + 0x8000 > 16 * 1024 * 1024:
    sys.exit('Combined image does not fit in the 16 MiB test card')
with open(sys.argv[2], 'wb') as f:
    f.seek(0x8000)
    f.write(payload)
    f.truncate(16 * 1024 * 1024)
PY
printf 'Built %s (U-Boot %s, TF-A %s)\n' "$root/build/rk3568/sd.img" \
    "$(git -C "$root/u-boot" rev-parse HEAD)" "$(git -C "$root/atf" rev-parse HEAD)"
