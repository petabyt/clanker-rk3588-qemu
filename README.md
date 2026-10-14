# RK3588 Qemu port

Qemu platform support implemented for the RK3588. Goal is to get unmodified Armbian Ubuntu images booting to desktop.

Mostly done by GLM5.3. I gave it the leaked RK3588 TRM and ARM v8a profile spec split up into text files for referencing.

- [x] Emulate 16gb LPDDR5 + DDR training
- [x] HDMI, VOP2, eDP
- [x] GPIO, IOMUX, PMU, sha256 crypto, etc
- [ ] Implementing Mali GPU is not practical. Try and pass in virtio-gpu and get that working with VOP2 somehow.
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
