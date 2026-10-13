/*
 * Rockchip IOMMU ("rockchip,rk3568-iommu", rk3588 compat list) - behavioral
 * model for the Linux rk_iommu driver (drivers/iommu/rockchip-iommu.c).
 *
 * The vop_mmu instance (iommu@fdd97e00, num_mmu=2 windows at +0x00/+0x100)
 * is the runtime-PM supplier of the VOP2 DRM device: a failing runtime
 * resume of the iommu propagates -EINVAL through rpm_get_suppliers() into
 * vop2_enable()'s pm_runtime_resume_and_get(), after which vop2_isr()
 * refuses to run (pm_runtime_get_if_in_use() == 0) and every atomic commit
 * dies with "flip_done timed out".  That failure chain was root-caused in
 * plan.md P14: the driver polls MMU_STATUS command-acknowledge bits that a
 * plain storage stub never sets (enable_paging -> PAGING_ENABLED, 1 s).
 *
 * Modeled registers (per window):
 *  - DTE_ADDR (0x00): plain storage; FORCE_RESET self-clears it (the
 *    driver's is_reset_done poll wants it back at 0).  The DTE_ADDR_DUMMY
 *    read-back test in rk_iommu_force_reset() works with plain storage.
 *  - STATUS (0x04): computed from the paging/stall state; IDLE and
 *    REPLAY_BUFFER_EMPTY always set (the MMU never queues transactions),
 *    STALL_NOT_ACTIVE mirrors !stall.
 *  - COMMAND (0x08): write-only, executes immediately:
 *      0 ENABLE_PAGING / 1 DISABLE_PAGING  -> paging flag
 *      2 ENABLE_STALL / 3 DISABLE_STALL   -> stall flag
 *      4 ZAP_CACHE / 5 PAGE_FAULT_DONE     -> no-op
 *      6 FORCE_RESET                      -> dte=0, paging=stall=0
 *
 * Translation is NOT modeled: the driver only touches guest RAM page
 * tables, so attach/map/unmap all succeed without hardware involvement.
 * Device DMA behind this IOMMU must be identity (iommu.passthrough=1) or
 * accept garbage; the VOP2 scanout in rk3588-display.c reads framebuffers
 * by physical address.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "rk3588-iommu.h"

#define RK_MMU_DTE_ADDR    0x00
#define RK_MMU_STATUS      0x04
#define RK_MMU_COMMAND     0x08

#define RK_MMU_STATUS_PAGING_ENABLED   (1 << 0)
#define RK_MMU_STATUS_STALL_ACTIVE    (1 << 2)
#define RK_MMU_STATUS_IDLE            (1 << 3)
#define RK_MMU_STATUS_REPLAY_EMPTY    (1 << 4)
#define RK_MMU_STATUS_STALL_NOT_ACT   (1u << 31)

struct RkIommuState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    bool paging;
    bool stall;
    uint32_t dte;
};

static uint64_t rk_iommu_read(void *opaque, hwaddr offset, unsigned size)
{
    RkIommuState *s = opaque;

    switch (offset) {
    case RK_MMU_DTE_ADDR:
        return s->dte;
    case RK_MMU_STATUS:
        return (s->paging ? RK_MMU_STATUS_PAGING_ENABLED : 0) |
               (s->stall ? RK_MMU_STATUS_STALL_ACTIVE : 0) |
               RK_MMU_STATUS_IDLE |
               RK_MMU_STATUS_REPLAY_EMPTY |
               (s->stall ? 0 : RK_MMU_STATUS_STALL_NOT_ACT);
    default:
        return 0;
    }
}

static void rk_iommu_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    RkIommuState *s = opaque;

    switch (offset) {
    case RK_MMU_DTE_ADDR:
        s->dte = value;
        break;
    case RK_MMU_COMMAND:
        switch (value) {
        case 0:                         /* ENABLE_PAGING */
            s->paging = true;
            break;
        case 1:                         /* DISABLE_PAGING */
            s->paging = false;
            break;
        case 2:                         /* ENABLE_STALL */
            s->stall = true;
            break;
        case 3:                         /* DISABLE_STALL */
            s->stall = false;
            break;
        case 4:                         /* ZAP_CACHE */
        case 5:                         /* PAGE_FAULT_DONE */
            break;
        case 6:                         /* FORCE_RESET */
            s->dte = 0;
            s->paging = false;
            s->stall = false;
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps rk_iommu_ops = {
    .read = rk_iommu_read,
    .write = rk_iommu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rk_iommu_reset_hold(Object *obj, ResetType type)
{
    RkIommuState *s = RK3588_IOMMU(obj);

    s->paging = false;
    s->stall = false;
    s->dte = 0;
}

static void rk_iommu_realize(DeviceState *dev, Error **errp)
{
    RkIommuState *s = RK3588_IOMMU(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rk_iommu_ops, s,
                          "rk3588.iommu", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void rk_iommu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = rk_iommu_realize;
    rc->phases.hold = rk_iommu_reset_hold;
    dc->desc = "Rockchip IOMMU window (behavioral)";
}

static const TypeInfo rk_iommu_type = {
    .name = TYPE_RK3588_IOMMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RkIommuState),
    .class_init = rk_iommu_class_init,
};

static void rk_iommu_register_types(void)
{
    type_register_static(&rk_iommu_type);
}
type_init(rk_iommu_register_types)

void rk3588_create_iommu_window(uint64_t base)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_IOMMU);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, base);
}
