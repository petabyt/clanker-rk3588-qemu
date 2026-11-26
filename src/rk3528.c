/* Experimental RK3528 machine with an optional direct DDR boot shim.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "hw/arm/bsa.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/intc/arm_gic_common.h"
#include "hw/intc/arm_gic.h"
#include "system/address-spaces.h"
#include "system/physmem.h"
#include "system/reset.h"
#include "system/blockdev.h"
#include "system/block-backend-io.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
#include "dw-apb-uart.h"
#include "rk3588-timer.h"
#include "rk3588-dwmshc.h"
#include "hw/sd/sd.h"

#define RK3528_SPIS 256
static uint32_t *rk3528_ddrgrf;

/* Experimental DDR-to-direct-U-Boot handoff. The BootROM ABI is unverified. */
typedef struct RK3528DDRBoot {
    uint8_t *image;
    size_t length;
    uint8_t *spl;
    size_t spl_length;
    bool returned;
} RK3528DDRBoot;

static uint64_t rk3528_ddr_return_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    RK3528DDRBoot *boot = opaque;
    ARMCPU *cpu = ARM_CPU(first_cpu);

    if (!boot->returned) {
        if ((uint32_t)cpu->env.xregs[0]) {
            return 0x14000000; /* DDR failed: do not start U-Boot. */
        }
        boot->returned = true;
        /* Drop an inconsistent DDR_MEM tag, as in the RK3568 port. */
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
                stl_le_p(hdr + 4, 0);
                physical_memory_write(tag + 4, hdr + 4, 4);
                break;
            }
            tag += words * 4;
        }
        if (boot->spl) {
            physical_memory_write(0, boot->spl, boot->spl_length);
            cpu->env.xregs[0] = 0xfe480000;
            cpu->env.xregs[1] = 8;
            cpu->env.xregs[30] = 0;
        } else {
            cpu->env.xregs[0] = 0;
            cpu->env.xregs[30] = 0x800000;
        }
    }
    return 0xd61f03c0; /* br x30 */
}

static const MemoryRegionOps rk3528_ddr_return_ops = {
    .read = rk3528_ddr_return_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void rk3528_ddr_reset(void *opaque)
{
    RK3528DDRBoot *boot = opaque;
    ARMCPU *cpu = ARM_CPU(first_cpu);
    uint8_t bootdata[32] = { 0 };

    physical_memory_write(0xfe480000, bootdata, sizeof(bootdata));
    physical_memory_write(0xfe481000, boot->image, boot->length);
    boot->returned = false;
    cpu->env.xregs[0] = 0xfe480000;
    cpu->env.xregs[1] = 8;
    cpu->env.xregs[30] = 0xffff0000;
    cpu->env.xregs[31] = 0xfe49fff0;
    cpu_set_pc(CPU(cpu), 0xfe481000);
}

static void rk3528_init_ddr_boot(MachineState *ms)
{
    g_autofree char *path = NULL;
    g_autofree gchar *contents = NULL;
    gsize length;
    RK3528DDRBoot *boot;
    MemoryRegion *ret;
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;
    uint8_t header[512];
    int64_t capacity;

    if (!ms->firmware && !blk) {
        return;
    }
    boot = g_new0(RK3528DDRBoot, 1);
    if (ms->firmware) {
        path = qemu_find_file(QEMU_FILE_TYPE_BIOS, ms->firmware);
        if (!path || !g_file_get_contents(path, &contents, &length, NULL) ||
            length < 4 || length > 60 * KiB) {
            error_report("rk3528: -bios must be a raw DDR binary (maximum 60 KiB)");
            exit(1);
        }
        boot->image = (uint8_t *)g_steal_pointer(&contents);
        boot->length = length;
    } else {
        uint64_t off[2], len[2];

        capacity = blk_getlength(blk);
        if (capacity < 0x8200 ||
            blk_pread(blk, 0x8000, sizeof(header), header, 0) < 0 ||
            ldl_le_p(header) != 0x534e4b52 || lduw_le_p(header + 0xa) != 2) {
            error_report("rk3528: SD boot requires a two-stage RKNS v2 image");
            exit(1);
        }
        for (unsigned i = 0; i < 2; i++) {
            unsigned entry = 0x78 + i * 0x58;

            off[i] = 0x8000 + (uint64_t)lduw_le_p(header + entry) * 512;
            len[i] = (uint64_t)lduw_le_p(header + entry + 2) * 512;
            if (!len[i] || off[i] > capacity || len[i] > capacity - off[i]) {
                error_report("rk3528: invalid SD image bounds");
                exit(1);
            }
        }
        if (len[0] > 60 * KiB || len[1] > ms->ram_size) {
            error_report("rk3528: SD boot stages are too large");
            exit(1);
        }
        boot->length = len[0];
        boot->spl_length = len[1];
        boot->image = g_malloc(len[0]);
        boot->spl = g_malloc(len[1]);
        if (blk_pread(blk, off[0], len[0], boot->image, 0) < 0 ||
            blk_pread(blk, off[1], len[1], boot->spl, 0) < 0) {
            error_report("rk3528: could not read SD boot stages");
            exit(1);
        }
    }
    ret = g_new0(MemoryRegion, 1);
    memory_region_init_io(ret, OBJECT(ms), &rk3528_ddr_return_ops, boot,
                          "rk3528.ddr-return", 4);
    memory_region_add_subregion(get_system_memory(), 0xffff0000, ret);
    qemu_register_reset(rk3528_ddr_reset, boot);
}

static uint64_t rk3528_regs_read(void *opaque, hwaddr offset, unsigned size)
{
    return ((uint32_t *)opaque)[offset / 4];
}

static void rk3528_regs_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    uint32_t *regs = opaque;
    uint32_t mask = value >> 16;

    regs[offset / 4] = (regs[offset / 4] & ~mask) | (value & mask);
}

static uint64_t rk3528_cru_read(void *opaque, hwaddr offset, unsigned size)
{
    uint32_t value = rk3528_regs_read(opaque, offset, size);

    if (offset == 0x04 || offset == 0x24 || offset == 0x64 ||
        offset == 0x20084) {
        value |= 1 << 10; /* PLL lock */
    }
    return value;
}

static const MemoryRegionOps rk3528_regs_ops = {
    .read = rk3528_regs_read, .write = rk3528_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps rk3528_cru_ops = {
    .read = rk3528_cru_read, .write = rk3528_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* Empty OTP: SBPI and user reads complete and return zero-filled fuses. */
static uint64_t rk3528_otp_read(void *opaque, hwaddr offset, unsigned size)
{
    return offset == 0x304 ? 6 : 0;
}

static void rk3528_otp_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
}

static const MemoryRegionOps rk3528_otp_ops = {
    .read = rk3528_otp_read, .write = rk3528_otp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t rk3528_pmugrf_read(void *opaque, hwaddr offset, unsigned size)
{
    /* Direct boot supplies the DDR handoff: 256 MiB, one rank. */
    return offset == 0x248 ? 0x6200 : rk3528_regs_read(opaque, offset, size);
}

static const MemoryRegionOps rk3528_pmugrf_ops = {
    .read = rk3528_pmugrf_read, .write = rk3528_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t rk3528_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    return offset == 0x78 ? 0x01000c2b : rk3528_regs_read(opaque, offset, size);
}

static const MemoryRegionOps rk3528_gpio_ops = {
    .read = rk3528_gpio_read, .write = rk3528_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void rk3528_pwm_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    ((uint32_t *)opaque)[offset / 4] = value;
}

static const MemoryRegionOps rk3528_pwm_ops = {
    .read = rk3528_regs_read, .write = rk3528_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* DDR controller acknowledges SWCTL writes immediately. */
static uint64_t rk3528_ddrc_read(void *opaque, hwaddr offset, unsigned size)
{
    uint32_t *regs = opaque;

    if (offset == 0x324) {
        return regs[0x320 / 4] & 1;
    }
    if (offset == 0x04) {
        return regs[0x30 / 4] & 0x20 ? 0x23 : 1;
    }
    if (offset == 0x1bc) {
        return !(rk3528_ddrgrf[0] & 0x20); /* DFI init handshake */
    }
    return regs[offset / 4];
}

static void rk3528_ddrc_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    if (offset == 0x10) {
        value &= ~(1u << 31); /* mode-register transaction completes */
    }
    rk3528_pwm_write(opaque, offset, value, size);
}

static const MemoryRegionOps rk3528_ddrc_ops = {
    .read = rk3528_ddrc_read, .write = rk3528_ddrc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* PHY lane training completes with the configured lanes present. */
static uint64_t rk3528_ddrphy_read(void *opaque, hwaddr offset, unsigned size)
{
    uint32_t *regs = opaque;

    if (offset == 0x1f0) {
        return 0x81; /* read and write training completed */
    }
    if (offset == 0x20c) {
        return (regs[0] & 0xf00) | ((regs[0] >> 8) & 0xf);
    }
    return regs[offset / 4];
}

static const MemoryRegionOps rk3528_ddrphy_ops = {
    .read = rk3528_ddrphy_read, .write = rk3528_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint32_t *rk3528_registers(MachineState *ms, hwaddr base, unsigned size,
                                  const char *name, const MemoryRegionOps *ops)
{
    MemoryRegion *mr = g_new0(MemoryRegion, 1);
    uint32_t *regs = g_new0(uint32_t, size / 4);

    memory_region_init_io(mr, OBJECT(ms), ops, regs, name, size);
    memory_region_add_subregion(get_system_memory(), base, mr);
    return regs;
}

static void rk3528_init(MachineState *ms)
{
    DeviceState *gic, *uart, *timer;
    MemoryRegion *sram = g_new0(MemoryRegion, 1);
    const int timers[] = { ARCH_TIMER_NS_EL1_IRQ, ARCH_TIMER_VIRT_IRQ,
                           ARCH_TIMER_NS_EL2_IRQ, ARCH_TIMER_S_EL1_IRQ };
    const int inputs[] = { ARM_CPU_IRQ, ARM_CPU_FIQ, ARM_CPU_VIRQ, ARM_CPU_VFIQ };

    if (ms->ram_size != 256 * MiB) {
        error_report("rk3528: direct boot requires 256 MiB RAM");
        exit(1);
    }
    memory_region_add_subregion(get_system_memory(), 0, ms->ram);
    /* Capacity probes wrap around the synthetic 256 MiB DRAM. */
    for (hwaddr base = 256 * MiB; base < 0xf0000000ULL; base += 256 * MiB) {
        MemoryRegion *alias = g_new0(MemoryRegion, 1);

        memory_region_init_alias(alias, OBJECT(ms), "rk3528.ram-wrap", ms->ram,
                                 0, 256 * MiB);
        memory_region_add_subregion(get_system_memory(), base, alias);
    }
    /* DDR firmware is linked at 0xfe481000 in the internal SRAM window. */
    memory_region_init_ram(sram, NULL, "rk3528.sram", 128 * KiB,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0xfe480000, sram);
    rk3528_registers(ms, 0xff4a0000, 0x30000, "rk3528.cru", &rk3528_cru_ops);
    rk3528_registers(ms, 0xff540000, 0x40000, "rk3528.ioc", &rk3528_regs_ops);
    rk3528_registers(ms, 0xff370000, 0x1000, "rk3528.pmugrf", &rk3528_pmugrf_ops);
    rk3528_registers(ms, 0xff340000, 0x10000, "rk3528.vpu-grf", &rk3528_regs_ops);
    rk3528_registers(ms, 0xff300000, 0x1000, "rk3528.core-grf", &rk3528_regs_ops);
    rk3528_registers(ms, 0xff380000, 0x10000, "rk3528.bl31-unknown", &rk3528_regs_ops);
    rk3528_registers(ms, 0xffc60000, 0x1000, "rk3528.bl31-unknown2", &rk3528_regs_ops);
    rk3528_registers(ms, 0xff600000, 0x2000, "rk3528.pmu", &rk3528_regs_ops);
    rk3528_registers(ms, 0xff2e0000, 0x1000, "rk3528.ddr-firewall", &rk3528_regs_ops);
    rk3528_registers(ms, 0xff2d0000, 0x1000, "rk3528.sys-firewall", &rk3528_regs_ops);
    rk3528_registers(ms, 0xff320000, 0x1000, "rk3528.ddr-control", &rk3528_regs_ops);
    rk3528_ddrgrf = rk3528_registers(ms, 0xff330000, 0x1000,
                                     "rk3528.ddr-control2", &rk3528_regs_ops);
    rk3528_registers(ms, 0xffe20000, 0x10000, "rk3528.ddr-unknown", &rk3528_regs_ops);
    rk3528_registers(ms, 0xff450000, 0x1000, "rk3528.ddr-unknown2", &rk3528_regs_ops);
    rk3528_registers(ms, 0xff440000, 0x10000, "rk3528.bl31-unknown3", &rk3528_regs_ops);
    rk3528_registers(ms, 0xff230000, 0x1000, "rk3528.ddr-unknown5", &rk3528_pwm_ops);
    rk3528_registers(ms, 0xff920000, 0x10000, "rk3528.ddrphy", &rk3528_ddrphy_ops);
    rk3528_registers(ms, 0xff910000, 0x10000, "rk3528.ddrc", &rk3528_ddrc_ops);
    rk3528_registers(ms, 0xffce0000, 0x4000, "rk3528.otp", &rk3528_otp_ops);
    rk3528_registers(ms, 0xffa90000, 0x40, "rk3528.pwm", &rk3528_pwm_ops);
    rk3528_registers(ms, 0xff610000, 0x200, "rk3528.gpio0", &rk3528_gpio_ops);
    for (unsigned i = 0; i < 4; i++) {
        rk3528_registers(ms, 0xffaf0000 + i * 0x10000, 0x200,
                         "rk3528.gpio", &rk3528_gpio_ops);
    }
    for (unsigned i = 0; i < ms->smp.cpus; i++) {
        Object *cpu = object_new(ms->cpu_type);

        object_property_set_int(cpu, "cntfrq", 24000000, &error_fatal);
        object_property_set_int(cpu, "mp-affinity", i, &error_fatal);
        object_property_set_bool(cpu, "has_el3", true, &error_fatal);
        if (i) {
            object_property_set_bool(cpu, "start-powered-off", true, &error_fatal);
        }
        qdev_realize(DEVICE(cpu), NULL, &error_fatal);
    }
    gic = qdev_new(TYPE_ARM_GIC);
    qdev_prop_set_uint32(gic, "revision", 2);
    qdev_prop_set_uint32(gic, "num-cpu", ms->smp.cpus);
    qdev_prop_set_uint32(gic, "num-irq", RK3528_SPIS + GIC_INTERNAL);
    qdev_prop_set_bit(gic, "has-security-extensions", true);
    qdev_prop_set_bit(gic, "has-virtualization-extensions", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gic), &error_fatal);
    for (unsigned i = 0; i < 4; i++) {
        sysbus_mmio_map(SYS_BUS_DEVICE(gic), i, 0xfed01000 + i * 0x2000 - (i ? 0x1000 : 0));
    }
    for (unsigned i = 0; i < ms->smp.cpus; i++) {
        DeviceState *cpu = DEVICE(qemu_get_cpu(i));
        unsigned ppi = RK3528_SPIS + i * GIC_INTERNAL;

        for (unsigned j = 0; j < ARRAY_SIZE(timers); j++) {
            qdev_connect_gpio_out(cpu, j, qdev_get_gpio_in(gic, ppi + timers[j]));
            sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + j * ms->smp.cpus,
                               qdev_get_gpio_in(cpu, inputs[j]));
        }
    }
    uart = qdev_new(TYPE_DW_APB_UART);
    if (serial_hd(0)) {
        qdev_prop_set_chr(uart, "chardev", serial_hd(0));
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, 0xff9f0000);
    sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0, qdev_get_gpio_in(gic, 40));
    for (unsigned i = 0; i < 6; i++) {
        timer = qdev_new(TYPE_RK3588_TIMER);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(timer), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(timer), 0, 0xff620000 + i * 0x20);
    }
    {
        DeviceState *sd = qdev_new(TYPE_RK3588_DWMSHC);
        DriveInfo *di = drive_get(IF_SD, 0, 0);
        BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;

        sysbus_realize_and_unref(SYS_BUS_DEVICE(sd), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(sd), 0, 0xffc30000);
        sysbus_connect_irq(SYS_BUS_DEVICE(sd), 0, qdev_get_gpio_in(gic, 133));
        if (blk) {
            DeviceState *card = qdev_new(TYPE_SD_CARD);

            qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
            qdev_realize_and_unref(card, qdev_get_child_bus(sd, "sd-bus"),
                                   &error_fatal);
        }
    }
    rk3528_init_ddr_boot(ms);
}

static void rk3528_machine_init(MachineClass *mc)
{
    mc->desc = "Rockchip RK3528 (experimental direct U-Boot boot)";
    mc->init = rk3528_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a53");
    mc->min_cpus = 1;
    mc->default_cpus = 4;
    mc->max_cpus = 4;
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "rk3528.ram";
}
DEFINE_MACHINE("rk3528", rk3528_machine_init)
