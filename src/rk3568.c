/*
 * Minimal RK3568 machine for firmware bring-up.  Firmware must be loaded
 * explicitly; DDR controller and PHY responses are synthetic boot stubs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qobject/qlist.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/reset.h"
#include "hw/arm/bsa.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/intc/arm_gic_common.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/sd/sd.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/block-backend-io.h"
#include "system/physmem.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
#include "dw-apb-uart.h"
#include "rk3588-i2c.h"
#include "rk3588-timer.h"
#include "rk3588-dwmshc.h"

#define RK3568_SPI_COUNT 480
static uint32_t *rk3568_ddrgrf_regs;

/* Experimental SD BootROM substitute. The real RK3568 call ABI is unknown. */
typedef struct RK3568Boot {
    uint8_t *ddr, *spl;
    size_t ddr_len, spl_len;
    bool staged;
    uint32_t *pmugrf;
} RK3568Boot;

static uint64_t rk3568_boot_return_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    RK3568Boot *boot = opaque;
    ARMCPU *cpu = ARM_CPU(first_cpu);

    if (!boot->staged) {
        if ((uint32_t)cpu->env.xregs[0] != 0) {
            /* DDR returned an error: stay here rather than booting SPL. */
            return 0x14000000;
        }
        boot->staged = true;
        /* Advertise the supplied 2 GiB, not the blob's synthetic 8 GiB. */
        /* DDR3: one rank, row 16, col 10, eight banks, 32-bit. */
        boot->pmugrf[0x208 / 4] = 0x62c0;
        boot->pmugrf[0x20c / 4] = 0;
        /* Discard the blob's inconsistent DDR_MEM tag, preserving other tags.
         * U-Boot then builds its bank list from the synthetic 2 GiB size. */
        for (hwaddr tag = 0x1fe000; tag + 8 <= 0x200000;) {
            uint8_t hdr[8];
            uint32_t words, magic;

            physical_memory_read(tag, hdr, sizeof(hdr));
            words = ldl_le_p(hdr);
            magic = ldl_le_p(hdr + 4);
            if (words < 2 || words > (0x200000 - tag) / 4) {
                break;
            }
            if (magic == 0x54410052) {
                stl_le_p(hdr + 4, 0); /* ATAG_NONE */
                physical_memory_write(tag + 4, hdr + 4, 4);
            }
            tag += words * 4;
        }
        physical_memory_write(0, boot->spl, boot->spl_len);
        cpu->env.xregs[0] = 0xfdcc0000;
        cpu->env.xregs[1] = 8;
        cpu->env.xregs[30] = 0;
    }
    return 0xd61f03c0; /* br x30 */
}

static const MemoryRegionOps rk3568_boot_return_ops = {
    .read = rk3568_boot_return_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void rk3568_boot_reset(void *opaque)
{
    RK3568Boot *boot = opaque;
    ARMCPU *cpu = ARM_CPU(first_cpu);
    uint8_t bootdata[32] = { 0 };

    physical_memory_write(0xfdcc0000, bootdata, sizeof(bootdata));
    physical_memory_write(0xfdcc1000, boot->ddr, boot->ddr_len);
    boot->staged = false;
    cpu->env.xregs[0] = 0xfdcc0000;
    cpu->env.xregs[1] = 8;
    cpu->env.xregs[30] = 0xffff0000;
    cpu->env.xregs[31] = 0xfdcd1d00;
    cpu_set_pc(CPU(cpu), 0xfdcc1000);
}

static void rk3568_boot_from_sd(MachineState *ms, uint32_t *pmugrf)
{
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;
    uint8_t header[512];
    uint64_t off[2], len[2];
    int64_t capacity;
    RK3568Boot *boot;
    MemoryRegion *ret;

    if (!blk || (capacity = blk_getlength(blk)) < 0x8200 ||
        blk_pread(blk, 0x8000, sizeof(header), header, 0) < 0 ||
        ldl_le_p(header) != 0x534e4b52) {
        return;
    }
    for (unsigned i = 0; i < 2; i++) {
        unsigned entry = 0x78 + i * 0x58;

        off[i] = 0x8000 + (uint64_t)lduw_le_p(header + entry) * 512;
        len[i] = (uint64_t)lduw_le_p(header + entry + 2) * 512;
        if (!len[i] || off[i] > capacity || len[i] > capacity - off[i]) {
            error_report("rk3568: invalid SD idbloader image bounds");
            exit(1);
        }
    }
    if (lduw_le_p(header + 0xa) != 2 || len[0] > 60 * KiB ||
        len[1] > ms->ram_size || ms->ram_size != 2 * GiB) {
        error_report("rk3568: SD boot requires a two-stage v2 image and 2 GiB RAM");
        exit(1);
    }
    boot = g_new0(RK3568Boot, 1);
    boot->pmugrf = pmugrf;
    boot->ddr_len = len[0];
    boot->spl_len = len[1];
    boot->ddr = g_malloc(len[0]);
    boot->spl = g_malloc(len[1]);
    if (blk_pread(blk, off[0], len[0], boot->ddr, 0) < 0 ||
        blk_pread(blk, off[1], len[1], boot->spl, 0) < 0) {
        error_report("rk3568: could not read SD idbloader");
        exit(1);
    }
    ret = g_new0(MemoryRegion, 1);
    memory_region_init_io(ret, OBJECT(ms), &rk3568_boot_return_ops, boot,
                          "rk3568.boot-return", 4);
    memory_region_add_subregion(get_system_memory(), 0xffff0000, ret);
    qemu_register_reset(rk3568_boot_reset, boot);
}

/* GRF write-mask registers used by the early UART pinmux. */
static uint64_t rk3568_grf_read(void *opaque, hwaddr offset, unsigned size)
{
    uint32_t *regs = opaque;

    return regs[offset / 4];
}

static void rk3568_grf_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    uint32_t *regs = opaque;
    uint32_t mask = value >> 16;

    regs[offset / 4] = (regs[offset / 4] & ~mask) | (value & mask);
}

static const MemoryRegionOps rk3568_grf_ops = {
    .read = rk3568_grf_read,
    .write = rk3568_grf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* RK3568 PLL CON1 bit 10 reports lock (clk_rk3568.c). */
static uint64_t rk3568_cru_read(void *opaque, hwaddr offset, unsigned size)
{
    uint64_t value = rk3568_grf_read(opaque, offset, size);

    if (offset < 0xc0 && offset % 0x20 == 4) {
        value |= 1 << 10;
    }
    return value;
}

static const MemoryRegionOps rk3568_cru_ops = {
    .read = rk3568_cru_read,
    .write = rk3568_grf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t rk3568_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    return offset == 0x78 ? 0x01000c2b :
           rk3568_grf_read(opaque, offset, size);
}

static const MemoryRegionOps rk3568_gpio_ops = {
    .read = rk3568_gpio_read,
    .write = rk3568_grf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* Secondary CPUs start powered off in this machine. */
static uint64_t rk3568_pmu_read(void *opaque, hwaddr offset, unsigned size)
{
    return offset == 0x31c ? 0xe : rk3568_grf_read(opaque, offset, size);
}

static const MemoryRegionOps rk3568_pmu_ops = {
    .read = rk3568_pmu_read,
    .write = rk3568_grf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* Empty synthetic OTP: reads complete immediately and return zero. */
static uint64_t rk3568_otp_read(void *opaque, hwaddr offset, unsigned size)
{
    return offset == 0x304 ? 6 : 0;
}

static void rk3568_otp_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
}

static const MemoryRegionOps rk3568_otp_ops = {
    .read = rk3568_otp_read,
    .write = rk3568_otp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* Experimental DDR PHY/controller: ordinary read/write register storage. */
static void rk3568_ddr_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    uint32_t *regs = opaque;

    regs[offset / 4] = value;
}

static const MemoryRegionOps rk3568_ddr_ops = {
    .read = rk3568_grf_read,
    .write = rk3568_ddr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* PHY training reports immediately complete for both observed modes. */
static uint64_t rk3568_ddrphy_read(void *opaque, hwaddr offset, unsigned size)
{
    uint32_t *regs = opaque;

    if (offset == 0x1f0) {
        return 0x81;
    }
    if (offset == 0x20c) {
        /* Reflect enabled byte lanes in both training-status fields. */
        return (regs[0] & 0x1f00) | ((regs[0] >> 8) & 0x1f);
    }
    return rk3568_grf_read(opaque, offset, size);
}

static const MemoryRegionOps rk3568_ddrphy_ops = {
    .read = rk3568_ddrphy_read,
    .write = rk3568_ddr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* uMCTL2 SWSTAT acknowledges SWCTL immediately; no real DRAM timing. */
static uint64_t rk3568_ddrc_read(void *opaque, hwaddr offset, unsigned size)
{
    uint32_t *regs = opaque;

    if (offset == 0x324) {
        return regs[0x320 / 4] & 1;
    }
    if (offset == 0x04) {
        /* Follow PWRCTL self-refresh request; otherwise normal mode. */
        return regs[0x30 / 4] & 0x20 ? 0x23 : 1;
    }
    if (offset == 0xf28) {
        return 2; /* synthetic ECC memory scrub completed, never busy */
    }
    if (offset == 0x1bc) {
        /* DDR-GRF bit 2 gates the synthetic DFI initialization handshake. */
        return !(rk3568_ddrgrf_regs[0] & 4);
    }
    return regs[offset / 4];
}

static void rk3568_ddrc_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    /* MRCTRL0.mr_wr launches an immediately completed mode-register write. */
    if (offset == 0x10) {
        value &= ~(1u << 31);
    }
    rk3568_ddr_write(opaque, offset, value, size);
}

static const MemoryRegionOps rk3568_ddrc_ops = {
    .read = rk3568_ddrc_read,
    .write = rk3568_ddrc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint32_t *rk3568_grf_init(MachineState *ms, hwaddr base, unsigned size,
                                 const char *name, const MemoryRegionOps *ops)
{
    MemoryRegion *region = g_new0(MemoryRegion, 1);
    uint32_t *regs = g_new0(uint32_t, size / 4);

    memory_region_init_io(region, OBJECT(ms), ops, regs, name,
                          size);
    memory_region_add_subregion(get_system_memory(), base, region);
    return regs;
}

static void rk3568_init(MachineState *ms)
{
    DeviceState *gic, *uart, *i2c, *stimer;
    uint32_t *pmugrf;
    MemoryRegion *sram = g_new0(MemoryRegion, 1);
    MemoryRegion *pmu_sram = g_new0(MemoryRegion, 1);
    QList *regions = qlist_new();
    const int timer_irq[] = {
        [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
        [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
        [GTIMER_HYP] = ARCH_TIMER_NS_EL2_IRQ,
        [GTIMER_SEC] = ARCH_TIMER_S_EL1_IRQ,
        [GTIMER_HYPVIRT] = ARCH_TIMER_NS_EL2_VIRT_IRQ,
        [GTIMER_S_EL2_PHYS] = ARCH_TIMER_S_EL2_IRQ,
        [GTIMER_S_EL2_VIRT] = ARCH_TIMER_S_EL2_VIRT_IRQ,
    };

    if (ms->ram_size > 0xf0000000ULL) {
        error_report("rk3568: RAM must not exceed 3840 MiB");
        exit(1);
    }
    memory_region_add_subregion(get_system_memory(), 0, ms->ram);
    if (ms->ram_size == 2 * GiB) {
        /* Synthetic 2 GiB DRAM wraps for the DDR capacity-detection probes. */
        MemoryRegion *wrap = g_new0(MemoryRegion, 1);

        memory_region_init_alias(wrap, OBJECT(ms), "rk3568.ram-wrap", ms->ram,
                                 0, 0x70000000);
        memory_region_add_subregion(get_system_memory(), 0x80000000, wrap);
    }
    /* RK3568 TRM v1.3, table 1-2: system and PMU SRAM. */
    memory_region_init_ram(sram, NULL, "rk3568.sram", 64 * KiB,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0xfdcc0000, sram);
    memory_region_init_ram(pmu_sram, NULL, "rk3568.pmu-sram", 8 * KiB,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0xfdcd0000, pmu_sram);
    rk3568_grf_init(ms, 0xfdc00000, 0x10000, "rk3568.pmusgrf", &rk3568_grf_ops);
    rk3568_grf_init(ms, 0xfdc10000, 0x10000, "rk3568.syssgrf", &rk3568_grf_ops);
    pmugrf = rk3568_grf_init(ms, 0xfdc20000, 0x10000,
                             "rk3568.pmugrf", &rk3568_grf_ops);
    rk3568_grf_init(ms, 0xfdc30000, 0x10000, "rk3568.cpu-grf", &rk3568_grf_ops);
    rk3568_grf_init(ms, 0xfdcb0000, 0x10000, "rk3568.edp-phy-grf", &rk3568_grf_ops);
    rk3568_grf_init(ms, 0xfdd90000, 0x10000, "rk3568.pmu", &rk3568_pmu_ops);
    rk3568_ddrgrf_regs = rk3568_grf_init(ms, 0xfdc40000, 0x10000,
                                         "rk3568.ddr-grf", &rk3568_grf_ops);
    rk3568_grf_init(ms, 0xfdc60000, 0x10000, "rk3568.grf", &rk3568_grf_ops);
    rk3568_grf_init(ms, 0xfdd18000, 0x4000, "rk3568.sgrf", &rk3568_grf_ops);
    rk3568_grf_init(ms, 0xfdd00000, 0x1000, "rk3568.pmucru", &rk3568_cru_ops);
    rk3568_grf_init(ms, 0xfdd10000, 0x8000, "rk3568.secure-cru", &rk3568_cru_ops);
    rk3568_grf_init(ms, 0xfdd20000, 0x1000, "rk3568.cru", &rk3568_cru_ops);
    rk3568_grf_init(ms, 0xfe38c000, 0x4000, "rk3568.otp", &rk3568_otp_ops);
    rk3568_grf_init(ms, 0xfe3a0000, 0x8000, "rk3568.secure-otp", &rk3568_otp_ops);
    rk3568_grf_init(ms, 0xfe880000, 0x1000, "rk3568.otp-phy", &rk3568_grf_ops);
    rk3568_grf_init(ms, 0xfe800000, 0x10000, "rk3568.ddr-phy", &rk3568_ddrphy_ops);
    rk3568_grf_init(ms, 0xfe200000, 0x10000, "rk3568.ddr-firewall", &rk3568_ddr_ops);
    rk3568_grf_init(ms, 0xfe250000, 0x10000, "rk3568.ddr-controller", &rk3568_ddrc_ops);
    /* DDR firmware programs this undocumented window after PHY training. */
    rk3568_grf_init(ms, 0xfe100000, 0x10000, "rk3568.ddr-unknown", &rk3568_ddr_ops);
    rk3568_grf_init(ms, 0xfdd60000, 0x1000, "rk3568.gpio0", &rk3568_gpio_ops);
    for (unsigned i = 0; i < 4; i++) {
        rk3568_grf_init(ms, 0xfe740000 + i * 0x10000, 0x1000,
                        "rk3568.gpio", &rk3568_gpio_ops);
    }
    for (unsigned i = 0; i < ms->smp.cpus; i++) {
        Object *cpu = object_new(ms->cpu_type);

        object_property_set_int(cpu, "cntfrq", 24000000, &error_fatal);
        object_property_set_int(cpu, "mp-affinity", i, &error_fatal);
        object_property_set_bool(cpu, "has_el3", true, &error_fatal);
        if (i) {
            object_property_set_bool(cpu, "start-powered-off", true,
                                     &error_fatal);
        }
        qdev_realize(DEVICE(cpu), NULL, &error_fatal);
    }
    gic = qdev_new(gicv3_class_name());
    qdev_prop_set_uint32(gic, "revision", 3);
    qdev_prop_set_uint32(gic, "num-cpu", ms->smp.cpus);
    qdev_prop_set_uint32(gic, "num-irq", RK3568_SPI_COUNT + GIC_INTERNAL);
    qdev_prop_set_bit(gic, "has-security-extensions", true);
    qlist_append_int(regions, ms->smp.cpus);
    qdev_prop_set_array(gic, "redist-region-count", regions);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 0, 0xfd400000);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 1, 0xfd460000);
    for (unsigned i = 0; i < ms->smp.cpus; i++) {
        DeviceState *cpu = DEVICE(qemu_get_cpu(i));
        unsigned base = RK3568_SPI_COUNT + i * GIC_INTERNAL;

        for (unsigned j = 0; j < ARRAY_SIZE(timer_irq); j++) {
            qdev_connect_gpio_out(cpu, j, qdev_get_gpio_in(gic, base + timer_irq[j]));
        }
        qdev_connect_gpio_out_named(cpu, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(gic, base + ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpu, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(gic, base + VIRTUAL_PMU_IRQ));
        for (unsigned j = 0; j < 6; j++) {
            static const int inputs[] = { ARM_CPU_IRQ, ARM_CPU_FIQ, ARM_CPU_VIRQ,
                                          ARM_CPU_VFIQ, ARM_CPU_NMI, ARM_CPU_VINMI };
            sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + j * ms->smp.cpus,
                               qdev_get_gpio_in(cpu, inputs[j]));
        }
    }
    uart = qdev_new(TYPE_DW_APB_UART);
    if (serial_hd(0)) {
        qdev_prop_set_chr(uart, "chardev", serial_hd(0));
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, 0xfe660000);
    sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0, qdev_get_gpio_in(gic, 118));
    /* i2c0 has no attached PMIC yet; absent addresses NACK. */
    i2c = qdev_new(TYPE_RK3588_I2C);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(i2c), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(i2c), 0, 0xfdd40000);
    sysbus_connect_irq(SYS_BUS_DEVICE(i2c), 0, qdev_get_gpio_in(gic, 46));
    /* Secure timer 1: same Rockchip down-counter register layout. */
    stimer = qdev_new(TYPE_RK3588_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(stimer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(stimer), 0, 0xfdd1c020);
    /* RK3568 SDMMC0 uses the same DesignWare controller register layout. */
    {
        DeviceState *sd = qdev_new(TYPE_RK3588_DWMSHC);
        DriveInfo *di = drive_get(IF_SD, 0, 0);
        BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;

        sysbus_realize_and_unref(SYS_BUS_DEVICE(sd), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(sd), 0, 0xfe2b0000);
        sysbus_connect_irq(SYS_BUS_DEVICE(sd), 0, qdev_get_gpio_in(gic, 98));
        if (blk) {
            DeviceState *card = qdev_new(TYPE_SD_CARD);

            qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
            qdev_realize_and_unref(card, qdev_get_child_bus(sd, "sd-bus"),
                                   &error_fatal);
        }
    }
    rk3568_boot_from_sd(ms, pmugrf);
}

static void rk3568_machine_init(MachineClass *mc)
{
    mc->desc = "Rockchip RK3568 (experimental direct firmware boot)";
    mc->init = rk3568_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a55");
    mc->min_cpus = 1;
    mc->default_cpus = 4;
    mc->max_cpus = 4;
    /* The missing DDR handoff currently makes U-Boot assume 256 MiB. */
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "rk3568.ram";
}
DEFINE_MACHINE("rk3568", rk3568_machine_init)
