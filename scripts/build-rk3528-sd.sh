#!/bin/sh
# Experimental SD image. DDR and BL31 binaries are supplied by the caller.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ddr=$(realpath "${1:?usage: build-rk3528-sd.sh DDR_BINARY BL31_ELF}")
bl31=$(realpath "${2:?usage: build-rk3528-sd.sh DDR_BINARY BL31_ELF}")
test "$(sha256sum "$ddr" | cut -d ' ' -f 1)" = a4a6d866f789d0c063334e4863ac7b4ed11bcdc735ea48fe3c0200575592edf8 || {
    echo 'Unexpected DDR binary SHA-256' >&2; exit 1;
}
test "$(sha256sum "$bl31" | cut -d ' ' -f 1)" = ab0b30d59e81b8a14ec6f45316d1dd9befc23fae02372a989ec63c90a4790bab || {
    echo 'Unexpected BL31 binary SHA-256' >&2; exit 1;
}
tmp=$(mktemp -d "${TMPDIR:-/tmp/opencode}/rk3528-sd.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir "$tmp/src" "$tmp/out"
git -C "$root/u-boot" archive HEAD | tar -x -C "$tmp/src"
# The test models the SD slot, not the optional eMMC controller.
printf '\n&sdhci { status = "disabled"; };\n' >> \
    "$tmp/src/dts/upstream/src/arm64/rockchip/rk3528-rock-2a.dts"
cross=${CROSS_COMPILE:-aarch64-linux-gnu-}
make -s -C "$tmp/src" O="$tmp/out" CROSS_COMPILE="$cross" rock-2-rk3528_defconfig
"$tmp/src/scripts/config" --file "$tmp/out/.config" \
    --disable USB --disable PCI --disable NET --disable SCSI --disable NVME \
    --disable ROCKCHIP_SFC --disable SARADC_ROCKCHIP --disable BUTTON_ADC \
    --disable CMD_ADC
make -s -C "$tmp/src" O="$tmp/out" CROSS_COMPILE="$cross" olddefconfig
make -s -C "$tmp/src" O="$tmp/out" CROSS_COMPILE="$cross" \
    ROCKCHIP_TPL="$ddr" BL31="$bl31" -j"${JOBS:-4}"
mkdir -p "$root/build/rk3528"
python3 - "$tmp/out/u-boot-rockchip.bin" "$root/build/rk3528/sd.img" <<'PY'
import pathlib, sys
payload = pathlib.Path(sys.argv[1]).read_bytes()
if len(payload) + 0x8000 > 16 * 1024 * 1024:
    sys.exit('combined image exceeds 16 MiB')
with open(sys.argv[2], 'wb') as f:
    f.seek(0x8000)
    f.write(payload)
    f.truncate(16 * 1024 * 1024)
PY
printf 'Built RK3528 SD image from U-Boot %s\n' "$(git -C "$root/u-boot" rev-parse HEAD)"
