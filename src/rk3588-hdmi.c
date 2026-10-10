/*
 * Rockchip RK3588 HDMI TX (Synopsys DW HDMI QP, "rockchip,rk3588-dw-hdmi-qp").
 *
 * Register behavior extracted from the Linux 7.2.9 drivers
 * (drivers/gpu/drm/bridge/synopsys/dw-hdmi-qp.c + rockchip glue):
 *  - the driver only touches offsets <= 0x482c; there is no ID/version
 *    check at probe, so the register file is plain storage;
 *  - EDID reads run through the internal I2C master (I2CM regs
 *    0xe0-0x118): the guest programs slave/offset/command into
 *    I2CM_INTERFACE_CONTROL0 (0xf4) and each transfer completes via the
 *    "main" IRQ: MAINUNIT_1_INT_STATUS (0x3020) bit0 OP_DONE / bit2
 *    NACK, unmasked through MASK_N (0x3024) and acked W1C at 0x3028.
 *    The model serves a fixed 128-byte 1080p EDID at DDC slave 0x50 and
 *    NACKs everything else, completing instantly at command-issue time;
 *  - HPD does not involve this MMIO window at all: the glue reads the
 *    level from sys_grf 0x384 bit16 (stub read hook) and the hpd IRQ is
 *    never asserted (the "monitor" is plugged before power-on).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/module.h"
#include "rk3588-hdmi.h"

#define HDMI_REG_WINDOW   0x20000
/* Highest offset the driver's regmap allows (EARCRX_1_INT_FORCE). */
#define HDMI_REG_STORAGE  0x4830

/* I2CM_INTERFACE_CONTROL0 (0xf4), dw-hdmi-qp.h */
#define I2CM_SLVADDR_SHIFT 5           /* bits [11:5]: slave address */
#define I2CM_SLVADDR_MASK  0x7f
#define I2CM_ADDR_SHIFT    12          /* bits [19:12]: 8-bit DDC offset */
#define I2CM_ADDR_MASK     0xff
#define I2CM_WR_MASK       0x1e        /* bits [4:1]: command field */
#define I2CM_CMD_WRITE      0x2        /* FM_WRITE  */
#define I2CM_CMD_READ       0x4        /* FM_READ   */
#define I2CM_CMD_SHORT_READ 0x8        /* SHORT_READ */
#define I2CM_CMD_EXT_READ   0x10       /* EXT_READ  */

/* MAINUNIT_1 interrupt block (status/mask/clear) */
#define MAINUNIT_1_STATUS   0x3020
#define MAINUNIT_1_MASK_N   0x3024     /* 1 = not masked */
#define MAINUNIT_1_CLEAR    0x3028     /* write-1-to-clear */
#define MAINUNIT_1_I2CM_IRQ 0x7        /* OP_DONE|READ_REQ|NACK */

/* I2CM data/segment registers */
#define I2CM_CONTROL0       0x00ec     /* 0x01 = software reset */
#define I2CM_WRDATA_0_3     0x00fc
#define I2CM_SEG_CONTROL1   0x00f8     /* bits[14:7] SEG_PTR, [7:0] SEG_ADDR */
#define I2CM_RDDATA_0_3     0x010c

#define DDC_ADDR            0x50

/*
 * Fixed EDID: 128-byte base block, EDID 1.3 digital ("QEMU rk3588",
 * mfg ID 0x44AD), 1920x1080@60 native DTD (148.5 MHz, CEA VIC-16
 * timing, digital separate sync, positive polarities, misc 0x1E),
 * name + range-limit descriptors, checksum 0xca.  Revision 3 and the
 * digital input bit are required or drm_update_display_info() drops
 * RGB444 ("HDMI Sink doesn't support RGB"); byte-24 bit1 makes DTD 1
 * the preferred mode.  Layout verified against drm_mode_detailed()
 * (drm_edid.c) byte for byte; sums to 0 mod 256 (checksum 0xca —
 * this array is authoritative, it was once corrupted by a hand
 * reformat; regenerate from this text if in doubt).
 */
static const uint8_t rk3588_hdmi_edid[128] = {
    0x00,0xff,0xff,0xff,0xff,0xff,0xff,0x00,
    0x44,0xad,0x35,0x88,0x01,0x00,0x58,0x51,
    0x01,0x24,0x01,0x03,0x80,0x30,0x1e,0x78,
    0x06,0x96,0x8f,0x52,0x33,0x66,0x05,0x9a,
    0x3d,0x40,0x51,0x21,0x04,0x00,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x3a,
    0x80,0x18,0x71,0x38,0x2d,0x40,0x58,0x2c,
    0x45,0x00,0xe0,0x0e,0x11,0x00,0x00,0x1e,
    0x00,0x00,0xfc,0x00,0x51,0x45,0x4d,0x55,
    0x20,0x72,0x6b,0x33,0x35,0x38,0x38,0x0a,
    0x20,0x00,0x00,0x00,0xfd,0x00,0x32,0x4b,
    0x1e,0x8c,0x96,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xca,
};

struct RkHdmiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq avp_irq, cec_irq, earc_irq, main_irq, hpd_irq;

    uint32_t regs[HDMI_REG_STORAGE / 4];
    uint32_t i2c_status;               /* MAINUNIT_1_INT_STATUS bits */
    uint32_t i2c_mask_n;               /* MAINUNIT_1_INT_MASK_N bits */
};

static uint8_t rk_hdmi_edid_byte(unsigned int index)
{
    return index < sizeof(rk3588_hdmi_edid) ?
           rk3588_hdmi_edid[index] : 0x00;
}

static void rk_hdmi_update_main_irq(struct RkHdmiState *s)
{
    qemu_set_irq(s->main_irq, !!(s->i2c_status & s->i2c_mask_n &
                                 MAINUNIT_1_I2CM_IRQ));
}

/*
 * A command written into I2CM_INTERFACE_CONTROL0 runs the DDC transfer
 * instantly: read data lands in RDDATA before the completion status is
 * raised, and the guest's interrupt handler completes the transfer once
 * the "main" IRQ handler reads/acks it.
 */
static void rk_hdmi_i2c_xfer(struct RkHdmiState *s, uint32_t ctl)
{
    unsigned int slave = (ctl >> I2CM_SLVADDR_SHIFT) & I2CM_SLVADDR_MASK;
    unsigned int offset = (ctl >> I2CM_ADDR_SHIFT) & I2CM_ADDR_MASK;
    unsigned int cmd = ctl & I2CM_WR_MASK;
    unsigned int segment;

    if (slave != DDC_ADDR) {
        /* real hardware reports the NACK and still completes */
        s->i2c_status |= (1 << 2) | (1 << 0);
        goto done;
    }

    switch (cmd) {
    case I2CM_CMD_WRITE:
        /* Word-offset write: the register address is in I2CM_ADDR. */
        break;
    case I2CM_CMD_READ:
    case I2CM_CMD_SHORT_READ:
        s->regs[I2CM_RDDATA_0_3 / 4] = rk_hdmi_edid_byte(offset);
        break;
    case I2CM_CMD_EXT_READ:
        segment = (s->regs[I2CM_SEG_CONTROL1 / 4] >> 7) & 0x7f;
        s->regs[I2CM_RDDATA_0_3 / 4] =
            rk_hdmi_edid_byte(segment * 256 + offset);
        break;
    default:
        s->i2c_status |= (1 << 2) | (1 << 0);
        goto done;
    }
    s->i2c_status |= 1 << 0;           /* I2CM_OP_DONE */

done:
    rk_hdmi_update_main_irq(s);
}

static uint64_t rk_hdmi_read(void *opaque, hwaddr offset, unsigned size)
{
    struct RkHdmiState *s = opaque;

    if (offset >= HDMI_REG_STORAGE || (offset & 3)) {
        return 0;
    }
    if (offset == MAINUNIT_1_STATUS) {
        return s->i2c_status;
    }
    if (offset == MAINUNIT_1_MASK_N) {
        return s->i2c_mask_n;
    }
    return s->regs[offset / 4];
}

static void rk_hdmi_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    struct RkHdmiState *s = opaque;

    if (offset >= HDMI_REG_STORAGE || (offset & 3)) {
        return;
    }

    switch (offset) {
    case MAINUNIT_1_STATUS:
        break;                          /* read-only */
    case MAINUNIT_1_MASK_N:
        s->i2c_mask_n = value;
        rk_hdmi_update_main_irq(s);
        break;
    case MAINUNIT_1_CLEAR:              /* write-1-to-clear */
        s->i2c_status &= ~(uint32_t)value;
        rk_hdmi_update_main_irq(s);
        break;
    case 0x00f4:                        /* I2CM_INTERFACE_CONTROL0 */
        s->regs[offset / 4] = value;
        if (value & I2CM_WR_MASK) {
            rk_hdmi_i2c_xfer(s, value);
        }
        break;
    case I2CM_CONTROL0:                  /* 0x01 = software reset */
        if (value & 1) {
            s->regs[0x00f4 / 4] &= ~I2CM_WR_MASK;
        }
        break;
    default:
        s->regs[offset / 4] = value;
        break;
    }
}

static const MemoryRegionOps rk_hdmi_ops = {
    .read = rk_hdmi_read,
    .write = rk_hdmi_write,
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

static void rk_hdmi_reset_hold(Object *obj, ResetType type)
{
    struct RkHdmiState *s = RK3588_HDMI(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->i2c_status = 0;
    s->i2c_mask_n = 0;
    rk_hdmi_update_main_irq(s);
}

static void rk_hdmi_realize(DeviceState *dev, Error **errp)
{
    struct RkHdmiState *s = RK3588_HDMI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rk_hdmi_ops, s,
                          "rk3588.hdmi", HDMI_REG_WINDOW);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->avp_irq);
    sysbus_init_irq(sbd, &s->cec_irq);
    sysbus_init_irq(sbd, &s->earc_irq);
    sysbus_init_irq(sbd, &s->main_irq);
    sysbus_init_irq(sbd, &s->hpd_irq);
}

static void rk_hdmi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = rk_hdmi_realize;
    rc->phases.hold = rk_hdmi_reset_hold;
    dc->desc = "Rockchip RK3588 DW HDMI QP TX";
}

static const TypeInfo rk_hdmi_type = {
    .name = TYPE_RK3588_HDMI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(struct RkHdmiState),
    .class_init = rk_hdmi_class_init,
};

static void rk_hdmi_register_types(void)
{
    type_register_static(&rk_hdmi_type);
}
type_init(rk_hdmi_register_types)
