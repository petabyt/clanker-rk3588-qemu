#!/usr/bin/env bash
# build-opi5-uboot.sh — from-scratch Orange Pi 5/5B (RK3588S) boot chain.
#
# Builds, with no pre-existing Armbian/vendor image:
#   1. ATF BL31            from ./atf          (plat/rockchip/rk3588)
#   2. mainline U-Boot     from ./u-boot       (orangepi-5-rk3588s_defconfig,
#                          binman packs u-boot-rockchip.bin = idbloader.img +
#                          u-boot.itb at SPL_PAD_TO)
#   3. DDR TPL blob        from ./rk/img        (Rockchip blob — the ONLY
#                          external input; mainline has no rk3588 DDR init.
#                          Default = v1.20-armbian: extracted from the pristine
#                          Armbian image's idbloader (verified `fwver: v1.20`).
#                          Empirics under this repo's QEMU PMUGRF/DDR stubs:
#                          v1.20 trains + boots to login (P20); v1.16 hangs
#                          mid-training; v1.24 trains but leaves os_reg state
#                          our mainline sdram driver misparses (U-Boot reports
#                          bogus 2.3/16 GiB -> fdt memory fixup poisons every
#                          kernel). Override with DDR_BLOB=.
#
# Outputs (stable copies for consumers such as fhs-core's
# os/scripts/build-raw-rk3588.sh):
#   images/opi5/u-boot-rockchip.bin   write at SD sector 64 (dd seek=64)
#                                     -> idbloader RKNS v2 header @sector 64,
#                                        u-boot.itb (FDT) @sector 16384
#   images/opi5/bl31.elf
#
# Host deps (dnf): gcc-aarch64-linux-gnu dtc bison flex swig python3-devel
#   python3-setuptools python3-pyelftools openssl-devel gnutls-devel
#
# Env overrides:
#   CROSS_COMPILE=aarch64-linux-gnu-    toolchain prefix
#   DDR_BLOB=/path/to/rk3588_ddr_*.bin  DDR TPL (default: v1.20-armbian)
#   UBOOT_DEFCONFIG=..._defconfig       board defconfig (default: orangepi-5)
#   UBOOT_JOBS=N                        parallel make jobs
#
# Notes:
#   * U-Boot's control DTB stays the stock rk3588s-orangepi-5 (it carries the
#     rk3588s-orangepi-5-u-boot.dtsi overlay the SD driver needs; 5 vs 5B is a
#     kernel-boot-level difference — the kernel gets Fedora's
#     rk3588s-orangepi-5b.dtb from /boot).
#   * Builds in a dedicated git worktree (./u-boot-opi5) instead of ./u-boot:
#     U-Boot refuses an O= out-of-tree build while the source tree carries a
#     .config, and ./u-boot's in-tree CoolPi build (u-boot.bin, dts/dt.dtb) is
#     still consumed by scripts/run-fhs.py's host-load path, so it must not be
#     mrproper'd away. The worktree starts pristine — no CoolPi .config, no
#     binman-fake/ placeholders.

set -euo pipefail

RK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
DDR_BLOB="${DDR_BLOB:-$RK_DIR/rk/img/rk3588_ddr_lp4_2112MHz_lp5_2400MHz_v1.20-armbian.bin}"
UBOOT_DEFCONFIG="${UBOOT_DEFCONFIG:-orangepi-5-rk3588s_defconfig}"
UBOOT_JOBS="${UBOOT_JOBS:-$(nproc)}"
OUT_DIR="$RK_DIR/images/opi5"
BL31_ELF="$RK_DIR/atf/build/rk3588/release/bl31/bl31.elf"
UBOOT_SRC="$RK_DIR/u-boot-opi5"

fail() { echo "ERROR: $*" >&2; exit 1; }

# --- preconditions -----------------------------------------------------------
command -v "${CROSS_COMPILE}gcc" >/dev/null || \
  fail "cross compiler '${CROSS_COMPILE}gcc' not found (dnf install gcc-aarch64-linux-gnu)"
command -v dtc >/dev/null || fail "dtc not found (dnf install dtc)"
[ -f "$DDR_BLOB" ] || fail "DDR TPL blob not found: $DDR_BLOB"
python3 -c 'import elftools' 2>/dev/null || \
  fail "python3-pyelftools missing (dnf install python3-pyelftools)"

# Fedora >=44's openssl-devel dropped the deprecated <openssl/engine.h> and
# <openssl/engineerr.h> headers, but U-Boot's FIT-signature tools (lib/rsa,
# lib/aes) still include them — while libcrypto.so.3 keeps exporting the ENGINE
# symbols they call. When the native header is absent, point the include path
# at scripts/include/openssl/{engine,engineerr}.h (verbatim upstream copies
# from the openssl-3.5.9 tag, Apache-2.0).
if [ ! -f /usr/include/openssl/engine.h ]; then
  export C_INCLUDE_PATH="$RK_DIR/scripts/include${C_INCLUDE_PATH:+:$C_INCLUDE_PATH}"
fi

# --- dedicated build worktree (see header note) --------------------------------
if [ ! -d "$UBOOT_SRC/.git" ] && [ ! -f "$UBOOT_SRC/.git" ]; then
  echo "=== git worktree: $UBOOT_SRC ==="
  head="$(git -C "$RK_DIR/u-boot" rev-parse HEAD)"
  git -C "$RK_DIR/u-boot" worktree add --detach "$UBOOT_SRC" "$head"
fi
[ -f "$UBOOT_SRC/configs/$UBOOT_DEFCONFIG" ] || fail "no such defconfig: $UBOOT_DEFCONFIG"

# --- 1. ATF BL31 --------------------------------------------------------------
echo "=== ATF: PLAT=rk3588 bl31 ==="
make -C "$RK_DIR/atf" -j"$UBOOT_JOBS" PLAT=rk3588 CROSS_COMPILE="$CROSS_COMPILE" bl31
[ -f "$BL31_ELF" ] || fail "BL31 build produced no $BL31_ELF"

# --- 2. U-Boot ----------------------------------------------------------------
echo "=== U-Boot: $UBOOT_DEFCONFIG (worktree: $UBOOT_SRC) ==="
make -C "$UBOOT_SRC" CROSS_COMPILE="$CROSS_COMPILE" "$UBOOT_DEFCONFIG"
echo "=== U-Boot: build (BL31 + ROCKCHIP_TPL) ==="
# `all` runs binman (.binman_stamp) which packs u-boot-rockchip.bin;
# there is no per-output target for it.
make -C "$UBOOT_SRC" CROSS_COMPILE="$CROSS_COMPILE" \
  BL31="$BL31_ELF" ROCKCHIP_TPL="$DDR_BLOB" -j"$UBOOT_JOBS"

UBOOT_BIN="$UBOOT_SRC/u-boot-rockchip.bin"
[ -f "$UBOOT_BIN" ] || fail "build produced no u-boot-rockchip.bin"

# --- 3. magic verification ----------------------------------------------------
# Layout contract (what build-raw-rk3588.sh / the QEMU bootrom-lite expect):
#   file offset 0x0        = "RKNS" v2 idbloader header (lands @ SD sector 64
#                            when written with dd seek=64), n_images (u16)
#                            @0x0a == 2
#   file offset SPL_PAD_TO = FDT magic d00dfeed of u-boot.itb (lands @ byte
#                            0x1000000 = sector 16384 for SPL_PAD_TO=0x7f8000)
#   file must end below byte 0x2000000 (sector 32768 = start of the first GPT partition)
echo "=== Verify $UBOOT_BIN ==="
UBOOT_BIN="$UBOOT_BIN" UBOOT_SRC="$UBOOT_SRC" python3 - <<'EOF'
import os, struct, sys

path = os.environ["UBOOT_BIN"]
with open(os.environ["UBOOT_SRC"] + "/.config") as f:
    cfg = dict(l.split("=", 1) for l in f if "=" in l and not l.startswith("# "))
pad_to = int(cfg.get("CONFIG_SPL_PAD_TO", "0x200").strip('"'), 16)

data = open(path, "rb").read()
size = len(data)

def die(msg):
    print(f"MAGIC FAIL: {msg}", file=sys.stderr); sys.exit(1)

if data[0:4] != b"RKNS":
    die(f"idbloader header at offset 0 is {data[0:4]!r}, want b'RKNS'")
n_images = struct.unpack_from("<H", data, 0x0a)[0]
if n_images != 2:
    die(f"n_images={n_images}, want 2 (bootrom contract: tpl + spl)")
fdt = data[pad_to:pad_to + 4]
if fdt != b"\xd0\x0d\xfe\xed":
    die(f"no FDT magic at SPL_PAD_TO=0x{pad_to:x} (got {fdt!r})")
if 0x8000 + size > 0x2000000:
    die(f"image {size} bytes + seek=64 would cross sector 32768 (first partition starts there)")

itb_on_disk = (0x8000 + pad_to) // 512
print(f"MAGIC OK: RKNS@0, n_images=2, FDT@0x{pad_to:x} "
      f"(-> SD sector {itb_on_disk}), size {size} bytes "
      f"(-> ends at byte 0x{0x8000 + size:x})")
EOF

# --- 4. stable artifacts -------------------------------------------------------
mkdir -p "$OUT_DIR"
cp -f "$UBOOT_BIN" "$OUT_DIR/u-boot-rockchip.bin"
cp -f "$BL31_ELF" "$OUT_DIR/bl31.elf"
cp -f "$UBOOT_SRC/idbloader.img" "$OUT_DIR/idbloader.img"
cp -f "$UBOOT_SRC/u-boot.itb" "$OUT_DIR/u-boot.itb"

echo "=== Done ==="
echo "u-boot:    $(strings "$UBOOT_BIN" | grep -m1 -E '^U-Boot 20' || true)"
echo "DDR blob:  $DDR_BLOB"
echo "BL31:      $BL31_ELF"
echo "artifacts: $OUT_DIR/{u-boot-rockchip.bin,bl31.elf,idbloader.img,u-boot.itb}"
echo "flash:     dd if=$OUT_DIR/u-boot-rockchip.bin of=<SD dev> bs=512 seek=64 conv=notrunc"
