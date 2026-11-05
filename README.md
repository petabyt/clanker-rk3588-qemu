# rkqemu

Qemu platform support implemented for the RK3588. Goal is to get unmodified Armbian Ubuntu images booting to desktop.

Mostly done by GLM5.3. I gave it the leaked RK3588 TRM and ARM v8a profile spec split up into text files for referencing.

- [x] Emulate 16gb LPDDR5 + DDR training
- [x] HDMI, VOP2, eDP
- [x] GPIO, IOMUX, PMU, sha256 crypto, etc
- [x] VirtIO GPU acceleration through PCI bus (Mali is not implemented)
- [ ] NPU - possibly incorporate research from https://github.com/widgetii/orangepi5plus-npu

Milestones I gave the LLM:
1. Get my [rk bootloader](https://github.com/petabyt/rk) working
2. Get the proprietary DDR image working
    - I gave it maskrom access to an Orange Pi 5 for it to work out some details
3. Get EDKII working
4. Get ATF working
5. Get minimal Linux/busybox working
6. Boot unmodified Armbian images

Build system is setup so that a small patch (qemu.patch) is applied to the qemu submodule, and src/ is symlinked to `qemu/hw/arm/rk3588`

```
# Run a genbook image (based on the rk bootloader)
./rk3588-qemu-x86_64.AppImage -M rk3588 -bios rk/genbook_demo.img -display gtk -serial stdio

# Run a stock Armbian image with a GPU attached to PCI bus (native HDMI disconnected)
./rk3588-qemu-x86_64.AppImage \
  -M rk3588,pcie-experimental=on,hdmi-connected=off \
  -device virtio-gpu-gl-pci,id=vgpu,bus=dw-pcie,addr=0.0,multifunction=on,xres=1280,yres=720 \
  -device virtio-keyboard-pci,bus=dw-pcie,addr=0.1 \
  -device virtio-tablet-pci,bus=dw-pcie,addr=0.2 \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0,bus=dw-pcie,addr=0.3 \
  -drive if=sd,format=raw,file=Armbian_26.8.1_Orangepi5_resolute_current_6.18.43_kde-plasma_desktop.img \
  -display gtk,gl=on -serial stdio -monitor none

```

16gb Orange Pi 5 is the current testing device. The Armbian image I tested with can be downloaded [here](https://armbian.com/boards/orangepi5).
