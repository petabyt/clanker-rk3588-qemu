#ifndef RK3588_STUBS_H
#define RK3588_STUBS_H

#include "system/memory.h"
#include "hw/core/irq.h"

/*
 * Register-file stubs for the RK3588 blocks the ./rk firmware pokes
 * (CRU/PMU/IOC/GRF/SGRF/GPIO/PWM/VOP2/eDP...).  See rk3588-stubs.c for
 * the read/write semantics each class needs.
 */
void rk3588_create_stub_regions(MemoryRegion *sysmem, DeviceState *gic);

/*
 * Set by rk3588.c when -bios carries a TF-A (BL31) ELF.  Gates the
 * ATF-only shutdown/reset hooks so the ./rk firmware paths keep their
 * exact prior behaviour.
 */
extern bool rk3588_atf_mode;
/* Enable the experimental BootROM's virtual secure OTP. */
extern bool rk3588_real_bootrom_mode;
/* Experimental VDEC0 power-domain and restricted decoder behavior. */
extern bool rk3588_vdec0_power;

/*
 * PCIe link state reported by the APB client's LTSSM_STATUS (+0x300).
 * Default true (the P10 stock-EDK2 path needs link-up so segment init
 * completes at once); the Linux audit runs pass -M rk3588,pcie-link=off
 * to model the empty slot the VM actually has, which makes
 * dw_pcie_wait_for_link() report "Device not found" / -ENODEV instead of
 * inventing a Gen.0 x0 link.  Set by rk3588.c before stub init.
 */
extern bool rk3588_pcie_link_up;

/* Static HDMI0 hotplug level, selected by the machine before boot. */
extern bool rk3588_hdmi_connected;

/*
 * Machine hook (rk3588.c): the PMU stub calls this when TF-A writes
 * core_pm_sft_wakeup_en to PMU2_CPU_AUTO_PWR_CON(cpu) (CPU_ON, perf.md A2).
 */
void rk3588_pmu_cpu_wake(int cpu_index);

/*
 * Machine hook (rk3588.c), P20: the modelled PMU core power-cycle for
 * PSCI CPU_SUSPEND.  Same reset-into-pmusram sequence as CPU_ON, but
 * valid for every core (the boot core suspends too) and repeatable, so it
 * bypasses the "already on" bookkeeping of rk3588_pmu_cpu_wake().
 */
void rk3588_pmu_cpu_resume(int cpu_index);
void rk3588_pmu_cpu_suspend(int cpu_index);
bool rk3588_pmu_cpu_has_wakeup(int cpu_index);

/*
 * Called from the machine's reset handler: drop any suspend-wake requests
 * that were armed before the reset (after a reset the secondaries are
 * start-powered-off again and must not be woken without a CPU_ON).
 */
void rk3588_pmu_suspend_reset(void);

/*
 * Genbook touchpad line (hid-over-i2c @ i2c5 -> GPIO1_PD6 -> SPI 278).
 * Set by rk3588.c (the GIC input); driven by the i2c-hid slave via
 * rk3588_touchpad_set_line(): pin 30 of gpio1 goes low while a report
 * is pending, and the GIC line follows (level, dts IRQ_TYPE_LEVEL_LOW
 * at the pin / LEVEL_HIGH at the bank).
 */
extern qemu_irq rk3588_touchpad_gic_line;
void rk3588_touchpad_set_line(int level);

#endif
