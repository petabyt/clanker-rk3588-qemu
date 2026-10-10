# RK3588 QEMU — Plan

Goal: emulate the RK3588 subsystem in QEMU well enough to boot, on the Orange Pi 5
and Cool-Pi Genbook reference boards:

  1. ./rk bootloader (minimal, debuggable)  2. U-Boot  3. upstream Linux  4. edk2-rk3588

## Decisions (settled)
- Build system: top-level Makefile (meson subproject rejected: QEMU's meson.build reads
  config-host.mak at parse time and calls add_global_link_arguments, which subprojects
  may not do). `make` applies qemu.patch idempotently + creates the src symlink,
  then configure --target-list=aarch64-softmmu into build/qemu + ninja; also builds ./rk.
- ./qemu stays upstream; only qemu.patch (Kconfig + 1 meson line) + symlink
  qemu/hw/arm/rk3588 -> ../../../src are applied. Patch apply is verified on every
  configure: pristine tree -> apply, already applied -> reverse-check ok, else error
  with regeneration hint. Regenerate via
  `git -C qemu diff -- hw/arm/Kconfig hw/arm/meson.build > qemu.patch`.
- ./rk is patchable for handoff fixes (DTB in x0, PSCI CPU_ON, parking), keep minimal,
  must keep working on real hardware.
- U-Boot and Linux built from source (aarch64, mainline).
- First Linux boot: no storage model; kernel+dtb+initrd staged in RAM by QEMU
  (-device loader), u-boot `booti` at fixed addresses, busybox initramfs root.
- Display: serial-first; VOP2 scanout + eDP AUX/DPCD/EDID window only at P6.

## Memory map (from rk/src/rk3588/rk3588.h)
  RAM        low [0x0, min(-m, 0xF0000000)) + rest at 0x100000000 (default 4G -> 3.75G + 0.25G)
             [stack 0xC0000000, FB 0xD0000000 must stay in low RAM; default -m 4G covers both]
  GICD/GICR  0xFE600000 / 0xFE680000          (GICv3, + ITS later)
  UART0/2    0xFD890000 / 0xFEB50000          (bootloader uses UART2)
  CRU        0xFD7C0000  SECURECRU 0xFD7D0000  PMU1CRU 0xFD7F0000
  PMU        0xFD8D0000  (version reg must read 0x3588)
  GPIO0-4    0xFD8A0000, 0xFEC20000-0xFEC50000
  IOC        PMU1 0xFD5F0000, PMU2 0xFD5F4000, BUS 0xFD5F8000, VCCIO 0xFD5FA000
  SGRF/GRF   PMU0 0xFD580000, PMU1 0xFD582000, SYS 0xFD58C000, VO0/1/VOP 0xFD5A6000-8000
  PWM0-3     0xFD8B0000, 0xFEBD0000-0xFEBF0000
  VOP2       0xFDD90000   (scanout -> QEMU console at P6)
  eDP0/1     0xFDEC0000 / 0xFDED0000  (status model first; AUX/DPCD/EDID at P6)
  HDPTX PHY  0xFED60000/70000 0x2000 (u-boot hdptx-phy binding) + GRF 0xFD5E0000/4000
  DDR blob   not run (guest path makes DDR a NOP)

## Device strategy
- Real models:  DW-APB UART (new, in src/), arch timer, GICv3 (QEMU stock).
- Register-file stubs that satisfy guest polling loops:
    CRU (PLL lock bit = 1), PMU (version=0x3588, gate status = on),
    GPIO/IOC/SGRF/GRF/PWM: hiword write-mask (rk_clr_set_bits) vs plain RW (RMW),
    VOP2 (RW regs until P6), eDP (HPD/PLL/DET/CHA/STRM status bits).
- Discovery loop: boot with `-d unimp,guest_errors` -> every log line = missing MMIO.
- No BootROM emulation in P1-P5: -bios image loaded at 0x0, CPU reset EL3 pc=0.

## Milestones
P0  Scaffold: symlink + qemu.patch + top-level Makefile; qemu builds; `-M rk3588` accepted.
    Exit: `make` succeeds, then
    `build/qemu/qemu-system-aarch64 -M help | grep rk3588` -> "rk3588  Rockchip RK3588".
    DONE (2026-10-07): make built qemu-system-aarch64 + rk images; -M help lists rk3588;
    machine idles without crash (timeout-killed); patch apply verified pristine/applied.
P1  Hello World: RAM + GICv3 + DW UART + timer + EL3 reset.
    Exit: `make -C rk opi5.bin` runs, serial shows "[UART] Hello, World".
    DONE (2026-10-07): stock arm-gicv3 wired at GICD 0xFE600000 / GICR 0xFE680000
    (8 redists, security extensions, 480 SPIs + 32, timer/maintenance/PPI wiring as
    virt/sbsa-ref); new src/dw-apb-uart.c device (UART2=serial_hd(0) at 0xFEB50000,
    UART0=serial_hd(1) at 0xFD890000); -bios loaded at 0x0, arm_load_kernel registers
    the EL3 reset (pc=0). Exit output:
      $ make -C rk opi5.bin
      make: 'opi5.bin' is up to date.
      $ timeout 8 build/qemu/qemu-system-aarch64 -M rk3588 -nographic \
            -serial mon:stdio -bios rk/opi5.bin
      [UART] Hello, World
    Extra checks: clean under `-d unimp,guest_errors`; boots at -smp 1/2/8;
    PATCH-SYNC-OK after adding dw-apb-uart.c to the meson line in qemu.patch.
P2  Full genbook path: CRU/PMU/IOC/SGRF/GPIO/PWM/VOP2/eDP register models.
    Exit: genbook.bin runs to completion ("VOP video is active", payload jump),
    no hangs, no unhandled MMIO in touched regions.
    DONE (2026-10-07): new src/rk3588-stubs.c(+.h), non-QOM register files wired
    in rk3588_init. Two semantics classes: hiword write-mask for CRU/PMU/GRF/SGRF/
    IOC/GPIO/DDR-GRFs (stores data only where mask<<16 bits set — rk_clr_set_bits
    sequences rely on it; reads return stored low16), plain RW for VOP2/eDP/PWM/
    EDP-TX PHY (guest uses |=/&= RMW). Read hooks: CRU CON6 +0x178/+0x218 bit15
    (PLL locked), PMU +0x4000 = 0x3588, eDP 0x600 bit2 DET_STA / 0x608 bit6 HPD /
    0x6c0 bit4 PLL lock (power-gate status, CHA_STA, STRM_VALID read 0 => fast
    paths). EDP-TX regions 0x2000 (u-boot rockchip,rk3588-hdptx-phy.yaml; guest
    RockchipHdptxPhyReset touches +0x1804). Discovery loop: guest EL3 exceptions
    (ESR/ELR/x0) -> addr2line on genbook.bin.elf -> driver source -> fixed stub.
    Exit output:
      $ timeout 8 build/qemu/qemu-system-aarch64 -M rk3588 -nographic \
            -serial mon:stdio -bios rk/genbook.bin -d unimp,guest_errors \
            -D qemu-p2.log
      [UART] Powering up pd 15 (0b10101)
      [UART] Powering up pd 1c (0b11100)
      [UART] Powering up pd 11 (0b10001)
      [UART] Powering up pd 13 (0b10011)
      [UART] VOP video is active
      [UART] edk2_hdptx_phy_init
      [UART] RockchipHdptxPhyInit
      [UART] RockchipHdptxPhyReset
      [UART] RockchipHdptxPhyDpPllInit
      [UART] RockchipHdptxPhyDpAuxInit
      [UART] edk2_hdptx_phy_init exit
      [UART] edk2_hdptx_phy_configure_edp
      [UART] RockchipHdptxPhySetRate
      [UART] RockchipHdptxPhySetVoltage
      [UART] RockchipHdptxPhySetVoltage
      [UART] edk2_hdptx_phy_configure_edp exit
      [UART] HPD status OK: 40 (0b1000000)
      [UART] PLL is locked
      [UART] Checking clock...
      [UART] Waiting on STA: 0 (0b0)
      [UART] Waiting for video to turn on... 40 (0b1000000)
      [UART] enabled video output
      [UART] Bad payload magic
      qemu-system-aarch64: terminating on signal 15 from pid ... (timeout)
      exit=124
    ("Bad payload magic" = firmware reached jump_to_payload with no P3 payload,
    bsod+halt -> timeout is expected until P3.) Extra checks: qemu-p2.log is
    0 bytes (no unimp/guest_errors); also verified at -smp 8.
P3  Payload path: genbook.bin + demo.bin (FuPayloadHeader, EL2 drop, FU_* SMC).
    Exit: demo runs, SMC services answer.
    DONE (2026-10-07). Payload image = `cat genbook.bin demo.bin`
    (rk make target demo_genbook.bin; demo linked -Ttext=0xa00000, header flags
    RELOCATION -> firmware copies it to 0xa00000, start_in_el2 drops to EL2,
    demo issues FU_* via `smc #0` handled by process_firmware_call at EL3).
    Root-cause fix during P3 (./rk patch, documented here): start_in_el2
    (rk/src/asm.S) rebuilt SCTLR_EL3 setting bit19 WXN — architecturally active
    at EL3 (AArch64-sctlr_el3.xml), which made every fetch of the RWX 1GB EL3
    blocks a permission fault (ESR IFSC 0x0d level 1; nested abort loop at
    VBAR+0x200). Fix = do not set WXN (hw-compatible; would fault on hardware
    too). Exit output:
      $ timeout 10 build/qemu/qemu-system-aarch64 -M rk3588 -nographic \
            -serial mon:stdio -bios rk/demo_genbook.bin -d unimp,guest_errors \
            -D qemu-p3.log
      [UART] Powering up pd 15 (0b10101)
      [UART] Powering up pd 1c (0b11100)
      [UART] Powering up pd 11 (0b10001)
      [UART] Powering up pd 13 (0b10011)
      [UART] VOP video is active
      [UART] edk2_hdptx_phy_init
      ... (hdptx PHY init/configure, HPD OK, PLL locked, enabled video output)
      [UART] Calling a00000 (0b1010000000000000000000000)
      [UART] Dropping down into EL2
      Hello World from Payload
      FUTO Bootloader payload binary, running on 'Genbook'
      We are in EL2
      Valid DTB present
      Memory description map:
      Range: 0x0-0x67F0
      Range: 0x67F0-0x10000000
      Range: 0x10000000-0xBFFF0000
      Range: 0xC0000000-0x100000000
      Turning off...
      qemu-system-aarch64: terminating on signal 15 ... (timeout)
      exit=124
    ("Turning off..." = demo's PSCI_SYSTEM_OFF SMC, firmware halts -> timeout
    is the expected end state.) Extra checks: qemu-p3.log 0 bytes (no
    unimp/guest_errors); `-d int` shows exactly 25 exceptions, all Secure
    Monitor Calls (FU_GET_DEVICE_INFO/DTB/MEM_MAP/PRINT + SYSTEM_OFF), zero
    faults/aborts.
P4  U-Boot: mainline u-boot for rk3588 built from source, entered as payload
    (handoff: DTB via FU_GET_DTB / x0 trampoline, PSCI from rk firmware).
    Exit: u-boot banner + prompt on serial.
    STATUS: DONE (2026-10-07). Exit command:
        timeout 60 build/qemu/qemu-system-aarch64 -M rk3588 -nographic \
          -serial mon:stdio -bios rk/genbook_uboot.bin \
          -d unimp,guest_errors,invalid_mem -D qemu-p4.log
    Output (firmware prefix elided after "[UART] Jumping to u-boot"; full
    run starts with the rk firmware power-up/edp prints):
        [UART] enabled video output
        [UART] Jumping to u-boot at EL2 (x0 = DTB)

        U-Boot 2026.10-g10adf52ed29d-dirty (Oct 07 2026 - 23:07:14 +0000)

        Model: CoolPi CM5 GenBook
        SoC:   RK0000
        DRAM:  512 MiB (total 510 MiB)
        Core:  800 devices, 26 uclasses, devicetree: separate
        MMC:
        Loading Environment from nowhere...
        Loading Environment from nowhere... OK
        In:    serial@feb50000
        Out:   serial@feb50000
        Err:   serial@feb50000
        Model: CoolPi CM5 GenBook
        SoC:   RK0000
        Net:   No ethernet found.
        Hit any key to stop autoboot: 2
        Hit any key to stop autoboot: 1
        Hit any key to stop autoboot: 0
        Scanning for bootflows in all bootdevs
        Seq  Method       State   Uclass    Part  Name                      Filename
        ---  -----------  ------  --------  ----  ---------------------------  --------
        Scanning global bootmeth 'efi_mgr':
        Cannot persist EFI variables without system partition
        No USB controllers found
        No bootdevs for 'mmc0'
        No more bootdevs
        ---  -----------  ------  --------  ----  ---------------------------  --------
        (0 bootflows, 0 valid)
        => (prompt; sits here until timeout) exit=124
    Extra checks: qemu-p4.log 0 bytes (no unimp/guest_errors/invalid_mem),
    zero exceptions in -d int beyond normal SMCs during the rk firmware stage.
    Exit-criterion caveats (accepted for P4, revisited in P5):
    - DRAM reported as "512 MiB (total 510 MiB)" — u-boot sees only the RAM
      below 4 GiB reported by its own cell count (cosmetic; 4 GiB total exists,
      split above 0x100000000).
    - "SoC: RK0000" — GRF/PMU version stubs return 0 (cosmetic).
    - No storage/USB/net — bootflow finds 0 bootdevs by design; P5 stages
      kernel via the QEMU loader anyway.
    P4 handoff design: combined image = cat genbook.bin uboot-payload.bin.
    uboot-payload.bin = rk/src/uboot_head.S (FuPayloadHeader carrying the new
    PAYLOAD_FLAG_UBOOT flag, .incbin "u-boot.bin") linked -Ttext=0 + objcopy.
    Firmware jump_to_payload sees the UBOOT flag -> jump_to_uboot(0x00800000,
    image_size - sizeof(header)) -> memcpy u-boot.bin to its TEXT_BASE, fetch
    the DTB via FU_GET_DTB, then start_in_el2(text, STACK2_TOP, dtb).
    P4 patches/changes, all documented here per rule:
    ./rk (minimal, real-hardware compatible):
    - src/uboot_head.S (new), Makefile targets uboot-payload.bin +
      genbook_uboot.bin (ld --oformat binary does not work for AArch64 ->
      gcc -c + ld -e _start -Ttext=0 + objcopy).
    - firmware.h: PAYLOAD_FLAG_UBOOT (1 << 2); firmware.c: UBOOT branch in
      jump_to_payload + new jump_to_uboot() (memcpy, FU_GET_DTB, EL2 entry).
    - asm.S + main.h: start_in_el2(addr, stack, x0_value) — x2 becomes the
      x0 value delivered at EL2 entry (previously x0 was clobbered before eret).
    QEMU src/ (stubs learned from u-boot's driver probes, find-gaps recipe
    -d unimp,guest_errors,invalid_mem + addr2line on u-boot ELF):
    - OTP 0xfecc0000 (INT_ST 0x84 bit1 RD_DONE on read) + TRNG 0xfe378000
      (VERSION 0xf0=0x46bc, ISTAT 0x14 bit0) — pre-reloc; fixes hang after
      "DRAM:" (rockchip_cpuid_from_efuse) and rng probe.
    - PLL lock polls: read hook sets bit15 for any CRU offset ending 0x18/0x38
      (per-PLL PLLCON(6)); hooks added to php/secure/sbus/pmu1 + new
      bigcore0/bigcore1/dsu windows at 0xfd810000/0xfd812000/0xfd818000 —
      fixed the post-relocate infinite udelay/rk3588_pll_set_rate loop
      (diagnosed via -d exec hot-PC histogram + QMP xp scan that located the
      relocated image at 0x1FF07000, reloc_off 0x1F707000, for addr2line).
    - IOC merged into one 0xfd5f0000/0x10000 block (the four 0x1000 split
      blocks left gaps -> SError on bus-ioc write at 0xfd5f911c).
    - New hiword GRF blocks: pipe-phy2/usbdpphy0/usb2phy0/2/3-grf,
      mipidcphy0/1-grf (regmap reads during efi_mgr/bootdev scan).
    ./u-boot (board config, documented here):
    - .config: CONFIG_SCMI_FIRMWARE=n (weak imply from mach-rockchip/Kconfig
      would route SCMI SMCs into our EL3 firmware and fail initf_dm;
      disable via .config + olddefconfig).
    - arch/arm/dts/rk3588-coolpi-cm5-genbook-u-boot.dtsi: status=disabled for
      &spi2 (rk806 PMIC SPI — no SPI modelled; crashed at probe), &sdhci
      (eMMC at 0xfe2e0000 — no SDHCI modelled yet; P5 will add one if
      storage boot is needed), &usb_host{0,1}_{ehci,ohci,xhci} and
      &usb_host2_xhci (EHCI read at 0xfc800000 during bootflow scan).
    - Build: make CROSS_COMPILE=aarch64-linux-gnu- u-boot.bin (binman's
      missing OP-TEE tee-os warning is harmless for direct payload entry).
P8  (user request 2026-10-07; ordered after P4, labels kept) Boot a completely
    unmodified RK3588 image: parse the Rockchip image header (makeboot --v2
    layout: idbloader @0x800, DDR blob + OS image following), run the stock DDR
    init blob (genbook-ddr.bin entered at 0xFF001000, secondaries parked via the
    WFE scheme), then return from DRAM init and jump back to the main image at
    0 — byte-identical stock image boots. First target: unmodified genbook
    image (eDP setup is the easiest path to bring up).
    STATUS: DONE (2026-10-08). Exit command and output:
      $ timeout 60 build/qemu/qemu-system-aarch64 -M rk3588 -nographic \
            -serial mon:stdio -bios rk/genbook_demo.img \
            -d unimp,guest_errors,invalid_mem -D qemu-p8.log
      qemu-system-aarch64: rk3588: v2 image, ddr@8800+12e00 os@1b600+28000
      DDR 9fffbe1e78 cym 24/02/04-10:09:20,fwver: v1.16
      unknown device
      If no 'may be ch* soldering abnormality' is printed, it may be ch0 soldering abnormality
      error
      ERR
      [UART] Powering up pd 15 (0b10101)
      [UART] Powering up pd 1c (0b11100)
      [UART] Powering up pd 11 (0b10001)
      [UART] Powering up pd 13 (0b10011)
      [UART] VOP video is active
      [UART] edk2_hdptx_phy_init
      [UART] RockchipHdptxPhyInit
      [UART] RockchipHdptxPhyReset
      [UART] RockchipHdptxPhyDpPllInit
      [UART] RockchipHdptxPhyDpAuxInit
      [UART] edk2_hdptx_phy_init exit
      [UART] edk2_hdptx_phy_configure_edp
      [UART] RockchipHdptxPhySetRate
      [UART] RockchipHdptxPhySetVoltage
      [UART] RockchipHdptxPhySetVoltage
      [UART] edk2_hdptx_phy_configure_edp exit
      [UART] HPD status OK: 40 (0b1000000)
      [UART] PLL is locked
      [UART] Checking clock...
      [UART] Waiting on STA: 0 (0b0)
      [UART] Waiting for video to turn on... 40 (0b1000000)
      [UART] enabled video output
      [UART] Calling a00000 (0b101000000000000000000000)
      [UART] Dropping down into EL2
      Hello World from Payload
      FUTO Bootloader payload binary, running on 'Genbook'
      We are in EL2
      Valid DTB present
      Memory description map:
      Range: 0x0-0x6950
      Range: 0x6950-0x10000000
      Range: 0x10000000-0xBFFF0000
      Range: 0xC0000000-0x100000000
      Turning off...
    qemu-p8.log is empty (0 unimp / guest_errors / invalid_mem across the whole
    boot).  Regression: `timeout 30 ... -bios rk/genbook_uboot.bin` still boots
    U-Boot ("Model: CoolPi CM5 GenBook / DRAM: 512 MiB").
    Shutdown follow-up (2026-10-08, post-milestone): the run now self-exits
    rc=0 after "Turning off..." — payload fw_handler(PSCI_SYSTEM_OFF) and
    U-Boot `poweroff` both issue SMC -> EL3 firmware process_firmware_call ->
    plat_shutdown() (rk/src/rk3588/io.c) writes 0x4f464621 ('OFF!') to
    0xfd58affc (last word of the grf-boot window 0xfd58a000+0x1000), then
    falls into plat_reset(); QEMU rk_stub_poweroff_write (write_hook on the
    grf-boot window, src/rk3588-stubs.c) matches (off==0xffc, 'OFF!') ->
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN).  The reset
    write (fd58a004=0xffff0000) is deliberately NOT hooked (reset-vs-shutdown
    ambiguity / boot loops).  U-Boot `poweroff` path: CONFIG_CMD_POWEROFF +
    CONFIG_SYSRESET_CMD_POWEROFF + CONFIG_SYSRESET_PSCI=y — psci_bind binds
    the psci-sysreset child, do_poweroff -> sysreset_walk(SYSRESET_POWER_OFF)
    -> psci_sys_poweroff() -> invoke SMC -> QEMU exits rc=0 (banner
    "U-Boot 2026.10-g10adf52ed29d-dirty", DRAM 512 MiB, `=> ` prompt reached,
    then poweroff at the prompt).  rk/src/rk3588/genbook.dts gained a /psci
    node (correct for the OS-facing DT; U-Boot's control FDT is its own
    linked DTB — fdtdec_setup ignores the x0 DTB pointer — so this does not
    affect U-Boot).  _end_of_image grew to 0x6950 with these additions.
    Design notes (code: src/rk3588.c bootrom-lite + src/rk3588-stubs.c):
    - bootrom-lite parses the makeboot v2 header at machine init (not via ROM
      reset emulation — QEMU rom_reset deferred bytes land AFTER init, so we
      stage with physical_memory_write directly).  RkHeaderV2 is packed,
      nested ImageEntry is UNPACKED stride 88 (0x58): verified layout for
      rk/genbook_demo.img (275968 B, rule rk/Makefile:74) is
      ddr@0x8800+0x12e00, os@0x1b600+0x28000 — logged as
      "rk3588: v2 image, ddr@... os@...".  DDR stage loaded at SRAM
      0xff000000, second-stage/OS staged per header; on reset the handler
      sets ELR/SPSR and enters the blob at 0xff001000 with the bootrom
      register contract (x0=0xff000078, x1=8, SP=SRAM top, x30=return
      trampoline).  The blob `ret`s back into the trampoline, which jumps to
      the main image entry at 0x (the bootrom's entry-value check is
      skipped — documented deviation).
    - genbook-ddr.bin: ddr.S linked -Ttext=0xFF001000 = `b shim` at
      0xff001000 (register-preserving trampoline -> ddr_shim C code: GPIO
      rail enables + PWM6 backlight -> restore -> `b bin`) then the stock
      vendor blob body (.incbin offset 4, byte-identical) executing from
      0xff001004.  Disasm recipe:
      aarch64-linux-gnu-objdump -D -b binary -m aarch64
      --adjust-vma=0xff001000 rk/genbook-ddr.bin (guest ELR - 0xff001000
      = file offset).
    - DDR blob register model (rk3588-stubs.c): window table + RkStub with
      RkStubBlock.track_engine on ddr-probe (0xf7000000/0x4000000) and
      ddr-cfg2 (0xfe0c0000/0x40000); engine sub-blocks keyed by
      offset>>16 (f701/f801/f901/fa01 config engines).  Rules, each derived
      from a hang site via -d exec histogram + gdb-multiarch:
        +0x80  START write: bit31 reads back self-cleared;
        +0x90  bit0 busy stays clear, bit16 read-done (access helpers
               ff004fcc write-path / ff005dbc read-path; +0x94 returns data);
        +0x14  0 until any +0xC80 command, then done/ready bit0
               (ff00b790 [x19+436] gate reads 8, same value the bootrom
               passes in x1 — normal, not a bug);
        +0xb4  bit18 training-done (polls ff00ab00, ff00b9e8);
        +0x3cc done flag, polled once;
        +0xC84 mirrors +0xC80 bit0 (doorbell helper ff00501c);
        +0x514 toggle status: init 1, flips on every +0x510 write whose
               value has bit0 clear, per engine — the blob polls it for
               both polarities (ff00baac waits 1, ff0059b0 waits 0,
               ff005a34 waits 1); plain mirrors/parity models fail at least
               one of the three, this satisfies all.
    - Other stub windows unchanged from P4/P8 bring-up (CRU PLL-lock hooks,
      PMU 0x4000=0x3588, eDP DET/HPD/PLL, OTP RD_DONE, RNG RAND_RDY,
      stimer, ddr-cfg1/cfg2, fd80-cru, grf-sec widened).
    - Benign blob console noise: "unknown device / error / ERR" (no SPD
      present in the model); training proceeds regardless.  "DDR
      9fffbe1e78 cym ... fwver: v1.16" banner confirms the stock blob ran.
    - Debug stack used during bring-up (all temp prints since removed):
      `timeout 40 ... -d exec -D /tmp/p8exec.log` + address histogram
      (grep -oE '\[[0-9]+/[0-9a-f]+' | cut -d/ -f2 | sort | uniq -c |
      sort -rn); exceptions `-d int`; gdb-multiarch -batch -x script.gdb
      (must be a script file; plain gdb has no aarch64 target — see
      README host-package table, gdb-multiarch row).
P9  (user request 2026-10-07; ordered after P4, labels kept) Basic VOP2
    framebuffer through ramfb: VOP2 scanout rendered into a ramfb-style display
    surface (simpler than the full analogix eDP AUX/DPCD/EDID model of P6; can
    come before it).
    STATUS: DONE (2026-10-08). Exit command:
      $ ./p9-exit.sh
    Exit output:
      qemu-system-aarch64: rk3588: v2 image, ddr@8800+12e00 os@1b600+28000
      qemu rc=0 (0 = guest PSCI shutdown), monitor rc=0
      --- serial log ---
      DDR 9fffbe1e78 cym 24/02/04-10:09:20,fwver: v1.16
      unknown device
      If no 'may be ch* soldering abnormality' is printed, it may be ch0 soldering abnormality
      error
      ERR
      [UART] Powering up pd 15 (0b10101)
      [UART] Powering up pd 1c (0b11100)
      [UART] Powering up pd 11 (0b10001)
      [UART] Powering up pd 13 (0b10011)
      [UART] VOP video is active
      [UART] edk2_hdptx_phy_init
      [UART] RockchipHdptxPhyInit
      [UART] RockchipHdptxPhyReset
      [UART] RockchipHdptxPhyDpPllInit
      [UART] RockchipHdptxPhyDpAuxInit
      [UART] edk2_hdptx_phy_init exit
      [UART] edk2_hdptx_phy_configure_edp
      [UART] RockchipHdptxPhySetRate
      [UART] RockchipHdptxPhySetVoltage
      [UART] RockchipHdptxPhySetVoltage
      [UART] edk2_hdptx_phy_configure_edp exit
      [UART] HPD status OK: 40 (0b1000000)
      [UART] PLL is locked
      [UART] Checking clock...
      [UART] Waiting on STA: 0 (0b0)
      [UART] Waiting for video to turn on... 40 (0b1000000)
      [UART] enabled video output
      [UART] Calling a00000 (0b101000000000000000000000)
      [UART] Dropping down into EL2
      Hello World from Payload
      FUTO Bootloader payload binary, running on 'Genbook'
      We are in EL2
      Valid DTB present
      Memory description map:
      Range: 0x0-0x6950
      Range: 0x6950-0x10000000
      Range: 0x10000000-0xBFFF0000
      Range: 0xC0000000-0x100000000
      Turning off...
      --- screendump check ---
      screendump 1920x1080, distinct colors: 2
        #4398d7 x2063372
        #ffffff x10228
      bg(4398d7) px=2063372, white px=10228
      P9 OK: VOP2 scanout rendered; guest PSCI_SYSTEM_OFF exited QEMU (rc=0)
      P9_SCRIPT_RC=0
    Design (src/rk3588-display.c + .h, added to qemu/hw/arm/meson.build —
    qemu.patch regenerated via `git -C qemu diff -- hw/arm/Kconfig
    hw/arm/meson.build > qemu.patch`):
    - rk3588_display_init() creates one graphic console
      (qemu_graphic_console_create(NULL, 0, &ops, s)) at machine init.
    - GraphicHwOps.gfx_update = rk_display_gfx_update: QEMU calls it before
      every UI redraw AND from qemu_console_co_wait_update() inside the
      screendump monitor command, so no refresh timer is needed (works with
      -display none and with a real window backend).
    - Each update reads the ESmart2 layer regs the firmware programs
      (rk/src/rk3588/vop2.c config_smart_layer, base 0xfdd90000+0x1c00):
      region0_mst_ctl(+0x10) enable bit, region0_mst_yrgb(+0x14) fb addr,
      region0_vir(+0x1c) stride in pixels, region0_act_info(+0x24)
      (h-1)<<16|(w-1).  When enabled, maps guest RAM at fb addr with
      physical_memory_map and builds a qemu_create_displaysurface_from
      surface (PIXMAN_x8r8g8b8 == guest XRGB8888 LE) exactly like
      hw/display/ramfb.c; surface destroy unmaps.  Surface is rebuilt only
      when addr/geometry change, else just qemu_console_update_full.
    - The demo framebuffer is 0xd0000000 1920x1080 (plat_get_framebuffer);
      demo/bmp.c draws 0x4398d7 background + white font into it, which the
      screendump check verifies pixel-for-pixel.
    - p9-exit.sh: boots with -display none, -serial file:/tmp/p9s.log,
      -monitor unix:socket (connect with retry), -D qemu-p9.log; waits for
      the payload's last draw ("Memory description map:") + 0.05s, sends HMP
      `screendump /tmp/p9.ppm` and does NOT quit — the guest's
      PSCI_SYSTEM_OFF shuts QEMU down; asserts qemu rc=0 (guest shutdown),
      monitor rc=0, and the PPM is 1920x1080 with bg #4398d7 + white pixels
      present.  qemu-p9.log empty (0 unimp/guest_errors/invalid_mem).
    - Regression: canonical P8 command (-nographic -serial mon:stdio) still
      runs the full boot and now self-exits rc=0 after "Turning off...".
      A real -display gtk/sdl window uses the same console/surface (not
      exercised here: headless container).
P5  Linux: custom DTS matching machine (upstream rk3588 dts, unmodeled hw disabled),
    kernel + busybox initramfs staged by QEMU loader, u-boot booti -> userspace prompt.
    Exit: init/shell on serial. (storage modeling deferred)
    STATUS 2026-10-10 (user-requested session "get linux working"; kernel choice
    v7.2.9 stable + arm64 defconfig, busybox initramfs, L1 milestone plan as
    scoped in-session):
    - Kernel: linux-7.2.9.tar.xz from cdn.kernel.org (v7.x dir), built
      `make defconfig ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j24 Image`
      (~3 min, tree at linux/linux-7.2.9). Container reset had wiped host
      packages: needed `apt-get update && apt-get install -y flex bison
      libssl-dev libelf-dev bc rsync dwarves cpio` (+ libgnutls28-dev for the
      QEMU build, gdb-multiarch, libc6-dev-arm64-cross for static userspace).
    - dts/rk3588-qemu.dts (NEW): minimal model DT — 1 cpu (cortex-a76, psci),
      arm,gic-v3 @ fe600000/fe680000 (#interrupt-cells 3, no ITS — model has
      none), armv8-timer (PPI 13/14/11/10 LEVEL_LOW, virt-style), uart2
      serial@feb50000 (snps,dw-apb-uart, reg-shift 2, reg-io-width 4, TWO
      xin24m fixed-clock phandles "baudclk","apb_pclk" — 8250_dw has NO
      clock-frequency fallback and fails probe without clocks), memory
      0x0-0xf0000000, /chosen earlycon console=ttyS0,1500000 nokaslr.
      Built with the kernel's scripts/dtc/dtc, padded to 128 KiB for booti's
      in-place fdt fixups -> images/rk3588-qemu.dtb.
    - initramfs/ (NEW): build.sh assembles images/initramfs.bin — busybox
      1.37 (aarch64 static .deb direct from deb.debian.org pool — the HOST
      busybox-static is x86-64, unusable; apt cache has no debs either),
      static demo.c (uname/cpuinfo/meminfo/interrupts/file roundtrip/fork/
      nanosleep battery, prints LINUX-OK), /init mounts proc/sys/devtmpfs,
      runs /bin/demo, execs /bin/sh. Device nodes via the kernel's
      usr/gen_init_cpio (gcc'd standalone): "nod /dev/console c 5 1" and
      /dev/null — the container forbids mknod, and the kernel needs
      /dev/console BEFORE /init (devtmpfs mounts too late). Padded to a
      constant 4 MiB so the booti initrd size operand stays a fixed 0x400000.
    - u-boot: .config (via scripts/config + olddefconfig, NOT defconfig —
      tree has hand edits like CONFIG_SCMI_FIRMWARE=n): BOOTCOMMAND
      "booti <kaddr> 0x09000000:0x400000 0x0a100000", rebuilt u-boot.bin.
      Verified the bare regress still reaches "=>" (booti on zeros ->
      "Bad Linux ARM64 Image magic!" -> prompt, invalid=0).
    - LESSON (cost ~2h of debugging): the arm64 boot protocol wants the
      Image at a 2 MiB-aligned address. Staged at 0x02080000 (the rockchip
      kernel_addr_r convention), booti prints "Moving Image from 0x2080000
      to 0x2200000" (lmb relocation +0x180000, overlapping memmove) and
      jumps there — kernel then died in jump_label_init+0xa4 reading a
      garbage key offset, and RAM dumps showed the relocated image's jump
      table area overwritten with mixed kernel-rodata/u-boot ("Pi CM")
      content (sparse: kernel+0x1E6D6B0-0x1E73BDF, +0x2003080-0x2004200,
      +0x200B570-0x200C580 — root cause not fully isolated; the watchpoint
      hunt was too slow under TCG and was abandoned per user request).
      Fix: stage the Image 2 MiB-aligned at 0x02000000 (bootcmd updated) —
      no relocation, kernel boots clean. Boot artifacts and logs:
      /tmp/opencode/linux13.txt (last boot), logs/p5-linux-boot-2026-10-10.txt.
    - RESULTS (kernel at 0x02000000, linux13 run): FULL early boot works —
      earlycon uart0@feb50000, "Linux version 7.2.9", "KASLR disabled",
      Machine model: QEMU rk3588 model, efi not found (fine), PSCI v1.1
      via SMC (TF-A), GICv3 CPU interface, memory 0x200000-0xefffffff
      (U-Boot's booti memory fixup -> 3.7G single bank), initramfs
      unpacked, "Run /init as init process" — /init runs as PID 1 (sh).
      invalid_mem/guest_errors: 0. Kernel boot to init handoff in ~1 s.
    - RESOLVED 2026-10-10 (2nd session of P5) — P5 EXIT MET.  Three model
      bugs fixed, found by reading the guest driver source rather than
      slow gdb probing:
      * IRQ wiring (the planned L2 work): rk3588_create_uarts(gic) now
        sysbus_connect_irq()s each DW UART to its real GIC SPI — uart0=331,
        uart2=333 (rk3588-base.dtsi; gicv3 gpio-in line n IS SPI n, since
        arm_gicv3_common maps [0..num_irq-32) to SPIs, PPIs behind them).
        dts/rk3588-qemu.dts uart2 gains "interrupts = <0 333 4>" (3-cell:
        GIC_SPI=0, 333, LEVEL_HIGH=4).  This fixed the first probe
        failure ("dw-apb-uart feb50000.serial: error -ENXIO: IRQ index 0
        not found" — the 8250_dw driver refuses to probe without a DT
        interrupt, so L2-style IRQ wiring was REQUIRED, not optional).
      * MSR loopback: the Linux autoconfig() existence test writes
        MCR=LOOP|OUT2|RTS and requires MSR&0xF0 == CTS|DCD — the model
        read MSR as 0, so the port classified PORT_UNKNOWN and
        uart_configure_port() skipped register_console (the log clue:
        "[ttyS0] disabled" with no "enabled" — uart_remove_one_port of
        the replaced ISA slot prints unconditionally).  dw-apb-uart.c now
        models MCR loopback routing (RTS->CTS, DTR->DSR, OUT1->RI,
        OUT2->DCD), reports DCD|DSR|CTS outside loopback so the console
        open doesn't block on carrier, and keeps the W1C delta bits.
      * chardev flow control: with `-serial mon:stdio` the mux chardev
        buffers input in a 32-byte ring that only drains via
        qemu_chr_fe_accept_input(); the model never called it, so
        interactive input stalled after a few 16-byte FIFO loads.
        Added after every RX free (rx_pop, FCR RX reset, SRR), matching
        hw/char/serial.c.
      * IER mask: the model stored the full IER byte, so autoconfig's
        Xscale probe (IER[6] UUE write/readback) classified the port
        PORT_XSCALE ("is a XScale"); real DW IER is bits [4:0] and
        reserved bits read zero — masked writes to 0x1f, and the port
        now logs "is a 16550A" like the real SoC.
    - RESULTS: `make run-linux` -> "printk: legacy console [ttyS0]
      enabled", bootconsole handover, full /bin/demo battery PASS
      (uname, cpuinfo, meminfo, /proc/interrupts shows GICv3 arch_timer
      + ttyS0, file write/fsync/read-verify, fork/waitpid, nanosleep,
      statfs), "LINUX-OK", ash shell on serial.  Interactive commands
      verified over the mux chardev (echo/uname/proc reads/exit; exit
      -> init PID-1 panic, as designed).  `make regress` green
      (U-Boot prompt=1 invalid=0, EDK2 banner=2 invalid=0); AppImage
      rebuilt + extract-run smoke green (container has no /dev/fuse:
      smoke = AppRun --appimage-extract, then run AppRun from /workspace
      with the usual relative -bios paths).  Logs:
      logs/p5-linux-boot-2026-10-10.txt (full boot + demo + LINUX-OK),
      logs/p5-linux-shell-2026-10-10.txt (interactive session).
    - Misc noted: TF-A rk3588 PSCI SYSTEM_OFF/SYSTEM_RESET poke unmodelled
      CRU/PMU regs, so Linux reboot/poweroff will not exit QEMU (the P9
      0xfd58affc 'OFF!' shutdown hook belongs to the rk EL3 payload, not
      TF-A). Kernel single-CPU per DTS; SMP deferred (TF-A rk3588
      rockchip_soc_cores_pwr_dm_on pokes unmodelled PMU; model-side
      cpuson/cpus_power_domain_on emulation would be needed).  busybox
      ash line-editing emits a DSR "\e[6n" cursor query at each prompt
      (harmless with a pipe; a real terminal answers it).
    - Makefile: new `run-linux` target (single command for the L1 boot).
P6  Display: VOP2 scanout + analogix eDP AUX/DPCD/EDID model -> window in QEMU,
    upstream analogix_dp driver link-trains (Cool-Pi Genbook is the driver test case);
    Orange Pi 5 HDMI path after that. RESOLVED 2026-10-10 as the OPi5
    HDMI chain instead (user directive; the analogix eDP sub-goal was
    superseded — eDP stays at the P2 status-model level).
    STATUS 2026-10-10, DONE — see EXIT EVIDENCE at the end of this
    entry. (User directive: "keep working towards
    [booting] the unmodified Orange Pi 5 Armbian image; next: hdmi" — so
    P6's target moved to the OPi5 HDMI chain, mainline kernel 7.2.9
    in-tree DRM stack: VOP2 -> DW HDMI QP -> Samsung HDPTX PHY. The P9
    ramfb scanout already solved pixels; this milestone makes the
    UNMODIFIED rockchip_drm_vop2 + dw-hdmi-qp + samsung-hdptx drivers
    probe and drive fbcon. All facts below were extracted from the full
    driver sources (dw-hdmi-qp.c 1394 L, dw_hdmi_qp-rockchip.c 669 L,
    rockchip_drm_vop2.c + rockchip_vop2_reg.c, phy-rockchip-samsung-
    hdptx.c 2606 L) — re-read them if anything here conflicts.

    DT GRAPH (rk3588-base.dtsi, OPi5 dtsi: rk3588s-orangepi-5.dtsi):
      vop@fdd90000 --endpoint reg=ROCKCHIP_VOP2_EP_HDMI0(=2)--> hdmi0@fde80000
      hdmi0 --port@1--> hdmi0-con ("hdmi-connector", type "a")
      hdmi0: phys = <&hdptxphy0@fed60000>, rockchip,grf = sys_grf@fd58c000,
             rockchip,vo-grf = vo1_grf@fd5a8000
      vop: rockchip,grf/vop-grf/vo1-grf/pmu = sys_grf/vop_grf@fd5a4000/
           vo1_grf/pmu@fd8d0000 (ALL FOUR syscons mandatory at bind —
           feature flags HAS_SYS_GRF|VOP_GRF|VO1_GRF|SYS_PMU), clocks aclk/
           hclk/dclk_vp0..dclk_vp3 mandatory (pclk_vop + pll_hdmiphy0/1
           optional — OMIT pll_hdmiphy0 so the driver skips dclk parent
           switching), IRQ GIC_SPI 156, regs 0x4200 + gamma-lut 0xfdd95000.
      display-subsystem { compatible = "rockchip,display-subsystem";
                          ports = <&vop_out>; } — missing "ports" = bind fail.
      Encoder id: dw_hdmi_qp-rockchip bind calls
      rockchip_drm_encoder_set_crtc_endpoint_id(np, port=0, reg=0) →
      remote endpoint reg (2 = HDMI0) → rk3588_set_intf_mux switches on it.

    VOP2 (rockchip_drm_vop2.c) MODEL REQUIREMENTS:
      - Probe does NO MMIO. First MMIO at first modeset (vop2_enable):
        VERSION_INFO 0x004 must read 0x40176786 (VOP_VERSION_RK3588;
        mismatch only logs), then RMWs on SYS_PD_CTRL 0x034 (clear bits
        0-3,7), reads OVL_LAYER_SEL 0x604 / OVL_PORT_SEL 0x608 seeds,
        REG_CFG_DONE 0x000 write 0x8000, SYS_AUTO_GATING 0x008 bit31,
        SYS0/SYS1 bus-error int enable 0x80/0x84 + 0x90/0x94.
      - REG_CFG_DONE 0x000: write-mask register — bits[19:16] gate
        writes to per-VP done bits [3:0]; bit15 global enable; HW must
        SELF-CLEAR the per-VP bits at frame start (ISR sends page-flip
        events only when BIT(vp->id) has dropped; setup_layer_mixer
        also reads it pre-write).
      - Per-VP int block 0xA0+vp*0x10: EN (write-mask over low16),
        CLR 0xA4+ (masked W1C: driver writes irqs<<16|irqs), STATUS
        0xA8+ (RO). VP_INT_FS_FIELD = bit5. The driver enables FS_FIELD
        on every vblank_get; drm_atomic_helper_wait_for_vblanks
        (commit_tail_rpm) waits up to 1s for the vblank count — WITHOUT
        a frame-start IRQ each fbcon commit stalls 1s + WARN. The ISR
        runs pm_runtime_get_if_in_use first (returns IRQ_NONE when the
        CRTC is off) — model gate: VP DSP_CTRL (0xc00+vp*0x100) bit31
        STANDBY clear = active.
      - regmap: max_register 0x3000; VP/window ranges CACHED (0xc00-0xfff,
        0x1000-0x23ff, cache dropped on CRTC disable) — plain storage
        satisfies both, but state must persist across standby (no reset
        on disable).
      - Windows (fbcon = primary plane = Cluster0-win0, the first
        DRM_PLANE_TYPE_PRIMARY entry in rk3588_vop_win_data; linear
        XRGB8888 = VOP2_FMT_ARGB8888 = format field 0):
          Cluster0 0x1000: WIN_CTRL0 0x1000 (enable bit0, format [5:1],
            rb_swap bit14), YRGB_MST 0x1010, WIN_VIR 0x1018 (yrgb stride
            in 4-BYTE WORDS [15:0]), ACT_INFO 0x1020 ((h-1)<<16|(w-1)),
            DSP_INFO 0x1024, DSP_ST 0x1028, CLUSTER_CTRL 0x1100 bit0.
          Cluster1/2/3 at 0x1200/0x1400/0x1600; Esmart0/1/2/3 at
            0x1800/0x1a00/0x1c00/0x1e00 (REGION0_* +0x10 ctl, +0x14 mst,
            +0x1c vir, +0x20 act) — P9's firmware path = Esmart2.
      - GRF writes (HDMI0 case, rk3588_set_intf_mux): vop_grf+0x08 =
        0x00020002; vo1_grf+0x00 = (0x60<<16)|(pol<<5). Storage fine.
      - dclk = fixed clock OK: clk_set_rate return ignored, mode_valid
        never checks clocks. NO iommus property → no vop_mmu → GEM via
        dma_alloc → yrgb_mst is guest-physical (scanout shim maps it
        directly). NO power-domains (driver has no dev_pm_ops; runtime
        PM is a no-op only if the DT omits power-domains).
      - gamma-lut window 0xfdd95000/0x1000: plain storage (written only
        when userspace installs a LUT; LUT_PORT_SEL 0x058).

    HDMI0 TX (dw-hdmi-qp.c + dw_hdmi_qp-rockchip.c) MODEL REQUIREMENTS:
      - Glue requires MMIO base to be EXACTLY 0xfde80000 (port_ids match,
        port_id=0; 0xfdea0000 = port1). Reg 0x20000, driver regmap
        max_register 0x482c. IRQs "main"/"cec"/"hpd" fetched
        unconditionally (avp/earc never looked up): real SPIs avp=169,
        cec=170, earc=171, main=172, hpd=360.
      - NO probe-time MMIO reads: no ID/version check — an all-zero
        register file probes fine. Probe writes (init_hw): 0x3014=0,
        0x3024=0 (mute MAINUNIT ints), TIMER_BASE_CONFIG0 0x80 =
        ref_clk_rate (fixed clock rate 0 → vendor fallback 428571429 +
        warning; harmless), I2CM_CONTROL0 0xec=1 (sw reset),
        I2CM_FM_SCL_CONFIG0 0xe4=0x085c085c (DDC timing is NOT derived
        from ref clock), 0xf4 clear FM_EN, MAINUNIT_1_INT_CLEAR
        0x3028=0x5.
      - DDC/EDID = internal I2CM master, regs 0xe0-0x118: transfer
        STARTS when a nonzero command is written into
        I2CM_INTERFACE_CONTROL0 0xf4: cmd bits[4:1] (FM_WRITE=0x2,
        FM_READ=0x4, SHORT_READ=0x8, EXT_READ=0x10), slave bits[11:5],
        DDC word offset bits[27:12]; data 0xfc (wr) / 0x10c (rd);
        segment 0xf8 (SEG_ADDR[7:0]=0x30, SEG_PTR[14:7] — programmed
        by an off-wire segment message; next read uses EXT_READ with
        index seg*256+off). Completion MUST come through the "main"
        IRQ: MAINUNIT_1_INT_STATUS 0x3020 (bit0 OP_DONE, bit1
        READ_REQUEST, bit2 NACK_RCVD), MASK_N 0x3024 (1 = unmasked),
        CLEAR 0x3028 (W1C); line = OR(status & mask_n). Without the
        IRQ every EDID byte times out at 100ms. DDC slave 0x50 serves
        the EDID; other slaves → NACK bit. Model EDID: 128-byte base
        block, "QEMU rk3588", 1920x1080@60 native DTD (CEA VIC16
        timing; DTD byte layout verified against drm_mode_detailed,
        drm_edid.c:3528-3535: pixclk 14850 = 0x3A02 LE, hact/blank
        80 07 / 18 01 / 71, vact/blank 38 04 / 2d 00 / 40, hfp 0x58,
        hsync 0x2c, byte10 0x45 (VFP<<4|VSW), misc 0x1E digital
        separate +ve syncs; checksum 0x19) — embedded in
        src/rk3588-hdmi.c.
      - HPD does NOT involve this MMIO window: glue reads level from
        sys_grf SOC_STATUS1 +0x384 bit16 (port0; bit24 port1); GRF
        SOC_CON2 +0x308 (HIWORD: INT_MSK bit13, INT_CLR bit12), SOC_CON7
        +0x31c io enables, vo1_grf CON3 +0x0c (color depth [7:4]/
        format [3:0]) + CON9 +0x24 (grant sel) — all HIWORD storage.
        Model: 0x384 bit16 permanently 1 (connected); hpd IRQ never
        asserted (sink is plugged before power-on).
      - SCDC completely unused by this driver (TMDS capped at 340MHz by
        dw_hdmi_qp_bridge_tmds_char_rate_valid); NO video-timing regs
        programmed at all (video comes from the VOP2 via AXI); enable
        path writes = storage: HDCP2LOGIC_CONFIG0 0x8e0 (bit0 bypass),
        LINK_CONFIG0 0x968 (bit4 OPMODE_DVI), PKTSCHED_PKT_EN 0xaa8 +
        PKTSCHED_PKT_CONFIG1 0xa9c, infoframe content regs
        AVI 0xbe0+/VSI 0xbc0+/SPD 0xc00+/AUDI 0xc20+/DRMI 0xc60+.
        CEC block 0x1000-0x1054 + CEC_INT 0x4000/0x4004/0x4008/0x400c
        (CONFIG_DRM_DW_HDMI_QP_CEC stays off; "cec" IRQ must still be
        in the DT because the glue fetches it unconditionally).
      - clocks: devm_clk_bulk_get_all_enabled (rates never used); "ref"
        required by name (rate only lands in 0x80). resets in the
        binding are NEVER fetched by this driver.
      - PHY: devm_of_phy_get_by_index(0); never phy_init/exit — only
        phy_power_on/off (power_on FIRST in atomic_enable, before the
        controller writes) + phy_configure(tmds_char_rate, bpc) in
        encoder_atomic_check. FRL unsupported (frl-enable-gpios driven
        low unconditionally — optional, omitted in our DT).

    HDPTX PHY (phy-rockchip-samsung-hdptx.c) MODEL REQUIREMENTS:
      - phy@fed60000/0x2000; base must be EXACTLY 0xfed60000
        (phy_ids match, phy_id=0). 4 named resets "apb","init","cmn",
        "lane" REQUIRED (bulk exclusive; "phy","ropll","lcpll" never
        touched) → DTS uses a reset-simple provider,
        "snps,dw-high-reset" @0xfd7c0400/0x40 (CRU SOFTRST_CON area;
        the model CRU stub's hiword semantics make reset-simple's RMW
        writes no-ops, reads return stored 0 — harmless). Clock "ref"
        required by name (driver hardcodes 24MHz PLL_REF_CLK; any
        existing clock works).
      - GRF hdptxphy0_grf@fd5e0000/0x100: CON0 @0x00 (HIWORD storage:
        MODE_SEL bit0 = 0 HDMI/1 DP, BGR bit5, BIAS bit6, PLL_EN bit7,
        LC_REF_CLK_SEL bit11), STATUS @0x80 RO (bit3 PLL_LOCK_DONE,
        bit2 PHY_CLK_RDY, bit1 PHY_RDY, bit0 SB_RDY/DP).
      - NO IRQs anywhere — everything is polled. TMDS ≤600MHz
        (HDMI20_MAX_RATE) always uses ROPLL (LCPLL only for FRL rates;
        148.5MHz hits the table entry mdiv=123 mdafc=123 pdiv=1
        refdiv=1 sdiv=3 sdm...). Polls (must pass on the FIRST read —
        budgets 400us/5ms): (1) GRF 0x80 bit2 "PHY clk ready" after
        post_enable_pll (GRF CON0 = BIAS|BGR then PLL_EN; 0x00600060,
        0x00800080); (2) GRF 0x80 bits1+3 "PHY lane lock" after
        post_enable_lane (GRF 0x00600060 + PHY LNTOP_REG(0207) @0x81c
        = 0x0f 4-lane enable). MODEL: STATUS bits3+2 follow CON0.PLL_EN;
        bit1 additionally needs the 0x81c write != 0; consumer_put's
        disable sequence only runs when bit3 reads set (else skipped).
      - PHY window itself = plain storage: the CMN init sequences
        (~200 writes, rk_hdptx_common_cmn_init_seq + tmds seqs, rate
        words at CMN 0x144/0x154/0x164/0x168/0x178/0x180/0x190/0x194/
        0x1a4/0x1b0/0x1c0/0x218 + branch selector CMN 0x20 bit4) MUST
        persist because the clock recalc_rate reads them back and
        computes fout = 24MHz*mdiv + SDM, rate = fout*16/(sdiv*10*bpc).
      - LNTOP 0x800=0x06 (HDMI+TMDS), 0x818=0x07; ≤340MHz uses the
        low-br lntop seq (0x804=0x07, 0x808=0xc1, 0x80c=0xf0, 0x810=
        0x7c, 0x814=0x1f); lane seqs per lane 0xc00+0x400*n (lane3
        skew differs: 0x1878=0x0a vs 0x02); disable writes 0x81c=0 etc.
      - Probe: NO PLL lock needed; probe-end deasserts apb/cmn/init;
        registers clock provider "clk_hdmiphy_pixel0" (#clock-cells=0)
        whose .prepare would run a full PLL re-program — nobody uses it
        because the VOP2 node omits pll_hdmiphy0.

    DTS DECISIONS (dts/rk3588-qemu.dts, raw cells: GIC_SPI=0,
    LEVEL_HIGH=4, EP ids from dt-bindings/soc/rockchip,vop2.h):
    every clock = <&xin24m> (model never gates; rates unused by all
    three drivers); syscons as bare compatible="syscon" on the real
    GRF windows; NO power-domains, NO iommus, NO pinctrl (HPD via
    GRF; frl-enable-gpios optional); hdmi0-con = "hdmi-connector"
    type "a" (needs CONFIG_DRM_DISPLAY_CONNECTOR=y).

    KERNEL (built this session): defconfig + scripts/config -e:
    DRM, DRM_ROCKCHIP, ROCKCHIP_VOP2, ROCKCHIP_DW_HDMI_QP (selects
    DRM_DW_HDMI_QP + DRM_BRIDGE_CONNECTOR), DRM_DISPLAY_CONNECTOR,
    PHY_ROCKCHIP_SAMSUNG_HDPTX — all =y (initramfs boots cannot load
    modules). RESET_SIMPLE was already =y; FRAMEBUFFER_CONSOLE,
    DRM_FBDEV_EMULATION, LOGO (penguin!) already =y in defconfig.
    Image rebuilt clean.

    IMPLEMENTATION STATE (all written this session, NOT yet compiled
    green — see BUILD FIXES):
      - src/rk3588-hdmi.c/.h (NEW): dw-hdmi-qp model — 0x20000 window,
        5 sysbus IRQs, storage regs ≤0x4830, instant DDC engine
        (serves the fake EDID at 0x50, NACKs others, completes with
        OP_DONE/NACK + main-IRQ assert), MAINUNIT_1 mask_n/W1C semantics.
      - src/rk3588-vop2.c/.h (NEW): VOP2 model — 0x4200 window,
        VERSION_INFO hook, REG_CFG_DONE write-mask, per-VP int block,
        60Hz frame timer (FS_FIELD raise gated on STANDBY-clear +
        FS-enable; cfg-done bit self-clear; UI push) on IRQ 156.
      - src/rk3588-stubs.c: sys-grf read hook (+0x384 bit16 connected);
        hdptxphy0-grf CON0-recording write hook + computed STATUS 0x80
        read hook; edp0tx write hook records LNTOP 0x81c; new plain
        stub "vop2-gamma" 0xfdd95000/0x1000; REMOVED the "hdmi0" and
        "vop2" plain stub entries (now real devices).
      - src/rk3588-display.c: rk_display_get_window() — kernel fbcon
        Cluster0 window first (XRGB8888 format 0, no rb_swap), P9
        ESmart2 firmware window fallback.
      - src/rk3588.c: rk3588_create_display_devices(gic) after
        rk3588_create_uarts (VOP2 + SPI 156; HDMI + SPIs 169/170/171/
        172/360).
      - dts/rk3588-qemu.dts: full display pipeline added (see above);
        images/rk3588-qemu.dtb rebuilt (make linux-dtb clean).
      - qemu.patch regenerated (meson line += rk3588-vop2.c,
        rk3588-hdmi.c; edited in qemu/hw/arm/meson.build then
        `git -C qemu diff -- hw/arm/Kconfig hw/arm/meson.build >
        qemu.patch`).

    BUILD FIXES APPLIED 2026-10-10 (this QEMU tree's API):
      1. class_init signature `static void foo(ObjectClass *oc,
         const void *data)` in both device files.
      2. reset via `ResettableClass *rc = RESETTABLE_CLASS(oc);
         rc->phases.hold = foo_reset_hold` (Object *obj, ResetType
         type), pattern hw/char/cadence_uart.c:626-633.
      3. rk3588-vop2.c includes "qemu/timer.h"; the frame tick calls
         the new rk3588_display_repaint() (rk3588-display.c singleton +
         direct gfx_update call) because this tree's
         qemu_console_hw_update(NULL) is a no-op.
      4. rk3588-hdmi.c I2CM command decode FIXED: the command field is
         the RAW register bits [4:1] (I2CM_WR_MASK 0x1e) — FM_WRITE
         0x2, FM_READ 0x4, SHORT_READ 0x8, EXT_READ 0x10 — written via
         regmap RMW; the previous shift-by-one decode never matched.
         NACK path sets NACK|OP_DONE (hardware completes on both).
      5. rk3588-stubs.c hdptx GRF CON0 write hook FIXED to accumulate
         hiword write-mask bits: post_enable_lane re-writes BIAS|BGR
         after PLL_EN was set, so overwrite semantics dropped PLL_EN
         right before the lane-lock poll (5 ms timeout -> power_on
         failure).  con0 = (con0 & ~mask) | (value & mask).
      6. EDID array in rk3588-hdmi.c REGENERATED: the hand-written one
         had a shifted layout (byte 18 = 0x80 as EDID version) and an
         invalid checksum.  /tmp/opencode/gen-edid.py builds a proper
         EDID 1.3 (digital input 0x80 — REQUIRED, else
         update_display_info() drops RGB444 -> "HDMI Sink doesn't
         support RGB"; features 0x06 preferred-timing bit so DTD 1
         becomes the preferred mode; DTD byte 10 = 0x45 = vfront<<4 |
         vsw per drm_mode_detailed; misc 0x1E separate+positive syncs;
         name "QEMU rk3588" + range-limit descriptors; checksum 0xcc
         — note: a byte-exact array is pasted from edid.bin BY
         SCRIPT, manual reformatting once dropped two 0x01 filler
         bytes and cost a whole debug round; sums to 0 mod 256).
         DDC serving itself was byte-perfect all along (kernel
         "remainder" == 0x100 - sum(bytes 0..126) matched the model
         array exactly on the first boot).

    ENVIRONMENT (container reset this session): apt packages wiped —
    `apt-get update && apt-get install -y python3-venv flex bison
    libssl-dev libelf-dev bc rsync dwarves cpio libgnutls28-dev` is
    now required before `make qemu` (python3-venv was missing and
    QEMU's configure died with "ERROR: python venv creation failed" —
    add it to the README host-package table).

    EXIT EVIDENCE 2026-10-10 (all green, milestone closed):
      - `make run-linux` serial (full log:
        logs/p6-linux-boot-2026-10-10.txt):
          "rockchip-drm display-subsystem: bound fdd90000.vop (ops
           vop2_component_ops)"
          "dwhdmiqp-rockchip fde80000.hdmi: registered DesignWare HDMI
           QP I2C bus driver"   <- EDID served over the modeled DDC
          "rockchip-drm display-subsystem: bound fde80000.hdmi"
          "Console: switching to colour frame buffer device 240x67"
           <- 240x67 text grid = NATIVE 1920x1080 from the EDID DTD
          "fb0: rockchipdrmfb frame buffer device"
          "LINUX-OK"
        No EDID checksum errors, no "Sink doesn't support RGB", no
        vblank/flip timeouts (boot is FASTER than P5's: /init at
        1.09 s — no 1 s commit stalls => the 60 Hz VOP2 FS interrupt
        plumbing works), -d invalid_mem,guest_errors log clean.
      - Pixel evidence (screendump via -monitor unix + `screendump`,
        gz PPM: logs/p6-hdmi-screendump-2026-10-10.ppm.gz): 1920x1080
        P6; the Linux Tux logo (LOGO=y) renders at takeover, and the
        P6 demo additions (initramfs/demo.c) prove both write paths:
          [PASS] open/mmap /dev/fb0 -> demo fills 4 pure bands:
          #00ff00/#0000ff/#ffffff 518400 px each (270*1920 exact),
          #ff0000 513536 (band minus the text-line area);
          [PASS] open/write /dev/tty0 -> fbcon glyphs render:
          #aaaaaa (fbcon foreground) 994 stroke pixels; red band
          deficit 4864 = glyph+background pixels of the test line.
      - `make regress`: U-Boot prompt=1 invalid=0; EDK2 banner=2
        invalid=0 (EDK2's ESmart2 programming now goes through the
        VOP2 device's plain-storage window offsets — unaffected).
      - `make appimage` + smoke: U-Boot prompt (1 x "=>") from
        squashfs-root/AppRun.
      - Model files: src/rk3588-vop2.c (VERSION_INFO 0x40176786,
        write-masked REG_CFG_DONE with frame-start self-clear, VP int
        blocks, 60 Hz FS timer gated on STANDBY-clear), src/rk3588-hdmi.c
        (I2CM DDC engine + valid 1080p EDID + MAINUNIT_1/main-IRQ
        completion), rk3588-stubs.c (sys-grf 0x384 HPD level, hdptx
        GRF CON0 accumulation + STATUS 0x80 model, LNTOP 0x81c
        tracking, vop2-gamma block), rk3588-display.c (Cluster0-first
        scanout + repaint entry), dts/rk3588-qemu.dts (full pipeline).

    Follow-ups noted: fbcon shows the logo + glyph draws but NOT the
    boot printks that raced the fbcon takeover (they scroll only on
    the next VT redraw; tty0 writes after boot DO render) — cosmetic,
    root cause is the fbdev damage worker vs early printk timing, not
    the model.  Remaining display items for the unmodified-Armbian
    goal: nothing in the HDMI path; CEA extension block only if
    audio/HDMI-ness (is_hdmi for infoframes) is ever needed.
P7  EDK2: download orangepi-5_UEFI_Release_v1.1.img, extract BL33 (the FV), decide
    BL31 (our bootloader is EL3) + DTB handoff, boot to EDK2 console/shell.
    STATUS: DONE (2026-10-08). The FV is loaded by QEMU at 0x200000 (BL33 entry via
    -machine bl33-base=0x200000); the image itself is NOT yet booted unmodified
    (see P10). Layout note (corrected 2026-10-08): the image is a GPT disk with one
    partition "uboot" at 0x100000; the FV sits at 0x100a00 inside it, byte-identical
    to edk2/BL33_AP_UEFI.Fv. idbloader is at sector 64 (0x8000), not 0x800.
    Exit command:
      $ timeout 150 build/qemu/qemu-system-aarch64 -M rk3588 -nographic \
            -serial mon:stdio -bios atf/build/rk3588/release/bl31/bl31.elf \
            -device loader,file=edk2/BL33_AP_UEFI.Fv,addr=0x200000,force-raw=on \
            -machine bl33-base=0x200000 -dtb u-boot/dts/dt.dtb \
            -d int,invalid_mem,guest_errors -D <log>
    Output (non-interactive, rc=124 is the expected timeout at the FrontPage):
      UEFI firmware (version v1.1 built at 22:11:54 on Apr  9 2025)
      Orange Pi 5 / Rockchip RK3588S 0.02 GHz v1.1 4096 MB RAM
      <English> Select Language / Device Manager / Boot Manager /
      Boot Maintenance Manager / Continue / Reset
      invalid-access count: 0, synchronous exceptions: 0
    Interactive proof (serial on tcp:127.0.0.1:4444; keys: down=\x1b[B, Enter=\r):
      2x down + Enter -> "Boot Manager Menu" (entries: UEFI Shell, Reset to MaskROM)
      Enter -> "UEFI Interactive Shell v2.2 / EDK II / UEFI v2.70 (EDK II, 0x00010000)"
      ESC (skip startup.nsh) -> Shell> ver -> "UEFI Interactive Shell v2.2"
      Shell> echo SHELL-OK -> SHELL-OK
      invalid-access count in the -D log: 0

P10 (user request 2026-10-08) Boot the UNMODIFIED orangepi-5_UEFI_Release_v1.1.img
    (and later unmodified Armbian images) through the real chain:
      BootROM -> idbloader (TPL + SPL) -> u-boot -> BL33 FIT -> FV (EDK2) -> Shell>
    with the image bytes untouched. All accommodation lives in QEMU (device models,
    bootrom modes, stub regions), as in P7/P8/P9.
    Image facts (decoded and checked against the file, 2026-10-08):
      - File is 0x698600 bytes (6915584). GPT: partition "uboot" = LBA 0x800..0x47ff,
        i.e. bytes 0x100000..0x8fffff. idbloader at 0x8000 (sector 64).
      - idbloader @0x8000: RKNS v2 (struct RkV2Header in src/rk3588.c; images[] at
        header+0x78, stride 88). hash_off 0x180, n_images 2.
          TPL: offset 4 (sectors) -> file 0x8800, size 148 sectors (0x12800 bytes),
               load address unset (0xffff).
          SPL: offset 0x98 -> file 0x1b000, size 448 sectors (0x38000 bytes),
               load address unset (0xffff). Ends at 0x53000.
        SPL DTB (fdt magic) at 0x51ae8, totalsize 0x12f2.
      - FIT at 0x100000 (totalsize 0x971): images edk2, atf-1, atf-2, atf-3, optee.
        FV payload @0x100a00 (5 MiB, byte-identical to edk2/BL33_AP_UEFI.Fv).
        atf-1 is the image's own TF-A; its format (ELF or raw) is still unchecked.
      - Tail DTB (fdt magic) at 0x696600, totalsize 0x1e95: rk3588-evb board with
        mmc@fe2c0000 (sdmmc) and mmc@fe2e0000 (eMMC) nodes.
      - Stock U-Boot SD driver: u-boot/drivers/mmc/dw_mmc.c uses IDMAC (DMA
        descriptors), so the QEMU model must implement IDMAC, not just PIO.
      - Existing SD stub: src/rk3588-stubs.c `rk_stub_dwmshc_read` at 0xfe2c0000
        (card-detect only). Phase 1 removes this stub entry for the real device.
    Decisions (user, 2026-10-08):
      - Phase 2 uses bootrom-lite (src/rk3588.c staging), NOT the real 32 KB bootrom
        binary (arm-docs/rk3588_bootrom.bin).
      - Phase 3 loads atf-1 from the FIT as a raw binary at its load address (our
        ELF-only loader is extended; our bl31.elf is dropped for that run).
      - Exit runs use timeouts of 150-300 s (EDK2 TCG boot takes minutes).
    Phases:
      Step 0  DONE 2026-10-08 (read-only). Results:
        * Stock SPL = "U-Boot SPL 2017.09-g5f53abfa1e-221223" (vendor build, NOT our
          u-boot tree). Boot order from SPL DTB chosen node: mmc@fe2c0000 (SD) first,
          then mmc@fe2e0000 (eMMC), then spi flash.
        * SD node (SPL DTB @0x51ae8): compatible rockchip,rk3588-dw-mshc; reg
          0xfe2c0000 size 0x4000; interrupts SPI 203 (0xcb); fifo-depth 0x100;
          bus-width 4; max-frequency 200 MHz; cd-gpios = gpio0 PA4 (the existing
          card-detect stub). The SD device must report a card present.
        * FIT source: SPL logs "Trying fit image at 0x%lx sector" and looks up the
          GPT partition named "uboot" (sector 2048 = 0x800 = file 0x100000). The FIT
          is therefore found by GPT name, not a fixed raw sector.
        * Risk: the SPL also looks for a GPT "misc" partition (A/B metadata: "No
          misc partition", "No bootable slots found, use lastboot"). Our GPT has only
          "uboot". Whether the SPL proceeds without "misc" is resolved in Phase 1.
        * Fallback path in SPL: "# Reset the board to bootrom #" and "FATAL:
          unexpected command to back_to_bootrom()". Phase 2 must model this, not
          ignore it.
        * FIT (0x100000, standard U-Boot FIT, default conf "orangepi-5"):
            edk2   load 0x200000, size 0x500000, data at 0x100a00 (= edk2/BL33 FV)
            atf-1  load 0x40000,  size 0x22176, data at 0x600a00. RAW ARM64 code
                   (first insn mov x20,x0), not ELF. Entry = load = 0x40000.
            atf-2  size 0 (ignored; "loadables" only).
            atf-3  load 0xff100000 (SRAM1, 0x2000 bytes), data at 0x622c00.
            optee  load 0x8400000, size 0x719a0, data at 0x624c00.
            fdt    "UEFI dtb", data at 0x696600 (= tail DTB, rk3588-evb).
            nvdata load 0x7c0000, data-position 0x6c0000 (beyond file EOF, so it reads
                   back as zeros; matches our EDK2 NV store at 0x7c0000).
        * Image idbloader: TPL at 0x8800 (148 sectors), SPL at 0x1b000 (448 sectors);
          Both entries have address 0xffff (unset), so the load addresses are not
          in the header. The existing bootrom-lite path (src/rk3588.c) assumes
          TPL->0xff001000 and SPL->0; that is unverified for this image (Phase 2).
        * Still open after Step 0: whether the SPL dw-mshc driver uses IDMAC or PIO.
          Expected IDMAC (fifo-depth 0x100 and the u-boot dw_mmc lineage), so Phase 1
          plans IDMAC. Verify in Phase 1 from the SPL's accesses.
        * Phase 3 implication: atf-1 is raw, so the loader must copy it to 0x40000.
          Our existing ELF loader (rk3588_load_elf64) does not apply.
      Phase 1 SD card, controller model. STATUS: DONE 2026-10-08, with the exit
              criterion REVISED (see below).
              Built: src/rk3588-dwmshc.{c,h} (dw-mshc at 0xfe2c0000, sd-bus, PIO
              FIFO at 0x200, IDMAC descriptor walk). Card attached by rk3588.c from
              -drive if=sd. Stub entry "sdmmc" removed from src/rk3588-stubs.c. GPIO0
              PA4 (card detect) now follows the real card (0 = present, no card = 1),
              so the P7 no-card path is unchanged. qemu.patch regenerated (meson line).
              Fixes found by tracing the real EDK2 RkSdmmc driver against the model:
                - sd.c signals insertion only on media-change; read sdbus_get_inserted()
                  at reset (else the driver saw "no card").
                - STATUS must report FIFO fill for PIO reads (FIFO_EMPTY bit 2, count
                  bits 29:17), like u-boot dw_mmc's dwmci_fifo_ready().
                - RXDR/TXDR are write-1-to-clear and re-raised while data remains.
                - SEND_AUTO_STOP completes with ACD (RINTSTS bit 14). CMD12 is sent
                  only for multi-block transfers (a single-block CMD12 leaves the card
                  in standby, which breaks the next CMD17).
              Exit evidence (EDK2 Shell over serial, card = scratch copy of the stock
              image, 16 MiB):
                Shell> map -r
                  BLK0 ... /SD(0x0)
                  BLK1 ... /HD(1,GPT,40B9D0E3-5DE8-4313-9D2E-DCBA1A7D435C)
              The GPT partition node exists, so the driver read the GPT header and
              entries through the controller. -D log: 0 invalid accesses.
              Scratch-copy rules: sd.c refuses readonly=on for an SD card, and the
              size must be a power of 2 (16 MiB). EDK2 issues writes beyond the card,
              so the scratch copy is re-copied from the stock image before each run.
              Regressions (same session): canonical U-Boot run rc=124, "=>" present,
              0 invalid; P7 no-card EDK2 run rc=124, FrontPage, 0 invalid.
              NOT yet exercised: the IDMAC path (EDK2 used PIO; CTRL.IDMAC_EN never
              set). Stock SPL uses the same dw-mshc, so its IDMAC/PIO choice is
              checked in Phase 2 (trace the SPL's CTRL writes).
              REVISED exit: "stock U-Boot reads the FIT from the card" moves to Phase 2.
              Nothing in Phase 1 runs the stock SPL: the -bios path runs our BL31 and
              then EDK2, not the image's SPL. Running the SPL needs the bootrom path.
      Phase 2 Bootrom-lite reads TPL and SPL from the SD device (not -bios). TPL runs
              against the existing DDR stubs (ddr-probe/ddr-cfg, P8). SPL loads the
              FIT from the card. Exit: one log showing ROM -> TPL -> SPL -> U-Boot ->
              FIT -> Shell>.
              STATUS: IN PROGRESS (2026-10-08). Blocked at the SPL's hardware SHA-256.
              Done this phase:
                - src/rk3588.c: with no -bios, rk3588_parse_v2_from_sd() reads the
                  idbloader header at 0x8000 through the SD card's block backend and
                  stages TPL (0x8800, 0x12800 bytes) to 0xFF001000 and SPL (0x1b000,
                  0x38000 bytes) to 0, with the P8 call contract (TPL returns to 0).
                  -bios paths unchanged.
                - Link addresses: TPL literals point to 0xFF01xxxx (linked at
                  0xFF001000); SPL literals point to 0x27458..0x2e4fc (linked at 0).
                - Stubs: secure-otp at 0xfe3a0000 (existing OTP hook, RD_DONE) and
                  crypto at 0xfe370000, 0x8000 bytes (+4 self-clears; the SPL's
                  rk_crypto_set_clk polls it).
              Observed chain (no -bios, card = scratch copy of stock image, 16 MiB):
                TPL runs; "U-Boot SPL 2017.09-g5f53abfa1e-221223"; SD (mmc1, printed
                as "MMC2") is read via CMD0/8/55/41/16/17/18/12 through the dw-mshc
                model; "Trying fit image at 0x800 sector" (GPT "uboot" found);
                "## Checking atf-1 0x00040000 ... sha256" then
                "hw_fit_calculate_hash: algo sha256 failed, ret=-22", then
                "SPL: failed to boot from all boot devices".
              Blocker: the SPL's hardware SHA-256 path. Corrected 2026-10-08: the
              -22 is not from the ops dispatch at 0xffc8 (not on the executed path).
              Traced path: 0x5590 (device lookup, uclass id 0x59) -> 0x55e4 -> 0xffd8
              -> 0xfeac -> ops fn 0x1028c (hash init) -> 0x10b94 (ctx alloc) ->
              reset/poll -> wait loop 0x10554-0x105d8 (status at +12, up to 500000
              x 1us, then -110). The -22 is returned through the epilogue at 0x102b0
              with a stale w0, so its exact source is still unconfirmed.
              rk_crypto register facts from the SPL (MMIO base = 0xfe370000, from DT
              node crypto@fe370000 reg, size 0x4000; SPL stores base in a global at
              0x3fe0000+0x220):
                +4   reset/enable; written 0x10001 (init), 0x50005 (reset path 0x107c4)
                +8   written 0 after start
                +12  status: poll until bits other than 0x80 set; W1C clear; ok if 0x40 or 0x4
                +16  mode/command (written with a small value, e.g. 2 at 0x105c0)
                +20  written with w19 (0x10504)
                +64  write-mask convention: high 16 bits = write enable; 0xffff0003
                +72  write-mask convention: 0xffff0000|mode (0x14/0x20/0x60/0x10)
                0x3a0..0x3dc  cleared to 0 before each hash start (16 words)
              RESOLVED (2026-10-08): src/rk3588-crypto.{c,h} models the hash path.
                - Descriptor address written to +0x14; start = write 0x10001 (bit0)
                  to +0x10 (the driver writes +0x40 earlier, which is config only).
                - Descriptor list (guest RAM): {src_addr @0, src_len @4, ...,
                  next @0x20}; walked while src_len != 0. Digest length 0x20.
                - Digest words H0..H7 read at 0x3a0..0x3dc (big-endian per word).
                - +0x3e4 reads 1 when digest valid; writing 1 acks. The SPL polls
                  it (0x109e0) and retries until nonzero.
                - +0x0c reads 0x40 (idle). +0x04 reads 0 (self-clear).
                - Digest via qcrypto_hash_bytes(SHA256) (host crypto, no hand hash).
              Result with no -bios and the stock card (scratch copy):
                "## Checking atf-1 ... sha256 + OK", same for edk2, atf-2 (empty,
                e3b0c4...), atf-3, optee, nvdata. "Jumping to U-Boot(0x00200000) via
                ARM Trusted Firmware(0x00040000)". BL31 prints its NOTICE banner.
              Machine change: SD boot sets BL33 entry to 0x200000 (the FIT edk2 node)
              unless -machine bl33-base is given (rk3588_bl33_explicit).
              NEW BLOCKER (2026-10-08): the stock BL31 parks, it does not hang in a
              poll. Disassembly of the atf-1 blob (image offset 0x600a00, 0x22176
              bytes, load 0x40000): BL31 init writes PMU/CRU registers (0xfd58..,
              0xfd7c..., 0xfd5f0000) then calls 0x5c230 at 0x57fd4, which is
              `dsb; wfi; b` and never returns. Callers of 0x5c230: 0x53c5c,
              0x542ec, 0x57f28, 0x57f38, 0x57fd4, 0x58008. So BL31 is parked
              waiting for an event (or the path is an error); BL33 is never entered.
              Also: BL31 reads and sets bit0 of 0xFEC60028 at 0x5aaf8 (read-modify-
              write; the read is the invalid access, reason: rejected; no stub; the
              address is not in our rk3588_def.h, the DTB, or arm-docs). Likely not
              the cause of the park, but it is unmapped and should get a stub once
              identified.
              Identity of 0xFEC60000: CONFIRMED from arm-docs/rk3588 (TRM address map
              and part1 Ch.16): MAILBOX0 (MCU_PMU), 0xFEC60000-0xFEC6FFFF. The
              0x28 access is MAILBOX_B2A_INTEN (BB to AP interrupt enable); 0x2c is
              MAILBOX_B2A_STATUS (W1C), 0x30/0x34 are B2A_CMD0/B2A_DAT0. BL31 sets
              INTEN bit0 (int0) and waits for the MCU side to post a message. TRM:
              "generated when there are writing operations to CMD_i and DAT_i in
              order". The GIC SPI is 93 (irq_mailbox0_ap0 in Ch.1 table; SPI numbers
              match, e.g. UART2 = 365). The BL31 handler at 0x5a9d4 checks IRQ id
              0x5d (93) and STATUS bit0, dispatches via 0x5219c, then clears STATUS.
              CORRECTION (traced, /tmp/opencode/trace.log via -d exec): the park
              is NOT a wait for a mailbox message. Path: 0x5aafc (RMW of
              0xFEC60028, unmapped) faults -> vector 0x5f800 -> 0x5f000 -> generic
              handler 0x5b778 -> 0x5c444 -> 0x57f3c -> 0x57fd4 -> 0x5c230 (wfi park).
              The mailbox IRQ path (0x5a9d4, SPI 93) is never reached. Fix: model
              MAILBOX0 (0xFEC60000) as readable/writable registers so the RMW at
              0x5aafc succeeds, then re-trace. The message/IRQ path is only needed
              later, for BL31 services.
              PROGRESS (done): MAILBOX0 stub added (src/rk3588-stubs.c, plain
              storage at 0xfec60000); PMU2_CLUSTER_ST (0xfd8d8080) reads 0xfe so
              pmu.c's nonboot-CPU wait succeeds ("nonboot cpus status(0)" gone).
              With no-card SD boot: 0 invalid, BL31 prints NOTICE and no longer
              parks at the mailbox fault.
              CURRENT BLOCKER: Data Abort, FAR=0x4, ELR 0x52adc. Caller 0x500d8
              (from 0x50188, boot init) does x0 = 0x54eb4(idx), idx = [0x61f1c] = 1,
              which selects table 0x67ac0; its +8 pointer is NULL (BSS, not in the
              stock image). 0x52ac8 dereferences it. A writer for 0x67ac8 is not
              found statically; gdb-multiarch (installed) hung in batch mode on a
              watchpoint, so use interactive gdb or add a QEMU-side trace.
              SD CHAIN WITH OUR BL31 (DONE): `make sd-own-bl31` patches a scratch
              copy of the stock card (scripts/own-bl31-sd.py: our BL31 padded into the
              FIT atf-1 slot at 0x600a00, sha256 rewritten in place). SPL verifies
              atf-1 OK, BL31 v2.15 NOTICE, EDK2 front page, 0 invalid, 0 aborts.
              The stock atf-1 still faults (see BLOCKER DETAIL below).
              OPTION 2 RESULT: our own BL31 (atf/build, -bios path) boots EDK2 to the
              UEFI front page with 0 invalid accesses (rc=124 after 200 s, serial shows
              the front-page menu). So the own-BL31 path is healthy; the NULL fault is
              specific to the stock atf-1 blob's init. A true SD-chain test with our
              BL31 needs a scratch FIT with atf-1 replaced and its hash/size updated.
              BLOCKER DETAIL (gdb, hbreak; watchpoints did not fire):
              - idx [0x61f1c] is 1 and nothing in the blob writes it: it is effectively
                constant, so 0x54eb4 always selects table 0x67ac0, which is zero.
              - 0x50680 (copies 0x58 bytes into the tables when [x0+1]==1) is entered
                with x0 = 0, so [x0+1] reads address 1. The NULL comes from the
                0x54ee0-0x54f74 init chain (vendor structs, earlier than 0x500d8).
              - 0x67b18 is filled at runtime with an FDT-magic blob (d00dfeed), so
                the platform config is supposed to come from a parameter block that
                our SD boot does not provide. Next: find where 0x54edc gets its
                struct pointer, or compare with a known-good BL31 (own atf build).
              Helpers: `make regress` (U-Boot + EDK2 checks), `make run-sd`,
              `make trace-sd` (logs under /tmp/opencode).
              MAILBOX0 is MCU_PMU (TRM Ch.16): 0x28 B2A_INTEN, 0x2c B2A_STATUS (W1C),
              0x30/0x34 B2A_CMD0/DAT0. Message data is read by BL31 from its own
              BSS records at 0x87xxx (dispatch 0x5205c, 92-byte record per core),
              not from the CMD/DAT registers.
              Regressions after the crypto change: canonical U-Boot rc=124, prompt
              present, 0 invalid; P7 EDK2 no-card rc=124, banner, 0 invalid; P8
              genbook demo rc=0.
              qemu.patch regenerated (hw/arm/meson.build now lists rk3588-crypto.c).
              Regressions this session: canonical U-Boot rc=124, "=>" present,
              0 invalid; P7 EDK2 no-card rc=124, UEFI banner + Boot Manager, 0
              invalid; P8 genbook demo rc=0 (PSCI self-exit).
              STOCK CHAIN STATUS (2026-10-09, supersedes the gdb notes above):
              - PMU stall fixed: PMU_PWR_GATE_SFTCON2 (0x8154) write hook drives
                PMU_PWR_GATE_STS1 (0x8184), which the TPL polls.
              - Stock BL31 now saves and warm-resets via RMR_EL3 (qemu.patch, accepted).
              - Second boot now fails in the TPL DDR driver: "unknown device" / "error".
                The check at 0xff00ac2c calls 0xff007dc4 with w4 = [struct+436]
                (0x1b4). That field is written only at 0xff008078 from w4 of the same
                call, so the first value is 0 and only 8/9 are accepted (0xff007dfc,
                0xff008004). The same error appears on a cold boot before the SPL, where
                it is non-fatal.
              - Not yet identified: which hardware read or table supplies the DDR device
                type (x22 table at 0xff00c72c / 0xff00c50c). Candidate: a DDR
                controller/PHY ID register returning 0 in our stubs.
              - grf-sec reset bit4 (0xfd587010) must be 1 for stock BL31's save path, but
                with bit4=1 the own-BL31 path enters ddr_resume and fails. Currently
                reset value 0 (own-BL31 passes, stock fails). Unresolved conflict.
              - DDR DEVICE TYPE (2026-10-09, experiment): the TPL device-type switch maps
                7=LPDDR4, 8=LPDDR4X, 9=LPDDR5 (strings + jump table at 0xff001d3c). The
                struct pointer is [0xff013618]=0xff016b10 (BSS) and the field is
                0xff016cc4 (+436). Writing 8 there at reset (EXPERIMENT in
                src/rk3588.c, rk3588_bootrom_lite_reset) gets the stock chain to
                "NOTICE: BL31: v2.12.0 ..." with grf-sec bit4=0. With bit4=1 (needed for
                the stock save path) the warm-reset TPL pass still prints "unknown
                device" and the chain fails; the value is apparently not preserved for
                that pass. Also, with bit4=0 the stock BL31 NULL-faults at 0x52adc.
                Next: find what zeroes/rewrites 0xff016cc4 on the warm pass, or the
                resume-path source of [x24+436] (0xff00b874/b890).
              - Monitor sampling (2026-10-09, sampler in /tmp/opencode/sampler.py): the
                field 0xff016cc4 reads 9 within 1 s of reset, even with an 8 written at
                reset, and stays 9 through the warm pass. So a fixed device-type value
                does not decide the resume pass, which still prints "unknown device".
                The first DDR call path (0xff0086a8 -> 0xff007dc4) also memsets the
                struct (0xff001a0c) before using w4, so the type is set by detection
                code that reads DDRPHY/DDR_MON state (0xfe0c0000.., 0xfe060000..) which
                the stubs do not model. The EXPERIMENT write was removed.
              - Static trace (2026-10-09): 0xff007dc4 takes the type as w4 from
                [struct+436]. 0xff007f38 reads a different check at [struct+40].
                0xff00e8b8 writes [struct+436] with a timing value (udiv result), so
                +436 is shared with a timing field. Live sample: +40 (0xff016b38)
                reads 0x208 at 1 s, not a type code. The "device type = +436" reading
                from the earlier session is not established. DRAM type source is still
                unknown; TRM DDRCTL_MSTR lpddr4/lpddr5 bits (Ch.2) are the likely
                hardware check but the TPL does not appear to read them at this point.
              - REVERSING (2026-10-09, v1.16 TPL in scratch image, decompile addresses
                = runtime - 0x1000; runtime file offset = addr - 0xff001000):
                * Type loop (decompile FUN_ff00b228) tries 7,8,9; DAT_ff015ce4 (runtime
                  0xff016ce4) = 9 at 1 s; training FUN_ff00590c returns -1 when
                  uVar15==0xff (no match) or uVar19==0.
                * uVar15 "no match" comes from an MR12 read (blob FUN_ff004dbc, cmd
                  +0x80 bit0=1, MR no. in +0x84 bits15:8, data +0x94) compared to a value
                  the blob wrote earlier (MR write = FUN_ff003fcc, cmd bit0=0).
                * Added (src/rk3588-stubs.c): ddr-probe MR port (stateful MR store,
                  MR14=0x5d GUESS for LPDDR4X check; MR5=0x01 and MR8=0x00 GUESSES);
                  ddr-cfg2 forced-ready bits at +0xa24 bit1 and +0x684 bit0 (GUESSES,
                  poll loops ff007470/ff006444); ddr-ctl-win plain block 0xfe000000
                  (store fault at 0x200 otherwise).
                * Result: blob now prints "LPDDR4X, 2112MHz" and channel geometry
                  (Size=512MB from MR8 guess), then "Manufacturer ID:0x1", then faults
                  in DQ write/read test FUN_ff00?? (decompile ~8503): address is
                  (ch << 33)+offset, invalid on this machine. Base not resolved.
                * Regressions after these stub changes: U-Boot prompt=1 invalid=0;
                  EDK2 banner=2 invalid=0. Own-BL31 path also stops at Manufacturer ID
                  (it was already failing at grf-sec bit4=1).
              - DQ TEST BASE (2026-10-09, gdb-verified on v1.16 scratch image):
                * Table at runtime 0xFF016B08 holds channel indices 0,1,2,3 (0xF = unused).
                * DQ store address = ch << 33 (lsl #33 on w26 at 0xFF00D21C); ch0 = 0x0,
                  ch1 = 8 GB, ch2 = 16 GB, ch3 = 24 GB.
                * Default QEMU RAM for rk3588 is 128 MB, so ch1..3 abort. With
                  "-m 32G" the DQ test stores and reads back without a fault (experiment
                  only; no Makefile change). This does NOT match real hardware: most
                  RK3588 boards are 8/16 GB, so ch2/ch3 (16/24 GB) are out of range there.
                  Either the test uses a DDR-controller alias, or the decompile mis-types
                  the base. Not resolved. Do not use -m 32G as a default.
                * Blob's own address map (decompile ~2777-2792) uses 4 GB channel strides,
                  and logs.txt shows 4096 MB/ch, so the 8 GB DQ stride contradicts it.
                  Real logs show DQS/DQ read training after Manufacturer ID, not a write
                  failure. Still unresolved.
                * FIXED MR stubs (src/rk3588-stubs.c): MR5=0xff (observed "Manufacturer
                  ID:0xff" in logs.txt); MR8=0x18 (density code 6, GUESS for exact code;
                  decompile ~8205 gives Row=((d+1)>>1)+14, logs need Row=17). Result:
                  "LPDDR4X, 2112MHz", Row=17, Size=4096MB/ch, Manufacturer ID:0xff, which
                  matches logs.txt. make regress: prompt=1 invalid=0; banner=2 invalid=0.

                * Next abort with -m 32G: ELR 0xFF00D598 reads FAR 0xFF080008 (table at
                  [x29+128] + w3*w11, just past the 512 KB SRAM model 0xFF000000..0xFF07FFFF).
                  Then the EL3 vector at 0x200 spins on repeated data aborts (FAR 0xFEFFFFE0,
                  ELR 0x120). Open: SRAM size on RK3588 and the table's origin.
              - DQ TEST BASE RESOLVED (documented, 2026-10-09):
                * Gate (blob ~0xFF00C4E8 = decompile ~8123): DQ test runs when
                  [PMU0_GRF OS_REG9 0xFD588084] != 0x2468ACE. TRM reset value of
                  OS_REG9 = 0 (arm-docs Chapter_6 line ~529), so a real cold boot runs it.
                * ch<<33 equals the channel base in TRM Table 1-5 (Chapter_1_Interconnect
                  line ~932-1000) for CENTER_GRF_CON4[8:4]=0 (4-channel interleave),
                  where channel bases are 0, 0x2_0000_0000, 0x4_0000_0000, 0x6_0000_0000.
                * So the blob writes each channel through its interleave-mode base; the
                  interconnect routes those addresses to the channel. Our QEMU has one RAM
                  block, so ch1..3 bases are unmapped and abort.
                * Model fix is NOT implemented yet: needs an alias of channel RAM at
                  ch<<33 (labelled model choice). Not a default -m change.
                * Not yet checked: CENTER_GRF_CON4 value the blob actually writes.
              - DIAGNOSTIC (2026-10-09): rk/demo/main.c now dumps read-only regs before
                the shutdown delay: CENTER_GRF_SOC_CON0..8 (base 0xFD59E000, TRM Ch.6)
                and PMU0_GRF_OS_REG8..11 (0xFD588080..8C). Builds (make -C rk
                genbook_demo.img). NOT verified: the P8 run never reaches the payload.
              - REAL BOARD DATA (user paste, 2026-10-09, 8 GB board, payload ran):
                * LPDDR4X 2112MHz, Row=16, 2048MB/ch (4 x 2 GB = 8 GB). Manufacturer ID:0xff.
                * CENTER_GRF_SOC_CON4 = 0xd80f -> [8:4]=0 (4-channel interleave).
                * PMU0_GRF OS_REG8..11 = 0xec235053 / 0xeedd1304 / 0x4000000f / 0x10101010
                  (post-training values, not the blob's magic).
                * DQ write test passed silently on this board, so ch<<33 (up to 24 GB)
                  is accepted by an 8 GB board: hypothesis = address bits above DRAM
                  size alias/wrap. Test: U-Boot mw.l 0x200000000 then md.l 0x0.
                * MR8 is board-specific: 0x18 fits 16 GB (Row=17); 8 GB needs density
                  code 3 or 4 (Row=16). Do not hard-code one MR8 for all boards.
              - WRAP ALIAS ADDED (src/rk3588.c rk3588_init, MODEL GUESS): low-RAM aliases
                at 0x2_0000_0000, 0x4_0000_0000, 0x6_0000_0000 (overlap prio -1).
                Result: DQ store at ch<<33 no longer aborts. Next abort is the known
                read at FAR 0xFF080008 (ELR 0xFF00D598), past the 512 KB SRAM model.
                The board is an Orange Pi 5 RK3588S; the TRM and model are RK3588.
              - SRAM SIZE (src/rk3588.c RK3588_SRAM_SIZE 0x80000 -> 0x100000): TRM
                Chapter_1_System_Overview gives SYSTEM_SRAM(1MB) at 0xFF000000. The
                0xFF080008 read is now inside SRAM.
              - NEXT BLOCKER: same table walk now faults at FAR 0xFF110008 (ELR 0xFF00D598).
                gdb at first hit: table base [x29+128] = 0xFF0139F8 (valid); fault comes
                when the loop index w3 grows far past the table. Loop bound w26 is
                computed from training values (stubbed PHY data), so this is a stub
                issue in DQ eye-scan data, not a memory-map issue. SRAM1 is 0x10000 in
                the model vs 256KB in the TRM (not changed yet).
              - ROOT CAUSE OF w26 UNDERFLOW (2026-10-09, gdb): at the bad iteration
                w26 = 0 before "sub w26,#3" (blob 0xFF00D4E0), so w26 = 0xFFFFFFFD.
                w26 counts training-table entries with nonzero halfwords at +4/+6
                (table at ctx+0x718, 12-byte stride, read at 0xFF00D478). All entries
                are zero because the window writers are never executed:
                  - 0xFF00BF24/BF40 (window writer): never hit in the run.
                  - 0xFF00C0D8 (PHY read into window table): never hit.
                  - 0xFF00FF40 (driver that calls BF24): never hit; no direct bl to
                    it found. Likely reached via a function pointer or skipped by a gate.
                Next: find the gate that skips the eye-scan driver (FF00FF40 caller).
              - EYE-SCAN DRIVER TRACE (2026-10-09, -d exec,nochain):
                  - Driver is FUN_ff0102d4 (window-setup callee of ff00ff20). Its callers
                    are at 0xFF010DF0 (inside the function that starts before 0xFF010D6C)
                    and 0xFF011290. Neither the driver nor either caller executes.
                  - Gate before the first caller: cbz on [x19+52], [x19+56], [x19+76],
                    [x19+80] (0xFF010D6C-0xFF010DA4). x19 is the training-config struct.
                  - Execution after "Manufacturer ID:0xff" goes to 0xFF0018xx -> 0xFF0116A4
                    -> 0xFF01153C (putc helper) and then 0xFF00D4xx/D598 (window table walk).
                  - Serial output stops after "Manufacturer ID:0xff" (no training banners).
                Next: dump x19 fields [x19+0..+88] when the flow passes 0xFF010D6C-ish,
                and find which earlier branch chooses the skip path.
              - CONFIG STRUCT (runtime, at 0xFF00B264): ctx x19 = 0xFF016000.
                  [+0x20]=0x33 [+0x28]=0x210 [+0x30]=6 [+0x34]=0x101D00 [+0x38]=3
                  [+0x4C]=0xA [+0x50]=0xA. The gate fields the driver tests (+0x34, +0x38,
                  +0x4C, +0x50) are NONZERO, so the skip is not from these values.
                  The skipped chain is FUN_ff010cf0 (loops over x19[] channel pointers,
                  calls FUN_ff0102d4 at 0xFF010DF0). FUN_ff010cf0 has no direct caller
                  in the blob; callers of FUN_ff010510 (0xFF010E1C, 0xFF011058,
                  0xFF0112BC) are not executed either.
                  Next: find the top-level caller that reaches FUN_ff010510/ff010c2c.
              - CORRECTION (2026-10-09): the channel table is NOT NULL. Decompile
                DAT_ff015a30 = runtime 0xFF016A30 (decompile +0x1000). At the scan entry
                FUN_ff005154 (runtime 0xFF006154) the table holds ch0 = {ctl 0xF7000000,
                scan base 0xFE0C0000, 0xFD800000}. The earlier "zero table" reading used
                the decompile address and is wrong.
              - REAL GAP: scan result reads (scan base + 0x68C/0x690 read value,
                +0x69C/0x6A0 window edges; 0x3FF = no window) hit plain RW storage in
                ddr-cfg2 (src/rk3588-stubs.c rk_stub_ddrcfg2_read: only +0xA24 and +0x684
                are modeled). They read 0 unless the blob wrote them. Real PHY produces
                these values; the model has no source for them.
              - EXPERIMENT 1 (GUESS, src/rk3588-stubs.c ddrcfg2 hook): scan results
                +0x68C/+0x690 = 0, +0x69C/+0x6A0 = 0x00600020 (left 0x20, right 0x60).
                Result: TPL prints "CHn RX Vref:43.2%, TX Vref:29.8%,29.8%" x4 and
                "change to F1: 528MHz". Values are guesses, not board values.
              - EXPERIMENT 2 (GUESS, ddr-probe hook): +0x10B88 |= 0x36000000
                (poll at FF00A370). Result: F1 reached; stalls at FF00A490 poll
                (+0x10014 & 7 must == 3, then bits 5:4 == 2). Setting +0x10014 to
                state 3 alone breaks the earlier poll at FF00B828 (wants state 1).
                The status field is a sequenced state machine; needs real ordering.
                Current build: EXPERIMENT 1 + 2 (0x10014 not stubbed).
              - DDR MODEL MOVED (2026-10-09): DDR register hooks now live in
                src/rk3588-ddr.{c,h} (MR port, ddr-probe, ddr-cfg2). rk3588-stubs.c keeps
                the block table entries and the generic track_engine logic. meson.build
                lists rk3588/rk3588-ddr.c; qemu.patch regenerated and verified.
              - BOARD END-STATE VALUES applied: scan +0x69C = 0x00340033,
                +0x6A0 = 0x00350037, +0x68C/+0x690 = 0; +0x10B88 |= 0x36000000
                (BOARD: 0x36000000 set). +0x10014 NOT modeled (BOARD end value 2).
                Result: F1 reached, stall at FF00A490 (same as before). Printed Vref
                stayed at 43.2%/29.8%, so the printed value does not depend on the
                edge fields changed here. Next: find what sets the printed Vref.
              - BOARD LOG (opi5-16gb-boot.txt, 16 GB real board):
                Row=16 2048MB/die; Vref CH0 RX 28.3 TX 24.8/23.8; CH1 27.9 22.8/21.8;
                CH2 28.9 21.8/20.8; CH3 29.7 22.8/21.8. Ends: +0x10014 = 2;
                scan +0x684 = 0x50000, +0x688 = 0x64a010c, +0x68c/+0x690 = 0,
                +0x69c = 0x340033, +0x6a0 = 0x350037, +0x6a4 = 0x160000.
              - VREF PRINT MECHANISM (decompile 8600-8660; runtime print sites
                0xFF00D318 CH, 0xFF00D348 TX, 0xFF00D5C0 RX):
                FUN_ff00ac3c(code, ch, rx/tx) writes the Vref code (table
                DAT_ff010dc0) to scan base +0x3EC (RX) / TX field. Scan
                FUN_ff005154 builds per-position 12-byte records at runtime
                0xFF0139F8 (word0 = code, ushort +4/+6 = edge sums). The printed
                Vref is the candidate DAT_ff012a1c row (runtime 0xFF013A1C, 6-byte
                stride) with the max window sum. Model: edges identical for every
                Vref => all sums tie => first candidate wins => 29.8% (TX), 43.2%
                (RX). Board gets real per-Vref eye widths. Printed Vref is an output
                artifact of flat scan data, not the stall.
              - Record check (gdb at 0xFF00D5C0): records word0 = 0x148,0x13e,...,
                0x102 (step -0xa); ushort +4 = 0x6a, +6 = 0x69 (= 0x33+0x37, 0x34+0x35).
              - +0x10014 poll: ff00b828 (&7==1), ff00a490 (&7==3), ff00a4a4
                (bits5:4==2). A read-sequence model (1, 0x23, 2) reordered the
                stall to B828, so it was removed. First +0x14 read comes from an
                unidentified early poll.
              - +0x10014 MECHANISM (2026-10-09, gdb + exec trace + qemu_log markers,
                debug code removed): stub engine (rk3588-stubs.c rk_stub_read,
                track_engine) sets bit0 of +0x14 once a write to +0xC80 has happened
                (engine_ready). That satisfies the bit0 polls (ff00b828 and
                ff00787c want &7 == 1). Poll ff00a490 wants &7 == 3, then ff00a4a4
                wants bits5:4 == 2 (0x23 fits). Nothing in the model sets bit1 or
                bits5:4, so the stall is at ff00a490. Board end value is 2 (bit1 set,
                bit0 clear, bits5:4 = 0). Source of bit1/bits5:4 unknown.
              - Exec-trace caveat: '-d exec' logs only translation-block starts, so
                a PC before a marker is a TB start, not necessarily the reader.
                Use gdb (hbreak at the instruction) for exact register values.
              - TRACED TPL (2026-10-09): rk/tools/ddr_trace_patch.py makes
                rk/ddr-trace.bin (make -C rk opi5-trace.img). Trampolines in the unreferenced
                function at ff002534 log each poll: words 1..4 = last value at
                ff00A490/ff00A4A4/ff00B828/ff00787C, words 5..8 = counts. Model result
                (QEMU, xp 0xff0c0000): ff00787C 5 reads value 1, ff00B828 8 reads value 1,
                ff00A490 about 3.3e8 reads value 1 (spins), ff00A4A4 0 reads.
                So the stall is ff00A490 waiting for &7 == 3 while the model only sets bit0.
                Board run: flash opi5-trace.img; read DDR_STATE_LOG in the UART dump.
                Caveat: 0xFF0C0000 is assumed free; check it stays intact after TPL.
              - BOARD OTG RUN (2026-10-09, user): LPDDR4X 2112MHz, Row=16 2048MB/die,
                Vref CH0 RX 28.3 / TX 24.8,23.8 (same as the earlier 16 GB log), then
                change to F1 528 -> F2 1068 -> F3 1560 -> F0 2112. The board passes
                the ff00A490 poll that stalls in the model, so the stall is a model gap
                (bit1/bits5:4 of +0x10014 are set on hardware). No DDR_STATE_LOG
                line and no 'out' line in the paste: the rest of the UART log is needed.
              - BOARD RUN with ddr-trace.bin (2026-10-09, OTG): training completes
                (F1..F0 then 'out'), same Vref as before. DDR_STATE_LOG line NOT in the
                paste even though rk/opi5.bin contains the string. Scan edges differ from
                the earlier run: DDR_SCAN_REGS +0x69C = 0x340034, +0x6A0 = 0x350036
                (earlier 0x340033 / 0x350037): per-run variation. 'Bad payload magic'
                comes from firmware.c (jump_to_payload) because opi5.bin runs without an
                appended payload; not a training failure.
              - BOARD DDR_STATE_LOG (2026-10-09, OTG, ddr-trace.bin): last values
                ff00A490 = 0x2023, ff00A4A4 = 0x2023, ff00B828 = 1, ff00787C = 1.
                Words 0 and 5..8 (count) are garbage: SRAM not cleared at cold boot.
                Value slots are written per read, so they are reliable.
              - MODEL GUESS (rk3588-ddr.c, +0x10014): after 20 reads return stored|0x2022
                (engine adds bit0 => 0x2023). Result: ff00A490 and ff00A4A4 now pass in
                the model; the next stall is ff00A554 (tbnz bit0 of +0x514 with
                ff00A554 waiting for 0). Model tally parity gives 1 there. Board value of
                +0x514 at that point is unknown: opi5.c now dumps DDR_TOGGLE
                (0xF7010500, 8 words).
              - regress after this change: U-Boot prompt=1 invalid=0; EDK2 banner=2 invalid=0.
              - MODEL PROGRESS (2026-10-09, after board DDR_TOGGLE data):
                board +0x510=5, +0x514=1, +0x518/51c=0x80000001 (end state).
                - Fixed probe hooks to be per-channel (stride 0x1000000, ch1
                  ctl base 0xf8010000 confirmed by gdb; ddr_is_ch_off()).
                - +0x10014 read-count guess extended to a cycling guess:
                  after 20 reads, alternate 20-read windows of state 3
                  (0x2022 + engine bit0) and state 1 (0x2020 + bit0); the
                  poll ff00a664 loops while &7==3 (state must move on), so
                  a single fixed value cannot pass all polls.
                - +0x514: fixed-polarity rules failed (ff00baa0 sets 0x20 of
                  +0x510 then wants 1; ff00a540 sets 0x20 then wants 0);
                  current GUESS: bit0 flips on every read starting at 1
                  (any poll exits within 2 reads). Removed the old
                  toggle-on-+0x510-write rule.
                - Result: model now prints the FULL sequence F1/F2/F3/F0
                  (matches board). Post-F0 it enters per-channel training
                  passes (caller loop ff00ad08..ae4c calls ff00490c per
                  index; w20 cycles 0..3) at ~60s per pass and does not
                  converge within 300s. Likely tied to the eye-scan writer
                  chain (ff00ff40/ff0102d4 top-level gate) never executing,
                  so the window tables stay empty.
                - Window-table writer found: copy loop at ff00af1c writes
                  12-byte records, 11 per call, to 0xff013b38 + ch*0x850,
                  from a constant source table at 0xff011ee0 (0xff011d88
                  +0x158).
                - regress at this state: U-Boot prompt=1 invalid=0;
                  EDK2 banner=2 invalid=0.
              - POST-F0 STALL ANALYSIS (2026-10-09): model completes F1..F0 then
                loops forever in a training pass inside the big init function
                0xff00c228. Writer-chain call sites found by call-graph:
                0xff010ed0 (called only from 0xff00d950, inside 0xff00c228)
                calls the eye-scan writer chain (ff010cf0/ff010ed0 ->
                ff0102d4/ff010510 -> ff00ff20 -> ff00ff40); 0xff010cf0 has NO
                static callers (orphan path). The 0xff00d8e0 loop that reaches
                the call is gated per-channel: at 0xff00d8e0 `ldr w0,[x27];
                cmp w0,#0xf; b.eq 0xff00d970` (0xff00d970 = skip channel);
                x27 walks the 4-entry channel map at 0xFF016B08 (values 0..3,
                gdb-verified). Then 0xff00d900 gates on `[x24-56]==2` and
                0xff00d950 `bl 0xff010ed0` unconditionally per live channel.
                GDB: breakpoints at ff010ed0/ff010cf0/ff00d900/ff00d8e0 are
                NEVER hit in the model -> the model never reaches that phase;
                it loops in an earlier pass loop (heads ff00ac1c and
                ff00ad08, both `cmp table_end, cursor; b.ne`, channel map
                entries != 0xf keep them live) and never converges to the
                later phase. Pass loop bodies call training subs
                ff00690c/ff00490c/ff009ae8 per channel/index; ~60s per channel
                in the model; w20 cycles 0..3 indefinitely.
                TB-start-only exec traces CANNOT prove linear code skipped
                (long TBs swallow branches); use gdb hbreak for these checks.
              - POST-F0 ANALYSIS 2 (2026-10-09): the post-F0 phase is NOT an
                immediate stall: exec-trace phase analysis shows steady
                progress (window copy at ff00af1c, per-channel loop 12 hits
                of ff00ac1c, 3 invocations of the big pass function ending
                ff00b158..b188, then phase at ff00ae50, ...). No tight spin.
                But the model is slow: each training pass ~60s (TCG).
                20-min run does NOT reach `out`; board finishes in seconds.
                Long untraced background run started (7200s cap) to see
                whether it converges eventually. Big pass function: start
                before ff00ac1c, epilogue ff00b158-b188 (ret). Driver loop
                heads ff00ac1c/ff00ad08 walk 4-entry channel tables (16 bytes,
                entries != 0xf live); 3 full invocations in first ~150s
                post-F0. If the background run converges, the remaining
                blocker is only runtime; if not, the convergence condition
                needs board data (window tables / scan edges at pass checks).
              - AUTONOMOUS BOARD ACCESS (2026-10-09): the OPi5 is now driven
                from this container without manual steps. Board sits in OTG
                maskrom (2207:350b); `rk/rock.out --v2 --ddr
                img/rk3588_ddr_lp4_2112MHz_lp5_2400MHz_v1.16.bin --os opi5.bin`
                sends blob+payload over USB (usbfs node; after container
                resets /dev/bus/usb/001/<devnum> re-appears with libusb
                working). Debug console = /dev/ttyACM2, 1500000 8N1 raw;
                capture with select-based reader (start capture BEFORE the
                send, same bash call, else the process-group kill on tool
                timeout reaps background jobs). Whole cycle: ~2 s.
                rk/src/opi5.c now ends with plat_reset() (jump_to_payload
                removed): after the dumps the board force-resets into OTG
                maskrom (PMU0_GRF_OS_REG8=0xef08a53c survives, visible in
                the NEXT run's OS_REG8 dump) and re-enumerates, ready for
                the next run. Clean full-log run saved:
                board-runs/2026-10-09-otg-stock-opi5.txt.
                NEW END-STATE DATA (fresh run, stock TPL):
                - DDR_TOGGLE +0x510=5, +0x514=7 (was 1 in the earlier run;
                  bit0=1 both times, upper bits vary) -> +0x514 model guess
                  (flip-on-read, init 1) remains consistent for bit0.
                - DDR_PROBE_STATE +0x14=1 (earlier run: 2) -> +0x10014 END
                  state varies between runs (1 or 2); cycling guess covers it.
                - DDR_SCAN_REGS +0x6a0=0x340034 (varies 33/34/35).
              - V1.16 vs V1.18 A/B (2026-10-09, evening): the stock EDK2 image
                ships fwver v1.18 in its idbloader TPL slot (0x8800, slot size
                0x12800); the v1.16 image used for all DDR reversing is the
                stock image + `dd` of rk/img/..._v1.16.bin at 0x8800. The
                recipe was only in /tmp (lost at container resets); it is now
                the `make sd-v116` / `make run-v116` Makefile targets
                (LOG_DIR scratch, decimal seek=34816 - $((0x8800)) breaks
                under /bin/sh).
                A/B RESULT (same model build + stubs):
                - v1.18 (stock image, fresh copy, 2 runs): prints the FULL
                  banner + new debug lines (DQS rds:/DQ rds:/stride=/hash=),
                  6 stages x 4 ch "chN ttot4" lines, F1->F2->F3->F0 -> `out`.
                  REPRODUCIBLE. Post-`out` the SD chain stays silent in the
                  model (SPL stage frontier, separate from DDR training).
                - v1.16 (recreated sd-v116.img, 300s): baseline unchanged -
                  F0 then stall, no `out`.
                => The model's DDR fakes (+0x10014 cycle, +0x514 flip-on-read,
                +0x10b88, MR port, engine) are SUFFICIENT for v1.18's training
                flow; the post-F0 non-convergence is v1.16-specific. The
                vendor's own code evolution between v1.16 and v1.18 may
                contain the fix (pass-loop convergence / poll semantics).
                v1.18 TPL extracted to /tmp/opencode/v118.bin + v118.dis
                (objdump --adjust-vma=0xff001000). Next-move options:
                (a) diff the post-F0 pass/DQ logic v1.16 vs v1.18;
                (b) make the stock v1.18 chain the model target (milestone:
                stock image's own TPL trains to out);
                (c) v1.16 board mid-training captures (extend
                rk/tools/ddr_trace_patch.py; board cycle now autonomous).
              - VICTORY + V1.24 (2026-10-09, night): declared victory for the
                stock chain: the stock image's own TPL (v1.18) trains to `out`
                in the model. Additionally the latest rkbin blob v1.24
                (rk/img/rk3588_ddr_lp4_2112MHz_lp5_2400MHz_v1.24.bin,
                downloaded from rockchip-linux/rkbin) was tested:
                - BOARD (OTG, autonomous run, board-runs/2026-10-09-otg-v124.txt):
                  completes in ~2 s with real per-lane results: `DQS rds:`/
                  `DQ rds:` lines with live values, per-ch RX Vref ~28-30%,
                  DDR_WIN_TBL filled with per-lane windows (0x620054 style),
                  stride=0x2 ddr_config=0x4, hash masks nonzero;
                  DDR_PROBE_STATE +0x14=2; DDR_TOGGLE +0x514=7 (matches the
                  v1.16 runs); payload dumps + plat_reset cycle fine.
                - MODEL: initially hung right after the first `DQS rds:` print.
                  Root cause found by trace + guest-error log: v1.24's DQ-read
                  training uses a NEW test engine at 0xFE050000 (search fn
                  blob ff0012f4 -> test ff001180/ff001238: program window
                  +0/+4/+8/+c/+2c/+58, poll +0x54, (status & 7) == 4 =
                  pass). The block was unmapped -> the first program store
                  data-aborts -> the CPU spins at vector 0x200 forever
                  (trace tail: PC 0x200).
                  FIX: new stub block `ddr-test-engine` (0xfe050000, 0x1000,
                  track_engine) + rk3588_ddr_test_read: +0x54 returns 4
                  (GUESS: always-pass test). After `make qemu`: v1.24 runs
                  F1->F2->F3->F0 -> `out` in the model (log
                  /tmp/opencode/v124-run2.txt; model reports stride=0x0,
                  ddr_config=0x0, flat Vref 43.7% - the known flat-fake-data
                  pattern, not a blocker).
                - make regress after the change: U-Boot prompt=1 invalid=0,
                  EDK2 banner=2 invalid=0. GREEN.
                - TPL scorecard: v1.18 (stock image) OUT, v1.24 (latest
                  rkbin) OUT, v1.16 (board-reversing target) stalls post-F0
                  (blob-specific pass-loop convergence; see POST-F0 STALL
                  ANALYSIS - not pursued further per user decision).
                - Container reset lost apt packages: `apt-get update &&
                  apt-get install libgnutls28-dev` needed before `make qemu`.
              - P8 STATUS (2026-10-09): `-bios rk/genbook_demo.img` does NOT reach
                "Turning off..." (rc=124, stops at "Manufacturer ID:0xff" with the
                new MR values; also stops at "Manufacturer ID:0x1" with the old ones).
                So this regression predates the MR8/MR5 change. The earlier
                "P8 rc=0" claim is not reproducible now.
              - Regressions at the current tree state: canonical U-Boot prompt=1
                invalid=0; EDK2 banner=2 invalid=0; own-BL31 SD chain NOTICE, 0 invalid.
      Phase 3 Raw loader for atf-1 from the FIT; drop our bl31.elf for this run. Exit:
              full chain with no component of ours, plus the canonical U-Boot
              regression (=> prompt, 0 invalid accesses) still passing.
    Every phase ends with a pasted exit command, per the milestone rule. Changes stay
    local (qemu/AGENTS.md policy: agent code is not for upstream contribution).
    After the milestone: make appimage + smoke test (agents.md rule 8).
P11 (user directive 2026-10-10, next step toward the unmodified-Armbian goal)
    DW-MMC/eMMC storage models (Linux-interrupt-driven) + real CRU
    clock/reset emulation, so the stock mainline drivers bind and an
    image-backed eMMC disk is mountable as a rootfs by our Linux 7.2.9
    boot.  All kernel-side facts below are from the in-tree 7.2.9
    sources; all QEMU-side facts from this QEMU tree.
    Kernel config (already =y in defconfig, verified): MMC, MMC_BLOCK,
    MMC_SDHCI_OF_DWCMSHC, MMC_DW(_PLTFM/_ROCKCHIP), CLK_RK3588,
    PHY_ROCKCHIP_EMMC, EXT4_FS, EFI_PARTITION (GPT).
    DT facts (rk3588-base.dtsi):
      - cru: clock-controller@fd7c0000, compatible "rockchip,rk3588-cru",
        reg <0xfd7c0000 0x5c000>, #clock-cells 1, #reset-cells 1,
        assigned-clocks incl. PLL_PPLL/AUPLL/NPLL/GPLL (=> PLL set_rate
        at boot, so PLL lock polls MUST pass).
      - sdhci (eMMC) mmc@fe2e0000: compatible "rockchip,rk3588-dwcmshc",
        reg size 0x10000, interrupts SPI 205 LEVEL_HIGH, clocks
        core/bus/axi/block/timer = CCLK/HCLK/ACLK/BCLK/TMCLK_EMMC,
        resets SRST_{C,H,A,B,T}_EMMC, max-frequency 200 MHz,
        supports-cqe (CQE EXCLUDED from our DT for now — see notes).
      - sdmmc mmc@fe2c0000: dw-mshc "rockchip,rk3588-dw-mshc", SPI 203,
        clocks biu/ciu from SCMI firmware clocks (no SCMI in the model:
        custom DT uses fixed clocks there), ciu-drive/ciu-sample =
        SCLK_SDMMC_DRV/SAMPLE from cru; fifo-depth 0x100.
    CRU driver facts (drivers/clk/rockchip/clk-rk3588.c, clk-pll.c):
      - CLK_OF_DECLARE early init + platform probe (late branches);
        reset controller via rk3588_rst_init -> rockchip_register_softrst_lut
        (SOFTRST_CON(x) = 0xa00 + 4x in each CRU window, HIWORD writes;
        the LUT also covers secure-cru resets).
      - Register map: PLL_CON(x)=4x (CON0=M[9:0], CON1=P[5:0]|S[7:6]|PWRDOWN[13],
        CON2=K, CON6=lock bit15); MODE_CON0=0x280 (2-bit mode per PLL);
        CLKSEL_CON(x)=0x300+4x (up to 177); CLKGATE_CON(x)=0x800+4x (up
        to 177); SOFTRST 0xa00..0xa7c; GLB_SRST_FST=0xc08 (u-boot/TF-A
        reset path only, kernel never writes it); SDMMC_CON0/1=0xc30/c34.
        Sub-windows inside the one 0x5c000 DT region: PHP 0x8000 (PPLL
        lives here: PMU_PLL_CON(x)=4x+0x8000, PLL_CON(128)=0x8200, lock
        0x8218), PMU1CRU 0x30000, BIGCORE0 0x50000 (B0PLL lock 0x50018),
        BIGCORE1 0x52000 (B1PLL CON(8)=0x20, lock 0x52038), DSU 0x58000
        (LPLL CON(16)=0x40, lock 0x58058). Main-window PLLs: V0PLL
        CON(88)=0x160 (lock 0x178), AUPLL CON(96)=0x180 (lock 0x198),
        CPLL CON(104)=0x1a0 (lock 0x1b8), GPLL CON(112)=0x1c0 (lock
        0x1d8), NPLL CON(120)=0x1e0 (lock 0x1f8).
      - rk3588 PLL set_rate writes CON0/1/2 with HIWORD_UPDATE masks and
        polls CON6 bit15 (rockchip_rk3588_pll_wait_lock, 1 ms timeout);
        recalc_rate READS CON0/1/2 at registration.  The GRF
        GRF_SOC_STATUS0 (0x600) lock bits are only used by the ancient
        rk3066 PLL type — irrelevant here.
      - CRITICAL for the model: registration-time recalc READS every
        CLKSEL_CON.  The current generic stub rule (lock any offset
        == 0x18/0x38 mod 0x40) would corrupt divider reads at 0x318,
        0x338, ... => main-window lock reads must use the EXACT offset
        list; the mod-0x18/0x38 rule stays for the other CRU windows
        (firmware PLL polls live there and firmware never reads dividers).
      - PLL preload: CRU resets read 0 in the model, so recalc would
        compute 0 MHz PLLs.  Preload canonical CON0/1/2 per PLL at model
        reset (GPLL 1188 MHz m=198 p=2 s=1, CPLL 1000 MHz m=250 p=6 s=0,
        PPLL 100 MHz m=25 p=3 s=1, NPLL 850 MHz m=425 p=6 s=0... values
        from the rk3588_pll_rates table tuples (rate, p, m, s): rate =
        24 MHz * m / (p * 2^s)); CON6 lock bit comes from the read hook.
        dclk_vp0 148.5 MHz = GPLL 1188/8 lands exactly on a divider —
        the display chain keeps working under real CRU clocks.
      - RkStubBlock gains a reset-value table (offset, value) pairs;
        only the CRU blocks use it.
    eMMC / sdhci-of-dwcmshc driver facts (7.2.9 sdhci-of-dwcmshc.c):
      - Probe: gets clocks core+bus (+ axi/block/timer via
        dwcmshc_rk35xx_init clk_bulk), reads P_VENDOR_AREA1 (0xe8) & 0xfff
        -> vendor_specific_area1; MUST be non-zero (0 writes would hit
        core SDHCI regs at area1+8).  Real rk35xx value: 0x500 (HOST_CTRL3
        0x508, EMMC_CONTROL 0x52c, EMMC_ATCTRL 0x540, AT_STAT 0x544);
        P_VENDOR_AREA2 (0xea) = 0x540 (CQE only).  Model returns 0x500/0x540.
      - rk3588 ops: set_clock = dwcmshc_rk3568_set_clock (sdclk divider
        bits non-functional; rate via clk_set_rate(core); <=52 MHz branch
        writes DLL CTRL/RXCLK/TXCLK/STRBIN/CMDOUT 0x800-0x810 + ATCTRL —
        pure storage; >52 MHz polls DLL_STATUS0 0x840 bit8 locked !bit9
        — AVOIDED by not advertising SDR104/HS200/HS400 in capareg).
        reset = rk35xx_sdhci_reset (reads DECMSHC_EMMC_MISC_CON 0x81c,
        optional reset_control_array, writes back MISC_INTCLK_EN bit1 —
        storage ok).  init writes HOST_CTRL3=0, DLL_TXCLK/STRBIN=0.
      - CAPABILITIES (0x40) from the model's capareg property: 8-bit bus
        (EMBEDDED_8BIT slot type), ADMA2, SDR25/50 only (no SDR104/HS200
        => no execute_tuning/AT_STAT path), no 64-bit V4 (32-bit ADMA).
        SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN is in the rk3588 quirk list —
        driver never reads caps base clock, so model value is cosmetic.
      - Card: QEMU TYPE_EMMC ("emmc" device) with "drive" + optional
        boot-partition-size/boot-config — real eMMC protocol incl.
        CMD1/CMD2/CMD3, EXT_CSD, boot partitions (hw/sd/sd.c sd_proto_emmc).
      - Host: QEMU TYPE_SYSBUS_SDHCI ("sysbus-sdhci", capareg property,
        SDHC_REGISTERS_MAP_SIZE window) is a complete standard SDHCI with
        IRQs; wrapper device maps a 0x10000 container: inner sdhci +
        vendor storage region 0x100-0xffff + overlay for 0xe8/0xea.
        -drive if=sd unit 1 attaches the eMMC (unit 0 stays the SD slot).
    dw-mshc (sdmmc) Linux facts: Linux dw_mmc is interrupt-driven
        (SPI 203 not yet wired in the model — added here); dw_mmc-rockchip
        sets ciu rates via clk_set_rate (fixed-clock ciu in our DT is
        accepted with a rate-set error print only), CD via cd-gpios GPIO0
        PA4 (already card-following from P10).
    Excluded / deferred (noted for the stock-DTB milestone): CQE
        (supports-cqe), SCMI clock provider (stock sdmmc clocks), power
        domains (genpd pm-domain driver over the PMU), pinctrl.
    Implementation order:
      1. CRU stub: exact main-window lock offsets + PLL preload reset
         values + RkStubBlock reset-value table.  dts: real cru node
         (rockchip,rk3588-cru) replacing the fixed clocks where the
         in-tree dtsi uses cru clocks (display pipeline incl. hdptxphy
         resets via SRST_HDPTX_*), sys_grf gains "rockchip,rk3588-sys-grf".
      2. src/rk3588-sdhci.c eMMC wrapper + rk3588.c attach + DT node +
         IRQ 205; Makefile: images/emmc.img (GPT + ext4, host mke2fs)
         + run-linux wiring.
      3. initramfs demo: /dev/mmcblk0 partition scan + mount /dev/mmcblk0p1
         + read-verify of a host-written file (ext4 mount = the rootfs
         proof), same for the SD slot.
      4. dwmshc IRQ emission for Linux (normal-interrupt + IDSTS paths).
      5. Exit: `make run-linux` log pasted (mmc0/mmcblk0p1 + EXT4 mount +
         demo PASS), `make run-linux` DRM chain still green (P6 stays
         reproducible), `make regress` green, `make appimage` + smoke.
    STATUS 2026-10-10, IN PROGRESS (session 1; CRU live, eMMC host up,
    card protocol stall = the remaining blocker, next step precise):
    - Built: RkStubBlock reset-value table + exact main-CRU-window PLL
      lock offsets (0x178/198/1b8/1d8/1f8; the old mod-0x18/0x38 rule
      stays for the other CRU windows — it would corrupt Linux's
      CLKSEL_CON recalc reads in the main window); PLL CON0/1/2
      preloads (V0PLL/AUPLL/CPLL 1000M, GPLL 1188M, NPLL 850M, PPLL
      100M @ php window, B0/B1/LPLL 1188M) so recalc_rate computes
      real rates.
    - dts/rk3588-qemu.dts: real cru node (rockchip,rk3588-cru, #clock-
      cells 1, #reset-cells 1, reg 0x5c000), eMMC sdhci@fe2e0000 node
      (5 clocks + 5 resets via cru ids 300/298/299/301/302 +
      280/278/279/281/282, bus-width 8, non-removable, max-frequency
      200M), sdmmc dw-mshc node (fixed biu/ciu, optional drive/sample
      clocks omitted), display pipeline migrated to cru clocks/resets,
      sys_grf compatible += "rockchip,rk3588-sys-grf".
    - src/rk3588-sdhci.c (NEW, meson + qemu.patch updated): eMMC host
      = "rk3588-sdhci" (TYPE_SYSBUS_SDHCI subclass) + vendor-ptr
      overlay (0xe8/0xea = 0x500/0x540) + page-aligned vendor RAM +
      TYPE_EMMC card from -drive if=sd unit 1.  make run-linux attaches
      images/emmc.img (scripts/mk-emmc-img.py: MBR + ext4 at LBA 2048,
      hello.txt, host mke2fs -d/-E offset; needs e2fsprogs in the
      container apt list).
    - initramfs/demo.c: P11 block (open /dev/mmcblk0 with async poll,
      MBR 0x55AA + partition type, open /dev/mmcblk0p1, mount ext4 to
      /mnt, read-verify hello.txt).
    - FIVE model/driver bugs found and fixed this session:
      1. The P7-era "emmc-sdhci" stub block (plain storage at
         0xfe2e0000) shadowed the new real device (stubs map after
         devices, same priority => stub wins) => caps read 0 =>
         sdhci_setup_host -ENODEV "timeout clock frequency" (the
         message goes to the "mmc1:" prefix — easy to miss in greps).
         Fix: stub entry removed; the real device owns the window.
      2. QEMU's SDHC_REGISTERS_MAP_SIZE is 0x100, not 0x1000: the
         dwcmshc vendor regs (0x508..0x844) sat in an unmapped hole
         and the first vendor write data-aborted the guest
         ("synchronous external abort" at dwcmshc_rk35xx_init+0xe0).
      3. TCG asserts on non-page-aligned leaf regions: a vendor RAM
         at base+0x100 aborted QEMU (cputlb.c tlb_set_page_full).
         Fix: one RAM page at +0x000 (inner SDHCI 0x100 mapped after
         wins the head) + RAM tail at +0x1000.
      4. RK3588 sdclk-enable bits are non-functional in hardware and
         the driver never sets SDHCI_CLOCK_CARD_EN — but QEMU's
         sdhci_can_issue_command() gates command issue on
         SDHC_CLOCK_IS_ON (all 3 bits).  Fix: "rk3588-sdhci" subclass
         copies the parent's MemoryRegionOps and forces bit 2 on
         CLOCK_CONTROL (0x2c) writes before delegating.
      5. The fixed-clock node lacked clock-output-names = "xin24m":
         clk-rk3588 looks PLL parents up BY NAME (PNAME mux_pll_p =
         {"xin24m","xin32k"}) => orphan parent => every PLL recalc
         0 MHz (clk_rate 0 confirmed in guest debugfs) => f_max 0,
         rk35xx_get_max_clock (clk_round_rate ULONG_MAX) = 0, and the
         clock-enable handshake never ran.  Fix: property added; GPLL
         reads 1188 MHz, "Disabling HS200/HS400, frequency too low
         (0)" gone, clock handshake trace shows 0x7 states.
    - CRU EMULATION VERIFIED LIVE (guest evidence, not just boot
      survival): clk-rk3588 early + platform probe bind fd7c0000
      (sysfs driver dir), the full clock tree registers
      (debugfs clk_summary: clk_hdmiphy_pixel0 = 148500000 Hz etc.),
      dclk_vp0 lands exactly on GPLL/8, hdptxphy + display + eMMC
      clocks resolve, resets provider registered, and the P6 DRM
      chain still boots (bound vop/hdmi, fbcon 240x67, LINUX-OK in
      the same run — no P6 regression from the CRU migration).
    - CURRENT BLOCKER (precise resume point): the eMMC host fully
      probes ("mmc1: SDHCI controller on fe2e0000.mmc [fe2e0000.mmc]
      using ADMA") and commands reach the card (trace:sdbus_command:
      CMD52/CMD5/CMD55 correctly ignored by the eMMC proto; guest
      errors log shows the expected "wrong state: idle" for them),
      but the card never answers CMD1 and stays in idle:
      "eMMC: CMD8 in a wrong state: idle" at the EXT_CSD stage and
      "error -110 whilst initialising MMC card" after the 10 s
      timeout.  No sdhci_response4 trace events => no response is
      ever latched.  sd_cmd_SEND_OP_COND (shared ACMD41 handler)
      returns sd_r3 only after the OCR/VDD-window check; CMD0's
      sd_reset -> sd_set_ocr sets sd->ocr = R_OCR_VDD_VOLTAGE_WIN_HI
      (voltage-window bits) and the kernel's CMD1 goes out with
      arg 0x40ffff00.  Next: rerun with
      -d trace:sdhci_response4,trace:sdhci_response16,trace:sdbus_command
      and read the CMD1 OCR the card returns vs the kernel's arg
      (suspect: the response IS sd_r3 but the sdhci rspreg latch or
      the VDD-window bit overlap — R_OCR_VDD_VOLTAGE_WIN_HI_MASK vs
      0x40ffff00 — leaves the card in idle so CMD2/3/7 never advance
      it; also check the inquiry-CMD1 powerup timer path).
      Evidence: logs/p11-progress-boot-2026-10-10.txt,
      logs/p11-sdbus-trace-2026-10-10.txt,
      logs/p11-sdhci-access-trace-2026-10-10.txt (CLOCK_CONTROL
      handshake 0x4->0x2005->0x7 visible),
      logs/p11-guest-errors-summary-2026-10-10.txt.
    - Regressions at this state: U-Boot prompt=1 invalid=0; EDK2
      banner=2 invalid=0 (the removed emmc-sdhci stub does not affect
      the EDK2 no-card path).  LINUX-OK still prints in run-linux.
    - Deferred (unchanged): dwmshc IRQ emission for the Linux dw_mmc
      path, ext4 mount demo PASS lines, exit evidence, appimage.

## Rules of engagement
- ./qemu tree: only qemu.patch application + symlink; regenerate patch via git diff.
- ./rk: minimal patches, real-hardware compatible, each documented here.
- No commits unless explicitly asked.
- A milestone is done only when its exit command is run and output pasted.
- SCOPE (updated per user directive 2026-10-07): "let's do p4/p8/p9" — P4, P8, P9 are
  explicitly requested and ALL DONE (P4 2026-10-07, P8+P9 2026-10-08). Do not start
  P5 (Linux), P6 (display) or P7 (edk2) unless the user explicitly asks; they stay
  documented above as future work only.
- SCOPE update 2026-10-08: P7 was continued on the user's standing "continue if you
  have next steps" directive and is DONE (exit evidence in P7). P10 (unmodified image
  boot, Steps 0 and Phases 1-3) was requested by the user on 2026-10-08.
- SCOPE update 2026-10-10: P5 was requested and is DONE (exit evidence in P5).
  The user then directed work towards booting the unmodified Orange Pi 5 Armbian
  image, "next: hdmi" — P6's target moved to the OPi5 HDMI path, which is
  DONE (exit evidence in P6; the analogix eDP AUX/DPCD sub-goal is
  superseded by P9's scanout shim + the HDMI chain). The user then
  directed P11 (DW-MMC/eMMC storage + CRU emulation) as the next step
  toward the unmodified Armbian image boot.
