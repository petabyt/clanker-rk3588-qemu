# RK3568 DDR bring-up (experimental)

The current test proves that the image returns success with **synthetic**
controller and PHY responses, not that real DDR training is emulated.
It uses Rockchip's unmodified `bin/rk35/rk3568_ddr_1560MHz_v1.26.bin`
from `https://github.com/rockchip-linux/rkbin` commit
`3e288fe814e059dd06833495f845cab04ac20a5c`, SHA-256
`acb4e054298f0f0c8471dd3d45de07a1c77b67e687ec6e77ede365a087f1d95a`.
The binary is not redistributed here. For example, extract that path from
the pinned revision of a local rkbin checkout, then run:

```
make test-rk3568-ddr RK3568_DDR=/path/to/rk3568_ddr_1560MHz_v1.26.bin
```

The blob's entry code checks its execution address is `0xfdcc1000`. The
RK3568 TRM v1.3 table 1-2 identifies system SRAM at `0xfdcc0000` (64 KiB)
and PMU SRAM at `0xfdcd0000` (8 KiB). The test loader places the unmodified
image in system SRAM. `scripts/rk3568-ddr-probe.S` runs at `0xfdcd1d00`
and calls it with a stack in PMU SRAM and a zeroed 32-byte block in system
SRAM as x0; x1 is 8. These
registers and stack location are **experimental assumptions**, not a
verified BootROM handoff. The machine already supplies emulated RAM.

The vendor image prints its banner, four frequency-change phases and `out`,
then returns zero to the harness. The harness prints `DDR_PROBE_RETURN_OK`;
the test also requires an empty QEMU exception/error log. The PHY and
controller supply synthetic completion and lane status, the ECC scrub is
reported complete without actually being performed, and the 2 GiB test RAM
wraps into part of the next address range. The blob consequently reports an
incorrect 8 GiB geometry: its output must not be used as a memory-size
claim. OTP/OTP PHY, secure GRF and clock registers are simplified stubs.
There is no BootROM, SPL, or subsequent handoff to U-Boot in this test.

## Experimental SD chain

Build and run the combined image (requires `pyelftools` for binman, the
cross compiler, and the `atf` and `u-boot` Git checkouts):

```
make test-rk3568-sd RK3568_DDR=/path/to/rk3568_ddr_1560MHz_v1.26.bin
```

`scripts/build-rk3568-sd.sh` archives the committed U-Boot and TF-A trees
without modifying either checkout. It produces `build/rk3568/sd.img`: the
v2 idbloader at sector 0x40 and a FIT with upstream BL31 and U-Boot. The
build disables unused eMMC and SDMMC2 nodes, uses `ERROR_DEPRECATED=0` for
SPL's v1 TF-A parameters, and changes TF-A's RK3568 `need_en_crypto` initial
value to zero **in the temporary build tree only**. That avoids a core
warm-reset/remap sequence not implemented by this machine. Vendor BL31
instead reports I2C timeouts and enters its reset path.

`rk3568` recognizes a two-stage RKNS v2 header on SD unit 0 and requires
`-m 2G`. Its BootROM substitute reads both stages directly from the block
backend, executes DDR in SRAM, and stages SPL at address zero only if DDR
returns zero; SPL reads the FIT via the SDMMC0 device. This is not a
verified RK3568 BootROM ABI or a complete BootROM implementation. SDMMC0
and the PMU/GRF/firewall stubs are experimental; PMU CPU power status only
represents secondary CPUs initially off.

DDR training and ECC scrub remain synthetic. The blob incorrectly reports
8 GiB, so the handoff replaces the PMUGRF size fields with a synthetic 2 GiB
geometry and discards the inconsistent DDR-memory ATAG; U-Boot then reports
2 GiB and constructs banks from that size. This does not validate real
hardware geometry. The regression uses a snapshot of the SD image, checks
its base hash is unchanged, verifies the ordered stage banners, empty QEMU
guest-error log and `echo RK3568_HELLO` output. OP-TEE is not included.
