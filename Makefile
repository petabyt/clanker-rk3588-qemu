# Top-level build for the RK3588 QEMU project.
#
#   make            build QEMU + the bootloader images
#   make qemu       build qemu-system-aarch64 into build/qemu/
#   make rk         build the ./rk bootloader images (opi5.bin, genbook.bin)
#   make appimage   package qemu-system-aarch64 as a portable AppImage
#   make run-*      boot scripts appear in scripts/ as milestones land
#   make run-sd     boot the stock SD image (scratch copy, no -bios)
#   make trace-sd   same run with -d exec,nochain -> $(LOG_DIR)/trace-sd.log
#   make regress    canonical U-Boot + EDK2 no-card regressions
#   make sd-own-bl31  SD chain with our BL31 in the FIT atf-1 slot (scratch)
#   make clean      remove ./build
#   make distclean  remove ./build + rk clean
#
# ./qemu is upstream and never edited directly: this Makefile applies
# qemu.patch (Kconfig + meson line) idempotently before every configure,
# and reaches ./src through the qemu/hw/arm/rk3588 symlink.

QEMU_DIR    := qemu
BUILD_DIR   := build
QEMU_BUILD  := $(BUILD_DIR)/qemu
QEMU_BIN    := $(QEMU_BUILD)/qemu-system-aarch64
QEMU_TARGETS := aarch64-softmmu
QEMU_PATCH  := qemu.patch
QEMU_KCONFIG := $(QEMU_DIR)/hw/arm/Kconfig
QEMU_MESON   := $(QEMU_DIR)/hw/arm/meson.build
QEMU_SYMLINK := $(QEMU_DIR)/hw/arm/rk3588
APPIMAGE      := rk3588-qemu-x86_64.AppImage
APPDIR        := $(BUILD_DIR)/AppDir
APPIMG_DESKTOP := packaging/rk3588-qemu.desktop
APPIMG_ICON   := packaging/rk3588-qemu.png

.PHONY: all qemu rk appimage run-sd trace-sd regress sd-own-bl31 sd-v116 run-v116 \
	run-linux linux-initramfs linux-dtb clean distclean help

all: qemu rk

qemu: $(QEMU_BIN)

# The symlink must exist before meson parses hw/arm/meson.build.
$(QEMU_SYMLINK):
	ln -sfn ../../../src $@

# configure regenerates config-host.mak + the QEMU Makefile wrapper in one go.
# The recipe first applies qemu.patch idempotently (forward-apply if the tree
# is pristine, reverse-check if the patch is already in, error otherwise),
# so a fresh clone, a patched tree, and `make clean` all converge.
$(QEMU_BUILD)/config.status: $(QEMU_DIR)/configure $(QEMU_PATCH) \
		$(QEMU_KCONFIG) $(QEMU_MESON) $(QEMU_SYMLINK)
	@if git -C $(QEMU_DIR) apply --check $(abspath $(QEMU_PATCH)) \
			>/dev/null 2>&1; then \
		echo "Applying $(QEMU_PATCH)"; \
		git -C $(QEMU_DIR) apply $(abspath $(QEMU_PATCH)); \
	elif git -C $(QEMU_DIR) apply --reverse --check $(abspath $(QEMU_PATCH)) \
			>/dev/null 2>&1; then \
		echo "$(QEMU_PATCH): already applied"; \
	else \
		echo "ERROR: $(QEMU_PATCH) is out of sync with ./qemu." >&2; \
		echo "Regenerate it: git -C $(QEMU_DIR) diff -- hw/arm/Kconfig hw/arm/meson.build hw/sd/sd.c hw/sd/sdhci.c hw/sd/sdhci-internal.h hw/sd/sdmmc-internal.h target/arm/helper.c > $(QEMU_PATCH)" >&2; \
		exit 1; \
	fi
	@mkdir -p $(QEMU_BUILD)
	cd $(QEMU_BUILD) && ../../$(QEMU_DIR)/configure --target-list=$(QEMU_TARGETS)

# ninja re-checks sources/build.ninja itself, so this stays correct as ./src
# and the qemu.patch change (QEMU's Makefile wrapper regenerates build.ninja).
$(QEMU_BIN): $(QEMU_BUILD)/config.status FORCE
	ninja -C $(QEMU_BUILD) qemu-system-aarch64

rk:
	$(MAKE) -C rk opi5.bin genbook.bin

# Portable AppImage of qemu-system-aarch64 (linuxdeploy + appimagetool).
# linuxdeploy lays down AppDir/usr/bin + bundled shared libraries; pc-bios is
# copied to AppDir/usr/share/qemu so QEMU's CONFIG_RELOCATABLE rewriting
# (get_relocated_path: exec_dir/../share/qemu) finds the datadir wherever the
# image runs.  linuxdeploy/appimagetool are AppImages themselves:
# APPIMAGE_EXTRACT_AND_RUN keeps them working on hosts without /dev/fuse.
# AppDir is rebuilt from scratch every time: linuxdeploy will NOT re-deploy
# usr/bin/qemu-system-aarch64 if it already exists (it keeps the old copy —
# the AppImage would silently ship a stale binary after any src/ change).
appimage: qemu
	rm -rf $(APPDIR)
	APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 linuxdeploy \
		--appdir $(APPDIR) -e $(QEMU_BIN) \
		-d $(APPIMG_DESKTOP) -i $(APPIMG_ICON)
	mkdir -p $(APPDIR)/usr/share/qemu
	cp -a $(QEMU_DIR)/pc-bios/. $(APPDIR)/usr/share/qemu/
	APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 appimagetool \
		$(APPDIR) $(APPIMAGE)

# Helper runs. Each uses a scratch copy of the stock SD image (EDK2 writes to
# the card) and writes logs under $(LOG_DIR), never into the repo.
LOG_DIR   := /tmp/opencode
STOCK_SD  := edk2/orangepi-5_UEFI_Release_v1.1.img
SCRATCH_SD := $(LOG_DIR)/sd-scratch.img
SD_TIMEOUT ?= 90
QEMU_RK3588 = $(QEMU_BIN) -M rk3588 -nographic -serial mon:stdio

$(SCRATCH_SD): $(STOCK_SD)
	mkdir -p $(LOG_DIR)
	cp $(STOCK_SD) $@ && truncate -s 16M $@

run-sd: $(QEMU_BIN) $(SCRATCH_SD)
	-timeout $(SD_TIMEOUT) $(QEMU_RK3588) \
		-drive if=sd,file=$(SCRATCH_SD),format=raw \
		-d int,invalid_mem,guest_errors -D $(LOG_DIR)/run-sd-int.log

trace-sd: $(QEMU_BIN) $(SCRATCH_SD)
	-timeout $(SD_TIMEOUT) $(QEMU_RK3588) \
		-drive if=sd,file=$(SCRATCH_SD),format=raw \
		-d exec,nochain -D $(LOG_DIR)/trace-sd.log >/dev/null 2>&1

OWN_BL31_BIN := $(LOG_DIR)/bl31-own.bin
OWN_BL31_SD  := $(LOG_DIR)/sd-own-bl31.img

$(OWN_BL31_BIN): atf/build/rk3588/release/bl31/bl31.elf
	mkdir -p $(LOG_DIR)
	aarch64-linux-gnu-objcopy -O binary -j .ro -j .data $< $@

# Scratch card with our BL31 in the stock FIT's atf-1 slot (hash rewritten).
$(OWN_BL31_SD): $(OWN_BL31_BIN) $(STOCK_SD) scripts/own-bl31-sd.py
	mkdir -p $(LOG_DIR)
	python3 scripts/own-bl31-sd.py $(STOCK_SD) $(OWN_BL31_BIN) $@
	truncate -s 16M $@

# Scratch card with the v1.16 DDR TPL swapped into the stock idbloader slot
# (tpl@0x8800, size 0x12800; v1.16 blob is 0x126d8). The stock image ships
# v1.18 there - this image boots the blob the board reversing targets.
V116_TPL   := rk/img/rk3588_ddr_lp4_2112MHz_lp5_2400MHz_v1.16.bin
V116_SD    := $(LOG_DIR)/sd-v116.img

$(V116_SD): $(STOCK_SD) $(V116_TPL)
	mkdir -p $(LOG_DIR)
	cp $(STOCK_SD) $@ && truncate -s 16M $@
	dd if=$(V116_TPL) of=$@ bs=1 seek=34816 conv=notrunc status=none

sd-v116: $(V116_SD)

run-v116: $(QEMU_BIN) $(V116_SD)
	-timeout $(SD_TIMEOUT) $(QEMU_RK3588) \
		-drive if=sd,file=$(V116_SD),format=raw \
		-d int,invalid_mem,guest_errors -D $(LOG_DIR)/run-v116-int.log \
		> $(LOG_DIR)/run-v116.txt 2>&1
	@tr -d '\r' < $(LOG_DIR)/run-v116.txt | grep -a "fwver\|change to\|out\|Vref" || true

# --- P5: Linux via TF-A + U-Boot (all staged by the QEMU loader, no SD card).
# Kernel Image MUST sit at a 2 MiB-aligned address (arm64 boot protocol):
# at 0x02080000 booti relocates it ("Moving Image"), and the relocated copy
# crashed in jump_label_init (see plan.md P5). Initramfs padded to 4 MiB so
# the booti initrd size operand is a constant.
LINUX_IMAGE := linux/linux-7.2.9/arch/arm64/boot/Image
LINUX_DTB   := images/rk3588-qemu.dtb
INITRD_BIN  := images/initramfs.bin

linux-initramfs:
	initramfs/build.sh

# eMMC test image (P11): MBR + ext4 partition, mount rootfs proof.
images/emmc.img: scripts/mk-emmc-img.py
	python3 scripts/mk-emmc-img.py

linux-dtb: dts/rk3588-qemu.dts
	linux/linux-7.2.9/scripts/dtc/dtc -Wno-unit_address_vs_reg \
		-I dts -O dtb -o /tmp/opencode/p5.dtb dts/rk3588-qemu.dts
	python3 -c "d=open('/tmp/opencode/p5.dtb','rb').read(); \
	open('$(LINUX_DTB)','wb').write(d+b'\0'*(0x20000-len(d)))"

STOCK_DTB := linux/linux-7.2.9/arch/arm64/boot/dts/rockchip/rk3588s-orangepi-5.dtb
images/rk3588s-orangepi-5-stock.dtb: $(STOCK_DTB)
	mkdir -p images
	python3 -c "d=open('$(STOCK_DTB)','rb').read(); \
	open('images/rk3588s-orangepi-5-stock.dtb','wb').write(d+b'\0'*(0x20000-len(d)))"

# Stock DTB with ONLY the eMMC node (sdhci@fe2e0000) enabled (status="okay"),
# exactly the Armbian board patch: the mainline node already carries
# bus-width 8 / supports-cqe / mmc-hs400-1_8v / mmc-hs400-enhanced-strobe.
# The cpu-idle-states references are dropped as well: the deep PSCI suspend
# state drives TF-A into cortex-a76_core_pwr_dwn, whose CPUECTLR_EL3 read
# (s3_0_c15_c2_7) is UNDEF under TCG and warm-resets the machine.
EMMC_DTB := images/rk3588s-orangepi-5-emmc.dtb
$(EMMC_DTB): images/rk3588s-orangepi-5-stock.dtb
	cp images/rk3588s-orangepi-5-stock.dtb $(EMMC_DTB)
	fdtput -t s $(EMMC_DTB) /mmc@fe2e0000 status okay
	for cpu in $$(fdtget -l $(EMMC_DTB) /cpus); do \
		fdtput -d $(EMMC_DTB) /cpus/$$cpu cpu-idle-states 2>/dev/null || true; \
	done

# P12 stock-DTB milestone: identical to run-linux but with the unmodified
# in-tree rk3588s-orangepi-5 DTB (Armbian kernel DTBs derive from it).
run-linux-stock: $(QEMU_BIN) images/rk3588s-orangepi-5-stock.dtb $(INITRD_BIN) images/emmc.img
	mkdir -p $(LOG_DIR)
	-timeout 290 $(QEMU_RK3588) \
		-bios atf/build/rk3588/release/bl31/bl31.elf \
		-device loader,file=u-boot/u-boot.bin,addr=0x800000,force-raw=on \
		-device loader,file=$(LINUX_IMAGE),addr=0x02000000,force-raw=on \
		-device loader,file=$(INITRD_BIN),addr=0x09000000,force-raw=on \
		-device loader,file=images/rk3588s-orangepi-5-stock.dtb,addr=0x0a100000,force-raw=on \
		-dtb u-boot/dts/dt.dtb \
		-drive if=sd,file=images/emmc.img,format=raw,unit=1 \
		-d int,invalid_mem,guest_errors -D $(LOG_DIR)/run-linux-stock-int.log \
		> $(LOG_DIR)/run-linux-stock.txt 2>&1
	@tr -d '\r' < $(LOG_DIR)/run-linux-stock.txt | grep -a "LINUX-OK\|Run /init\|Kernel panic\|Starting kernel\|ttyS2" || true

# P13 eMMC milestone: stock-DTB boot with the eMMC node enabled - drives the
# dwcmshc HS200/HS400 + DLL + CQE (CQHCI) paths in the model.
run-linux-emmc: $(QEMU_BIN) $(EMMC_DTB) $(INITRD_BIN) images/emmc.img
	mkdir -p $(LOG_DIR)
	-timeout 290 $(QEMU_RK3588) \
		-bios atf/build/rk3588/release/bl31/bl31.elf \
		-device loader,file=u-boot/u-boot.bin,addr=0x800000,force-raw=on \
		-device loader,file=$(LINUX_IMAGE),addr=0x02000000,force-raw=on \
		-device loader,file=$(INITRD_BIN),addr=0x09000000,force-raw=on \
		-device loader,file=$(EMMC_DTB),addr=0x0a100000,force-raw=on \
		-dtb u-boot/dts/dt.dtb \
		-drive if=sd,file=images/emmc.img,format=raw,unit=1 \
		-d int,invalid_mem,guest_errors -D $(LOG_DIR)/run-linux-emmc-int.log \
		> $(LOG_DIR)/run-linux-emmc.txt 2>&1
	@tr -d '\r' < $(LOG_DIR)/run-linux-emmc.txt | grep -aE "LINUX-OK|Run /init|Kernel panic|Starting kernel|ttyS2|mmc0:|\[PASS\]|\[FAIL\]|CQHCI|tuning|HS[0-9]+|dwcmshc" || true

run-linux: $(QEMU_BIN) $(LINUX_DTB) $(INITRD_BIN) images/emmc.img
	mkdir -p $(LOG_DIR)
	-timeout 110 $(QEMU_RK3588) \
		-bios atf/build/rk3588/release/bl31/bl31.elf \
		-device loader,file=u-boot/u-boot.bin,addr=0x800000,force-raw=on \
		-device loader,file=$(LINUX_IMAGE),addr=0x02000000,force-raw=on \
		-device loader,file=$(INITRD_BIN),addr=0x09000000,force-raw=on \
		-device loader,file=$(LINUX_DTB),addr=0x0a100000,force-raw=on \
		-dtb u-boot/dts/dt.dtb \
		-drive if=sd,file=images/emmc.img,format=raw,unit=1 \
		-d int,invalid_mem,guest_errors -D $(LOG_DIR)/run-linux-int.log \
		> $(LOG_DIR)/run-linux.txt 2>&1
	@tr -d '\r' < $(LOG_DIR)/run-linux.txt | grep -a "LINUX-OK\|Run /init\|Kernel panic\|dw-apb-uart\|Starting kernel" || true

sd-own-bl31: $(QEMU_BIN) $(OWN_BL31_SD)
	-timeout $(SD_TIMEOUT) $(QEMU_RK3588) \
		-drive if=sd,file=$(OWN_BL31_SD),format=raw \
		-d int,invalid_mem,guest_errors -D $(LOG_DIR)/sd-own-bl31-int.log \
		> $(LOG_DIR)/sd-own-bl31.txt 2>&1
	@tr -d '\r' < $(LOG_DIR)/sd-own-bl31.txt | grep -a "Checking atf-1\|NOTICE\|ERROR\|Jumping" || true

# Canonical U-Boot run must reach "=>" with 0 invalid accesses; EDK2 no-card
# must print the UEFI banner with 0 invalid accesses. Timeouts (rc=124) are
# expected: the guest is interactive.
regress: $(QEMU_BIN)
	mkdir -p $(LOG_DIR)
	-timeout 45 $(QEMU_RK3588) \
		-bios atf/build/rk3588/release/bl31/bl31.elf \
		-device loader,file=u-boot/u-boot.bin,addr=0x800000,force-raw=on \
		-dtb u-boot/dts/dt.dtb -d int,invalid_mem,guest_errors \
		-D $(LOG_DIR)/reg-uboot.log > $(LOG_DIR)/reg-uboot.txt 2>&1
	@echo "U-Boot: prompt=$$(tr -d '\r' < $(LOG_DIR)/reg-uboot.txt | grep -c '=>') invalid=$$(grep -ci invalid $(LOG_DIR)/reg-uboot.log)"
	-timeout 150 $(QEMU_RK3588) \
		-bios atf/build/rk3588/release/bl31/bl31.elf \
		-device loader,file=edk2/BL33_AP_UEFI.Fv,addr=0x200000,force-raw=on \
		-machine bl33-base=0x200000 -dtb u-boot/dts/dt.dtb \
		-d int,invalid_mem,guest_errors -D $(LOG_DIR)/reg-edk2.log > $(LOG_DIR)/reg-edk2.txt 2>&1
	@echo "EDK2: banner=$$(tr -d '\r' < $(LOG_DIR)/reg-edk2.txt | grep -ci 'UEFI\|Boot Manager') invalid=$$(grep -ci invalid $(LOG_DIR)/reg-edk2.log)"

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(APPIMAGE)

distclean: clean
	$(MAKE) -C rk clean

help:
	@echo "Targets:"
	@echo "  all     - qemu + rk (default)"
	@echo "  qemu    - qemu-system-aarch64 in $(QEMU_BUILD)/"
	@echo "  rk      - bootloader images via ./rk"
	@echo "  appimage - portable $(APPIMAGE) (linuxdeploy + appimagetool)"
	@echo "  clean   - remove $(BUILD_DIR)/ + $(APPIMAGE)"
	@echo "  distclean - clean + rk clean"

FORCE:
