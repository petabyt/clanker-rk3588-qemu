# P16 — Boot-speed plan (perf.md)

Written 2026-10-13 from a full evidence pass over logs/p12-p15 + src/ + atf/.
Agreed order (user): SMP → timeout devices. (virtio-gpu was split out and
is now DONE — executed 2026-10-14, see ./gpu-plan.md RESULT.) Guest-side
Armbian service tuning was explicitly declined this round; network
(virtio-net+SLIRP) is in scope.

## STATUS BOARD (updated 2026-10-14)
| Item | State |
|------|-------|
| A (SMP/PSCI)      | DONE 2026-10-14 |
| B1 (virtio-net)   | DONE 2026-10-14 |
| B2 (IOMMU windows)| DONE 2026-10-14 — all 15 windows behavioral; 0 FORCE_RESET |
| B3 (pm-domain off)| REVERTED/DEFERRED — see B3 note below |
| B4 (rockchip-rng) | DONE 2026-10-14 — 0 reseed timeouts |
| B5 (i2c+hym8563)  | DONE 2026-10-14 — rtc0 registered, hctosys 2026-10-14, others fast -ENXIO |
| B6 (VOP2 dsp_hold)| OPEN (low priority, one-off 10-min greeter) |
| B7 (boot dance)   | OPEN (optional, 10-25s host wall) |
| virtio-gpu plan   | DONE 2026-10-14 (gpu-plan.md, exit bar met) |
| Full regression sweep | PENDING (Rule #8: appimage + smoke after every ./src change) |
| CQE-enable flake  | OPEN (2/4 silent fails, needs dedicated pass) |

### What's left, in order
1. **B6 VOP2 dsp_hold** — small.
2. **B7 boot dance trim** — optional host-side polish.
3. **CQE-enable flakiness** — dedicated pass (pre-existing, 2/4 silent fails).
4. **Full regression sweep** — `make appimage` + smoke + run-linux* /
   regress / stock chain still owed for the B4+B5 ./src changes
   (SKIPPED this round per user; needed before any commit).
5. **B3 re-attempt (optional)** — only if the 10ms power_off timeouts become
   a perceived delay; approach documented in rk3588-stubs.c comment (skip
   pwr tracking for the probe-time need_regulator write).

Baseline numbers (Armbian 26.8.1 KDE/GNOME, TCG, 1 vCPU effective):
- `smp: Bringing up secondary CPUs` 0.07s → `Brought up 1 node, 1 CPU`
  36.32s — 7 failed CPU_ON at 5.17s intervals (logs/p14-armbian-cleanboot.txt:164-179)
- systemd storm 71→236s serialized on CPU0 (apport 162s, ModemManager 158s,
  armbian-resize ~120s, ldconfig 105s, NM-wait-online 75s)
- driver-probe tail (pm-domain off failures + iommu FORCE_RESET) runs
  40→414s; login prompt ~180-260s; GDM ≥265s
- firstboot adds "Waiting for network" + 6× "Waiting for local connection"
  ≈ 75s (logs/p14-armbian-cleanboot.txt:973-987)
- host boot dance (U-Boot autoboot CR flood) ≈ 10-25s before "Starting kernel"

Host: 24 threads, TCG only, MTTCG auto-enabled for ARM
(qemu/accel/tcg/tcg-all.c:111-131) — no accel work needed, 7 idle vCPU
threads + most host threads wasted today.

---

## Workstream A — Multicore (SMP) — DONE 2026-10-14 (see plan.md P16 exit block)

Blocker (plan.md 1952): "PSCI CPU_ON still fails 8x, kernel continues 1 CPU".
TF-A IS the PSCI provider (bl31.elf from ./atf, plat/rockchip/rk3588); QEMU
psci-conduit never set (by design — keep it that way).

Root causes (stacked):
1. PMU CPU power controller unmodelled. TF-A `rockchip_soc_cores_pwr_dm_on`
   (atf/plat/rockchip/rk3588/drivers/pmu/pmu.c:818-838) writes, hiword-mask
   semantics: `PMU2_CPU_AUTO_PWR_CON(cpu) = 0xFD8D0000+0x8010+4*cpu` with
   raw `0x00010000` (clear core_pm_en) then `0x00080008` (set
   core_pm_sft_wakeup_en), then returns SUCCESS unconditionally.
   `rk_stub_pmu_write` (src/rk3588-stubs.c:184-203) handles only
   0x8154/0x810c/0x8110 — the 0x8010+4i writes land in dead backing store.
   Result: exactly the observed "failed in unknown state : 0x0".
2. MPIDR layout wrong. src/rk3588.c:995-996 assigns affinity i<4 → 0x0-0x3,
   i>=4 → 0x100|（i-4) ("two clusters of four"). The real part, every guest
   DTB (stock + Armbian: reg 0x0,0x100…0x700), and TF-A's
   `plat_core_pos_by_mpidr` assume mpidr = i<<8. Consequences: vCPU4
   (mpidr 0x100) collides with vCPU1 in TF-A's index space; mpidr 0x200-0x700
   have no vCPU; GICR_TYPER affinities (from mp-affinity,
   hw/intc/arm_gicv3_common.c:462-469) don't cover CPUs 2-7.
3. Secondary entry path absent. Hardware wakes the core at the PMUSRAM
   alias 0xFFFF0000 → `pmu_cpuson_entrypoint` @ 0xFF100000 (.text_pmusram,
   ALREADY loaded into QEMU RAM via the bl31 ELF + sram1 region,
   src/rk3588.c:85-87, 969-971). QEMU has no code to start the
   start-powered-off vCPUs (they are parked via arm_cpu_reset,
   hw/core/cpu-common.c:114); QEMU's own PSCI start path is disabled.

### Steps
A1. src/rk3588.c:995-996 — `affinity = (uint64_t)i << 8` (0x000…0x700).
    Keep all CPUs cortex-a76 (kernel tolerates DTB midr mismatch today;
    per-cluster A55/A76 types = later fidelity item, needs TF-A MIDR
    checks reviewed first).
A2. src/rk3588-stubs.c — extend `rk_stub_pmu_write` for
    `0x8010+4*i` (i=0..7), decode hiword mask like the 0x8154 handler.
    On core_pm_sft_wakeup_en set (bit3), call new machine helper
    `rk3588_pmu_cpu_wake(cpu)` (src/rk3588.c) that mirrors QEMU's own
    arm_set_cpu_on_async_work (qemu/target/arm/arm-powerctl.c:56-83),
    thread-safe via async_run_on_cpu:
      cpu_reset(target);
      arm_emulate_firmware_reset(target, 3);   /* EL3 firmware mode */
      target->halted = 0;
      arm_set_cpu_power_state(ARM_CPU, PSCI_ON);
      cpu_set_pc(target, 0xFF100000);          /* pmusram entry */
    Track per-CPU on/off state (reset with the system-reset hook).
    TF-A then runs its own warm path: pmu_cpuson_entrypoint →
    platform_cpu_warmboot (reads cpuson_flags/entry_point in BL31 .data
    0x60088/0x600c8) → pwr_domain_on_finish (clears AUTO_PWR_CON bits via
    the stub — harmless).
A3. Reset hygiene: clear the wake-state array in the existing
    qemu_register_reset hook (src/rk3588.c:1071-1073). start-powered-off
    already re-parks secondaries on cpu_reset.
A4. CPU_OFF / CPU_SUSPEND: NOT modelled this round. Derived DTBs keep
    `cpu-idle-states` deleted (TF-A cortex_a76_core_pwr_dwn reads
    CPUECTLR_EL3 s3_0_c15_c2_7 → UNDEF under TCG, P13b) → guest idles via
    WFI only. Optional later: AUTO_PWR_CON core_pwr_wfi (0x0003_0001) →
    halt vCPU.
A5. Thread-safety: confirm MTTCG MMIO/BQL situation for the helper (use
    async_run_on_cpu — safe from any thread). Watch the known-flaky
    CQE-enable path (plan.md P14 caveat: 2/4 silent fails) under 8-thread
    timing.
A6. Do NOT wire QEMU PSCI (psci-conduit) — TF-A stays the faithful
    provider (SCMI also runs through it, P13).

### Exit evidence (A)
- `make run-linux-emmc`: 35 PASS / 0 FAIL / LINUX-OK / HS400ES /
  `Command Queue Engine enabled` (twice — flaky check).
- New: GICv3 "found redistributor region" ×8 + `smp: Brought up 8 CPUs`
  in an Armbian boot log; no CPUx "failed to come online".
- `make regress` (U-Boot + EDK2) green — affinity change touches all boots.
- `-d unimp,guest_errors` clean (0 new MMIO).
- Before/after wall-clock table (kernel start → login prompt, → sddm).

---

## Workstream B — Timeout devices (ordered by measured cost)

### B1. virtio-net + user SLIRP (~75-150s/boot) — DONE 2026-10-14
Note: SPI 95 (0x5f) is taken by video-codec@fdc38000 in the stock DTB;
the net transport uses SPI 98 (0x62) instead.  eth0 verified at
10.0.2.15/24 (SLIRP), virtio_net bound, NM-wait-online inactive.
Evidence: NO NIC at all → NetworkManager-wait-online 75s every boot
(p14-armbian-gdm.txt:830) + firstboot "Waiting for network" + 6× IP
retries ≈75s (p14-armbian-cleanboot.txt:973-987). CONFIG_SLIRP is #undef
(build/qemu/config-host.h:344); libslirp-dev installable (4.8.0-1+deb13u1);
configure auto-detects → NO Makefile/qemu.patch change. No /dev/net/tun in
this container → SLIRP is the only backend anyway.
Steps:
- apt-get install libslirp-dev; rebuild (make qemu).
- src/rk3588.c: add 4th virtio-mmio transport 0xfb003000, SPI 95, inside
  the existing vgpu=on loop (rk3588_create_virtio_gpu).
- DTB: add `net@fb003000` virtio,mmio node (dma-coherent, <0 95 4 0>) to
  images/armbian/rk3588s-orangepi-5-vgpu-boot.dtb. The decompiled source
  was /tmp/opencode/armbian-boot.dts — COMMIT a copy under dts/ for
  reproducibility before regenerating (also the Q2 cosmetic renames can
  ride along).
- scripts/run-desktop.py: put `-device virtio-net-device,netdev=net0`
  FIRST in the -device list (attach order is REVERSED: first CLI device →
  highest transport → 0xfb003000, preserving gpu@fb002000 /
  keyboard@fb001000 / tablet@fb000000 node meaning) +
  `-netdev user,id=net0`.
- Makefile run-linux* targets: leave without net (busybox initramfs, no NM).
Exit: guest `ip link` shows eth0 up (10.2.0.15 via SLIRP), NM-wait-online
completes in ~1s, 0 "Waiting for local connection" lines, boot-time table row.

### B2. Behavioral IOMMU for all windows — DONE 2026-10-14
Evidence: `rk_iommu <addr> FORCE_RESET command timed out` ×10-13 per boot
(codec fdb50xx, npu fdab9/90, vdec, av1, i2s… storage stubs); only vop_mmu
(fdd97e00/fdd97f00) got the behavioral model (P14 §1). Failed iommu probe
→ deferred-probe retries = the 40-414s async tail competing for CPU.
Implemented: `rk3588_create_iommu_window(uint64_t base, uint64_t size)` in
src/rk3588-iommu.c; 15 windows created in rk3588_create_display_devices()
(src/rk3588.c iommus[] table) mapped via
memory_region_add_subregion_overlap(..., priority 1) over the storage
stubs (need address-spaces.h). av1-iommu (0xfdca0000, verisilicon compat)
left as storage stub; SMMU-v3 (fc900000/fcb00000) not modeled.
Exit: 0 FORCE_RESET timeout lines on full Armbian boots (g0-baseline3,
g4-final); login ~90-100s (was 233s+ hang); no new unimp/guest_errors.

### B3. pm-domain off path — REVERTED 2026-10-14 (deferred)
Evidence: `rockchip-pm-domain ... failed to set domain 'X' off` ×28 per
boot (10ms poll each — cheap individually; spam + honest-state issue).
The stub answers +0x180 status=0 "powered" always (P13 §3), so power_on
short-circuits but power_off always fails.
Attempted and REVERTED: implementing pwr_off tracking made the probe-time
power_off write (pm-domains.c:888-900, need_regulator domains) report
success → start_off=true → pm_genpd_add_subdomain() returned -EINVAL for
npu→nputop → whole rockchip-pm-domain probe failed (`probe with driver
rockchip-pm-domain failed with error -22`). Reverted to always-on status
reads with explanatory comment in rk3588-stubs.c (which also documents the
re-attempt idea: skip pwr tracking for the probe-time need_regulator write,
or fix the genpd interaction). Power-off timeouts (10ms each) remain but
are NOT the hang cause — B2 was. Not worth re-attempting unless the
timeout spam becomes a perceived delay.

### B4. rockchip-rng (fe378000) — DONE 2026-10-14
Evidence: `rockchip-rng fe378000.rng: timed out waiting for hwrng to
reseed` then ~10.3s later rtc probe fails (p14-armbian-gdm.txt:807→824).
Root cause: rk3588_rng_init() polls TRNG_V1_STAT (0x04) until SEEDED
(bit9) is set with GENERATING/RESEEDING clear; the stub read back 0, so
the poll never terminated (-ETIMEDOUT after 10ms).
Implemented (src/rk3588-stubs.c rk_stub_rng_read):
- STAT (0x04) reads SEEDED (bit9) set, idle -> init poll exits at once.
- RAND0..RAND7 (0x20..0x3c) return xorshift bytes seeded by host
  g_get_monotonic_time() -> rk3588_rng_read's ISTAT poll (RAND_RDY, bit0,
  already faked) + 32-byte copy yields non-zero data.
- ISTAT (0x14) RAND_RDY + VERSION (0xf0)=0x46bc already present.
- Driver polls (no IRQ) -> no SPI wiring needed.  No write hook required
  (RAND data regenerated on read).
Exit: 0 reseed timeouts (p16-b4-rng2.txt), rng-tools started against
/dev/hwrng, boot reached login prompts, no new unimp/guest_errors.

### B5. I2C completion + rtc-hym8563 — DONE 2026-10-14
Evidence (original): `rtc-hym8563 6-0051: could not init device, -110`
(probe fails; i2c core ETIMEDOUT because Linux rk_i2c is interrupt driven
and the model never raised the controller SPI; the passive
rk_stub_i2c_read faked IPD done bits for EDK2/U-Boot polling only).
Implemented (replaces the plan's "write hook" sketch — passive stubs have
no per-block context and no GIC line, so this needed a real device):
- src/rk3588-i2c.c — behavioural rk3x sysbus device, 9 instances
  (rk3588.c rk3588_create_i2cs), passive i2c stubs removed. Reacts to the
  driver's register writes: CON+START -> IPD.START, MTXCNT -> do_tx (MBTF
  or NAKRCV), MRXCNT -> do_rx (MBRF or NAKRCV), CON+STOP -> IPD.STOP,
  IPD is W1C; IRQ line = level (IPD & IEN). Satisfies both the Linux
  interrupt handshake and U-Boot/EDK2 IPD polling.
- HYM8563 slave at 0x51 on i2c6 only (has-rtc prop): control regs storage,
  calendar regs 0x02..0x08 served live from host wall clock as BCD.
- All other addresses/bus -> NACK -> fast -ENXIO (fan53555 probes went
  from 3x -110 timeouts to 3x -ENXIO in ~1ms).
Gotchas found the hard way:
- GIC wiring: the board passes the RAW DTB SPI number (0x13d..0x145) to
  qdev_get_gpio_in — QEMU's gicv3_set_irq adds GIC_INTERNAL itself
  (hw/intc/arm_gicv3.c:373; uarts 331/333 confirm). Using 32+SPI raised
  unowned INTIDs -> silent -ETIMEDOUT until fixed.
- QEMU_CLOCK_REALTIME is NOT wall time here: qemu_clock_get_ns(REALTIME)
  returns get_clock() = CLOCK_MONOTONIC (uptime); first hctosys set the
  guest epoch to 1970-01-04+3.1d uptime ("2070-01-04"). Wall time is
  get_clock_realtime() (what QEMU_CLOCK_HOST maps to) — the slave uses it.
- qemu.patch regenerated (added rk3588-i2c.c to hw/arm/meson.build);
  dev headers libslirp-dev + libvirglrenderer-dev reinstalled (they had
  vanished from the container, breaking the link step).
Exit evidence (logs/p16-b5-verify.txt, serial-driven guest shell):
  rtc-hym8563 6-0051: registered as rtc0
  rtc-hym8563 6-0051: setting system clock to 2026-10-14T22:39:05 UTC
  /dev/rtc0 exists; /proc/driver/rtc rtc_date 2026-10-14;
  guest `date -u` = Wed Oct 14 22:39:11 UTC 2026 (matches host)
  0 i2c/rtc -110 lines (remaining 3: rk8xx-spi + 2x EHCI, pre-existing);
  0 hwrng reseed timeouts (B4 intact); boot reaches login prompts.

### B6. VOP2 dsp_hold (small)
Evidence: one `rockchip-drm: wait for vp0 dsp_hold timeout` ~10 min into
greeter (plan.md 5e; suspected DPMS blank path).
Steps: src/rk3588-vop2.c — model the DSP_HOLD_DONE status the kernel polls
on disable/blank. Low priority; do after B2-B5.
Exit: no dsp_hold timeout lines on an idle-20-min greeter boot.

### Optional B7 (host-side, cheap): trim the U-Boot autoboot dance in
run-desktop.py (marker-driven, fewer blind CRs) — 10-25s host wall today.
Only if everything else is green.

---

## Wrap-up obligations (rules of engagement)
- Document as P16 in plan.md (+ SCOPE update line); reference this file.
- Full regression sweep after src changes: run-linux, run-linux-emmc,
  make regress, sd-own-bl31, stock chain, Armbian desktop boot.
- `make appimage` + smoke boot self-exit rc=0 after EVERY ./src change
  (Rule #8).
- No commits (Rule #5); milestone claims need pasted exit output (Rule #6).

## Risks / unknowns
- MTTCG exposes stub register files to 8 threads — races mostly benign
  (hooks are pure), but the wake helper must use async_run_on_cpu; verify
  empirically; watch CQE-enable flakiness.
- TF-A secondary-entry (0xFF100000) may need debugging: hot-PC histogram
  via `-d exec` + objdump bl31.elf (existing recipe, plan.md P8).
- Affinity fix changes GICR discovery — any boot path with old expectations
  (U-Boot smp, EDK2) re-tested via `make regress`.
- virtio-net makes the guest "have a network": armbian firstboot behavior
  changes (it will find one — good) and NM adds eth0; SLIRP gives
  outbound-only networking.
- PMU stub changes interact with P14 idle-ack mirroring — retest
  rockchip-pm-domain paths carefully.
