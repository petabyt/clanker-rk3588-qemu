/*
 * Rockchip RK3588 VOP2 display controller ("rockchip,rk3588-vop").
 *
 * Satisfies the Linux 7.2.9 rockchip_drm_vop2 driver beyond the plain
 * register file it replaced (plan.md P2 stub):
 *  - VERSION_INFO (0x004) must read VOP_VERSION_RK3588 0x40176786
 *    (checked in vop2_enable(), first modeset);
 *  - REG_CFG_DONE (0x000) is a write-mask register: bits[19:16] gate
 *    writes to the per-VP done bits [3:0], bit15 is the global enable;
 *    hardware self-clears the per-VP bits at frame start (the ISR only
 *    delivers a page-flip event when the bit has dropped);
 *  - per-VP interrupt block at 0xA0 + vp*0x10: INT_EN/INT_CLR are
 *    write-mask registers over the low 16 status bits; the ISR clears
 *    pending bits by writing them back with the mask.  The driver needs
 *    the FS_FIELD (frame-start, bit5) interrupt at frame cadence: every
 *    blocking atomic commit waits for a vblank ("wait_for_vblanks"),
 *    otherwise each fbcon update stalls for one second.
 *
 * The frame timer also drives UI refresh: text the guest renders into
 * its framebuffer shows up on every tick without an explicit flush.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "rk3588-vop2.h"
#include "rk3588-display.h"

#define VOP2_REG_WINDOW   0x4200
#define VOP2_NR_VPS       4

/* VERSION_INFO */
#define VOP2_VERSION_RK3588   0x40176786

/* REG_CFG_DONE (0x000) */
#define CFG_DONE_GLB_EN       (1 << 15)
#define CFG_DONE_VP_MASK      0xf        /* [3:0], gated by [19:16] */

/* VP register bases (RK3568_VPn_CTRL_BASE) */
#define VOP2_VP_CTRL(vp)      (0x0c00 + (vp) * 0x100)
#define VOP2_VP_INT_EN(vp)     (0x00a0 + (vp) * 0x10)
#define VOP2_VP_INT_CLR(vp)   (0x00a4 + (vp) * 0x10)
#define VOP2_VP_INT_STATUS(vp) (0x00a8 + (vp) * 0x10)

#define VP_DSP_CTRL_STANDBY   (1 << 31)
#define VP_INT_FS_FIELD       (1 << 5)

#define VOP2_FRAME_NS         16666667   /* ~60 Hz */

struct RkVop2State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *frame_timer;

    uint32_t regs[VOP2_REG_WINDOW / 4];
    uint32_t int_en[VOP2_NR_VPS];
    uint32_t int_status[VOP2_NR_VPS];
};

static void rk_vop2_update_irq(struct RkVop2State *s)
{
    int vp, pending = 0;

    for (vp = 0; vp < VOP2_NR_VPS; vp++) {
        pending |= s->int_status[vp] & s->int_en[vp];
    }
    qemu_set_irq(s->irq, !!pending);
}

static void rk_vop2_frame_tick(void *opaque)
{
    struct RkVop2State *s = opaque;
    int vp;

    /*
     * Frame start: per-VP REG_CFG_DONE bits self-clear (page-flip
     * latching), and a frame-start interrupt is raised for every VP
     * whose FS_FIELD enable is set and whose CRTC left standby.
     */
    s->regs[0] &= ~CFG_DONE_VP_MASK;
    for (vp = 0; vp < VOP2_NR_VPS; vp++) {
        if (s->int_en[vp] & VP_INT_FS_FIELD &&
            !(s->regs[VOP2_VP_CTRL(vp) / 4] & VP_DSP_CTRL_STANDBY)) {
            s->int_status[vp] |= VP_INT_FS_FIELD;
        }
    }
    rk_vop2_update_irq(s);

    /* push the (possibly redrawn) guest framebuffer to the console */
    rk3588_display_repaint();

    timer_mod(s->frame_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VOP2_FRAME_NS);
}

static uint64_t rk_vop2_read(void *opaque, hwaddr offset, unsigned size)
{
    struct RkVop2State *s = opaque;

    if (offset >= VOP2_REG_WINDOW || (offset & 3)) {
        return 0;
    }
    if (offset == 0x004) {
        return VOP2_VERSION_RK3588;
    }
    if (offset >= 0x00a0 && offset < 0x00e0) {
        int vp = (offset - 0x00a0) / 0x10;

        switch ((offset - 0x00a0) % 0x10) {
        case 0x0:                        /* INT_EN */
            return s->int_en[vp];
        case 0x8:                        /* INT_STATUS */
            return s->int_status[vp];
        default:
            return 0;                    /* INT_CLR / RAW: write-only */
        }
    }
    return s->regs[offset / 4];
}

static void rk_vop2_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    struct RkVop2State *s = opaque;

    if (offset >= VOP2_REG_WINDOW || (offset & 3)) {
        return;
    }

    if (offset == 0x000) {
        /* write-mask over the per-VP done bits, bit15 global */
        uint32_t mask = (value >> 16) & CFG_DONE_VP_MASK;

        s->regs[0] = (s->regs[0] & ~mask) | (value & mask);
        if (value & CFG_DONE_GLB_EN) {
            s->regs[0] |= CFG_DONE_GLB_EN;
        }
        return;
    }
    if (offset >= 0x00a0 && offset < 0x00e0) {
        int vp = (offset - 0x00a0) / 0x10;
        uint32_t mask = value >> 16;

        switch ((offset - 0x00a0) % 0x10) {
        case 0x0:                        /* INT_EN: write-mask */
            s->int_en[vp] = (s->int_en[vp] & ~mask) | (value & mask);
            rk_vop2_update_irq(s);
            return;
        case 0x4:                        /* INT_CLR: masked W1C */
            s->int_status[vp] &= ~(value & mask);
            rk_vop2_update_irq(s);
            return;
        default:
            return;
        }
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps rk_vop2_ops = {
    .read = rk_vop2_read,
    .write = rk_vop2_write,
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

static void rk_vop2_reset_hold(Object *obj, ResetType type)
{
    struct RkVop2State *s = RK3588_VOP2(obj);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->int_en, 0, sizeof(s->int_en));
    memset(s->int_status, 0, sizeof(s->int_status));
    rk_vop2_update_irq(s);
}

static void rk_vop2_realize(DeviceState *dev, Error **errp)
{
    struct RkVop2State *s = RK3588_VOP2(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rk_vop2_ops, s,
                          "rk3588.vop2", VOP2_REG_WINDOW);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rk_vop2_frame_tick,
                                  s);
    timer_mod(s->frame_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VOP2_FRAME_NS);
}

static void rk_vop2_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = rk_vop2_realize;
    rc->phases.hold = rk_vop2_reset_hold;
    dc->desc = "Rockchip RK3588 VOP2 display controller";
}

static const TypeInfo rk_vop2_type = {
    .name = TYPE_RK3588_VOP2,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(struct RkVop2State),
    .class_init = rk_vop2_class_init,
};

static void rk_vop2_register_types(void)
{
    type_register_static(&rk_vop2_type);
}
type_init(rk_vop2_register_types)
