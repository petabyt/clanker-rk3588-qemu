# Agents — how to work in this repo

## What this is
A QEMU machine/device model for the Rockchip RK3588 (reference boards: Orange Pi 5,
Cool-Pi Genbook), booting the ./rk bare-metal bootloader -> U-Boot -> upstream Linux ->
edk2-rk3588. Ambitious, phased; see plan.md for milestones and exit criteria.

## Layout
- ./qemu/    upstream QEMU clone. DO NOT edit by hand. Changes = qemu.patch only,
              applied idempotently by the top-level Makefile (see Rules #1).
- ./src/     all RK3588 QEMU sources (machine + device models). Reached from QEMU
              via symlink qemu/hw/arm/rk3588 -> ../../../src (created by the build).
- ./qemu.patch  the ONLY patch to qemu: hw/arm/Kconfig entry + hw/arm/meson.build line.
- ./Makefile top-level build: applies qemu.patch + symlink, configure+ninja into
              build/qemu (aarch64-softmmu), and ./rk bootloader images.
- ./rk/      the bootloader (own git repo). Patchable — keep patches minimal and
              real-hardware compatible; document each one in plan.md.
- ./u-boot/, ./linux/   source checkouts (P4/P5), built aarch64. u-boot is also the
              best reference for how real drivers touch device registers (ns16550/
              8250_dw serial, rockchip cru/grf/iomux, rk3588 dts) — clone it early
              and grep it whenever a register's meaning is unclear.
- ./edk2/    EDK2 inputs: BL33_AP_UEFI.Fv (5 MiB FV, P7) and the stock
              orangepi-5_UEFI_Release_v1.1.img (P10 target; must stay unmodified).
- ./packaging/ AppImage desktop/icon files used by `make appimage`.
- ./images/  artifacts: kernel Image, dtb, initrd, edk2 img, packed boot images.
- ./arm-docs/ architecture reference (see next section) — XML/HTML first, PDFs last.

## arm-docs/ — how to use it
Machine-readable Arm reference (2026-09 releases). Prefer XML/HTML here over the PDFs;
it is far easier to grep and read a few hundred lines than to page through PDFs.

### ISA_A64_xml_A_profile-2026-09/ — instruction reference
- `xhtml/<mnemonic>.html` (2321 files) — ONE file per instruction, fully rendered
  prose: description, encoding diagram, pseudocode, exceptions. Easiest for reading:
  `xhtml/adc.html`, `xhtml/tlbi_sys.html`.
- `*.xml` (2322 files) — same content as source XML (e.g. `adc.xml`, `dc_sys.xml`,
  `tlbi_sys.xml`). Use when you want structured bits (regdiagram boxes, encodings).
- `index.xml` — alphabetic index of every instruction with a one-line summary.
  First stop when you only know the mnemonic: grep a name here, then open the file.
- `encodingindex.xml` (4.4 MB) — decode-by-encoding: raw opcode bits -> instruction
  (`encname`, `iformfile`, feature conditions). Grep this to identify an unknown
  instruction word or to enumerate encodings in a bit range.
- `shared_pseudocode.xml` (3.4 MB) — shared pseudocode routines referenced by
  instruction pages (exception entry, address translation helpers, etc.).
- `diff-from-ISA_A64_xml_A_profile-2026-06/` — per-instruction HTML diffs vs the
  previous release (what changed).

### SysReg_xml_A_profile-2026-09/ — system register reference
- `AArch64-<name>.xml` (894 files) — one file per sysreg: purpose, field-by-field
  descriptions (bit ranges, access, reset values, FEAT_* conditions), PSTATE/ESR
  encodings. Example: `AArch64-esr_el3.xml` (decode ESR in the guest's EL3 handler),
  `AArch64-sctlr_el1.xml`, `AArch64-cntvct_el0.xml`. Lowercase name, prefix
  `AArch32-` for the AArch32 alias.
- `xhtml/<same-name>.html` (1970 files) — rendered prose of the same register;
  easier to read, same content.
- Naming lookup: `ls SysReg_.../AArch64-*<partial>*` or grep across `*.xml`.

### PDFs (fallback only)
- `Rockchip RK3588 TRM V1.0-Part1/Part2` + `RK3588 Brief Datasheet.pdf` — SoC
  register manual. No pdftotext tooling is installed; if a register is only
  documented here, `apt install poppler-utils` then `pdftotext -f <page> -l <page>`.
  Try upstream Linux dt-bindings/headers and QEMU guest code first; PDF last.

### rk3588_bootrom.bin + rk3588_bootrom.c — real bootrom dump + Ghidra decomp
- 32 KB at 0xffff0000; the .c is a full C decompilation (names are Ghidra-style
  FUN_ffffXXXX / DAT_...). Read it for the real boot contract before designing
  handoff/parking behaviour. Key facts already extracted:
  - Entry `FUN_ffff0000`: cores run at EL3; secondary (mpidr aff0 != 0) writes 1 to
    0xffff00e0, then WFE-loops until *0xffff00e0 == 0xDEADBEEF, then jumps to *0xffff00e8
    (park/release protocol). Primary sets VBAR_EL3 and calls `entry()`.
  - `FUN_ffff06a0_guess_validate`: loads a first stage to 0xff001000 and calls it with
    (x0=0xff000078, x1=8), then loads the main image to address 0 and jumps there with
    the SAME args: payload entry contract = pc 0, x0=0xff000078, x1=8 (SRAM boot-data
    block; rk firmware ignores them today).
  - Boot-method flag lives at 0xff000010; staging SRAM at 0xff000000-0xff01xxxx;
    no UART/print code in the bootrom (it never prints).
  - Our QEMU model does NOT emulate the bootrom (plan.md): -bios image at 0 + EL3
    reset reproduces the same handoff.

## Build & run
- Build:   make            (QEMU into build/qemu/ + rk images; `make qemu` / `make rk` to split)
- Boot:    scripts live here as they appear (scripts/run-*.sh); until then:
           build/qemu/qemu-system-aarch64 -M rk3588 -nographic -serial mon:stdio -bios <image>
- Bootloader build: make -C rk opi5.bin / genbook.bin   (needs aarch64-linux-gnu-gcc, dtc)

## Rules
1. qemu/ gets only qemu.patch + the hw/arm/rk3588 symlink (the Makefile applies both).
   To edit the patch: edit hw/arm/Kconfig and/or hw/arm/meson.build in qemu/ directly,
   then `git -C qemu diff -- hw/arm/Kconfig hw/arm/meson.build > qemu.patch`.
   `make` re-applies it idempotently: pristine -> apply, already applied -> verify,
   out of sync -> error with that regeneration hint.
2. New/changed device code goes in ./src, follows QEMU style (QOM for stateful
   devices, one device per file, trace events over printf, no debug prints).
3. Prefer stub registers that satisfy the guest's polling loops over silence.
   Find gaps with: -d unimp,guest_errors -D qemu.log — every line is a to-do.
4. Never make ./rk unbootable on hardware; QEMU-specific hacks stay in tiny,
   clearly labeled patches (or on the QEMU side).
5. Do not commit unless explicitly asked.
6. Claim of a milestone requires running its plan.md exit command and pasting output.
7. SCOPE (user directive 2026-10-07: "let's do p4/p8/p9"): P4, P8, P9 were explicitly
   requested — ALL DONE (2026-10-08). P7 (edk2 FV boot) is DONE. P10 (boot the
   unmodified orangepi-5 image through BootROM/TPL/SPL/u-boot/FV) was requested
   2026-10-08. Never start P5 (Linux) or P6 (display) without an explicit request.
8. AppImage (user directive 2026-10-08: "regenerate the appimage (and do that
   whenever you finish/get to milestone)"): after finishing a milestone (and
   after any ./src change), run `make appimage` and smoke-test the artifact
   (boot demo -> self-exit rc=0).  The recipe wipes build/AppDir first —
   linuxdeploy does NOT re-deploy usr/bin/qemu-system-aarch64 when the file
   already exists, so without the wipe the image ships a stale binary.

## Debug toolkit
- Hang in a poll loop: -d exec (tail it), or gdbstub -s -S + aarch64-linux-gnu-gdb.
- Guest exceptions print via rk's own EL3 handler ("!!!!! Exception !!!!!" + ESR/ELR).
- Timer note: guest derives µs from CNTFRQ_EL0 — machine must set 24 MHz.
- CPU: TCG only (no /dev/kvm); 8x cortex-a76, has_el3, reset EL3 pc=0.

## Current milestone status: P0+P1+P2+P3+P4+P7+P8+P9 DONE (2026-10-08); P10 in progress.
Scope rule #7 applies: P5 (Linux) and P6 (display) only on explicit user request.
P10 (unmodified image boot) is the active project; see plan.md for its phases.
P10 Step 0 (decode) DONE. Phase 1 controller DONE; its exit was revised (stock SPL
runs in Phase 2, not Phase 1). AppImage rebuilt and smoke-tested 2026-10-08.
Scratch SD images: copy the stock image to /tmp and pad to 16 MiB; never -drive the
stock file directly (EDK2 writes to the card).
Shutdown (2026-10-08, post-milestone): PSCI_SYSTEM_OFF (demo fw_handler / U-Boot
`poweroff` via CONFIG_CMD_POWEROFF+CONFIG_SYSRESET_CMD_POWEROFF+CONFIG_SYSRESET_PSCI)
-> EL3 firmware plat_shutdown() (rk/src/rk3588/io.c) writes 'OFF!' (0x4f464621)
to 0xfd58affc (grf-boot window) -> QEMU rk_stub_poweroff_write hook
(src/rk3588-stubs.c) -> qemu_system_shutdown_request: demo, ./p9-exit.sh and
U-Boot `poweroff` all exit QEMU rc=0 on their own (plan.md P8 "Shutdown
follow-up").  Firmware DTB also carries a /psci node; U-Boot uses its own linked
DTB as control FDT (fdtdec_setup ignores the x0 DTB), which already had
arm,psci-1.0/method=smc from rk3588-base.dtsi.
P9 (2026-10-08): VOP2 -> ramfb display (src/rk3588-display.c, +meson.build,
qemu.patch regenerated).  One graphic console created at machine init; its
GraphicHwOps.gfx_update reads the firmware's ESmart2 scanout regs
(0xfdd91c00: mst_ctl/yrgb/vir/act_info — rk/src/rk3588/vop2.c
config_smart_layer) and maps guest RAM at fb=0xd0000000 (1920x1080 XRGB8888)
into a qemu_create_displaysurface_from surface exactly like hw/display/ramfb.c;
QEMU invokes gfx_update before UI redraws and inside screendump's
qemu_console_co_wait_update, so no timer and no display backend needed.
Exit: ./p9-exit.sh — boots demo, HMP screendump over unix monitor socket after
"Memory description map:", no `quit` (guest PSCI_SYSTEM_OFF must exit QEMU rc=0);
asserts qemu rc=0, monitor rc=0 + PPM 1920x1080 with bg #4398d7 + white font
present; qemu-p9.log empty; P8 canonical command self-exits rc=0 after
"Turning off...".
P8 (2026-10-08): stock-DDR boot path — bootrom-lite in rk3588.c parses the makeboot
v2 header at init (ImageEntry UNPACKED stride 88; genbook_demo.img = ddr@0x8800
+0x12e00, os@0x1b600+0x28000), stages firmware with physical_memory_write (rom_reset
bytes arrive too late), enters the blob at 0xff001000 with the bootrom register
contract (x0=0xff000078, x1=8, x30=trampoline) and returns to the main image at 0.
genbook-ddr.bin = ddr.S shim (GPIO/PWM C code, register-preserving) + stock blob
body byte-identical from byte 4. DDR-blob register model cracked hang-by-hang in
rk3588-stubs.c (track_engine windows ddr-probe/ddr-cfg2, per-engine 64K sub-blocks):
+0x80 START self-clear, +0x90 busy/read-done, +0x14 done-after-C80, +0xb4 bit18
training-done, +0x3cc done, +0xC84 mirrors +0xC80, +0x514 = toggle status (init 1,
flips on each +0x510 write with bit0 clear; polls ff00baac=1/ff0059b0=0/
ff005a34=1 — mirrors and simple parity models fail at least one site).
Exit: DDR banner -> payload eDP bring-up -> "Hello World from Payload" ->
"Turning off..." then QEMU self-exits rc=0; qemu-p8.log empty;
-bios genbook_uboot.bin boots to `=> ` and `poweroff` exits rc=0.
Debug recipe: -d exec address histogram -> hotspot PC -> objdump of
rk/genbook-ddr.bin (--adjust-vma=0xff001000) -> gdb-multiarch -batch -x
script.gdb (plain gdb has no aarch64 target; gdb-multiarch added to README).
P4 (2026-10-07): genbook.bin + uboot-payload.bin (FuPayloadHeader, PAYLOAD_FLAG_UBOOT)
-> u-boot.bin @0x00800000 at EL2, banner + `=> ` prompt, empty qemu-p4.log.
All P4 patches documented in plan.md (./rk: uboot_head.S, PAYLOAD_FLAG_UBOOT,
jump_to_uboot, start_in_el2 x0 param; QEMU: OTP/TRNG, PLL-lock offset pattern,
IOC merge, bigcore/GRF windows; ./u-boot: SCMI off, spi2/sdhci/usb disabled in
board dtsi). Debug recipe that cracked the post-relocate hang: -d exec hot-PC
histogram -> QMP `xp` scan found relocated image at 0x1FF07000 (reloc_off
0x1F707000) -> addr2line on u-boot ELF -> udelay/rk3588_pll_set_rate poll on
unmodelled PLL lock bits.
P3 fix (documented ./rk patch): rk/src/asm.S start_in_el2 no longer sets
SCTLR_EL3.WXN (bit19) — WXN is architecturally active at EL3 and turned the
firmware's RWX 1GB EL3 blocks into XN, faulting every fetch (ESR IFSC 0x0d,
level-1 permission fault, nested abort at VBAR+0x200). Symptom -> diagnosis
recipe: `-d int,guest_errors` first-exception decode (ESR EC/IFSC + FAR + SPSR
EL) + addr2line on genbook.bin.elf.
NOTED per user (2026-10-07 — P8+P9 DONE): boot a completely
unmodified rk3588 image (Rockchip header parse -> stock DDR blob at 0xFF001000
-> back to main image at 0; first target = unmodified genbook image, eDP path)
and a basic VOP2 framebuffer via ramfb.
P1 delivered in ./src: rk3588.c (machine: split RAM, 8x A76 EL3, stock arm-gicv3 at
0xFE600000/0xFE680000 with timer PPIs, -bios at 0, arm_load_kernel bookkeeping) and
dw-apb-uart.c (DW-APB UART device; UART2 -> serial_hd(0), UART0 -> serial_hd(1); UART IRQs
deliberately not wired yet — SPI numbers come from the TRM in P2/P4).
P2 delivered: rk3588-stubs.c(+.h) — register-file stubs for every block the ./rk firmware
touches, in two semantics classes: hiword write-mask (CRU/PMU/GRF/SGRF/IOC/GPIO/DDR-GRFs;
mask bits 31:16 gate data bits 15:0, high half never reads back) and plain RW
(VOP2/eDP/PWM/EDP-TX PHY — guest RMW). Read hooks terminate guest polls: CRU CON6
bit15=1, PMU version=0x3588, eDP DET_STA/HPD/PLL-lock. Region quirks learned: EDP-TX
PHY is 0x2000 (u-boot binding), power-gate status reads 0 => "already on" fast path.
Exits pasted in plan.md.
