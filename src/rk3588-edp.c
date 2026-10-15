/*
 * Rockchip RK3588 eDP controller (Analogix DP, "rockchip,rk3588-edp").
 *
 * rk3588.c creates two instances of this model:
 *   edp0 @0xfdec0000 - the mainline Linux analogix_dp chain (chip data
 *     reg = 0xfdec0000 in analogix_dp-rockchip.c), property
 *     "strm-valid" = true;
 *   edp1 @0xfded0000 - the ./rk bootloader's controller
 *     (rk/src/analogix_edp.c edp_init(EDP1)), "strm-valid" = false.
 *
 * Guest contract (Linux 7.2.9 drivers/gpu/drm/bridge/analogix/):
 *  - Plain 32-bit register file (raw readl/writel, no regmap quirks)
 *    with status read hooks that terminate every polling loop:
 *      SYS_CTL_1 0x600 DET_STA  bit2 = 1  (input stream clock present)
 *      SYS_CTL_2 0x604 CHA_STA  bit2 = 0  (no channel alarm)
 *      SYS_CTL_3 0x608 HPD_STATUS bit6 = 1 (sink plugged; static, so the
 *                    model never raises SPI 163 - the DT interrupt exists
 *                    only because platform_get_irq() is mandatory)
 *      DEBUG_CTL 0x6c0 PLL_LOCK bit4 = 1
 *    plus a polarity split on SYS_CTL_3 STRM_VALID bit2 that the two
 *    guests demand opposite ways: analogix_dp_is_video_stream_on()
 *    waits for the bit SET, while the ./rk firmware's video-on loop
 *    (edp_enable in rk/src/analogix_edp.c) breaks only while the bit is
 *    CLEAR and reports a timeout when it stays set.  The per-instance
 *    "strm-valid" property serves each driver its own polarity; on edp1
 *    all hooked reads are bit-identical to the old rk_stub_edp_read
 *    stub window (the firmware never writes AUX 0x7a4 either, so the
 *    engine below stays dormant there).
 *  - The INT block 0x3c4-0x3d0 + 0x3dc is write-1-to-clear:
 *    analogix_dp_transfer() acks RPLY_RECEIV/AUX_ERR by writing them and
 *    would otherwise see AUX_ERR latched from plain storage forever.
 *  - AUX engine: a write to AUX_CH_CTL_2 0x7a4 with AUX_EN executes the
 *    transaction synchronously - native DPCD reads/writes into a modeled
 *    sink window, I2C-over-AUX to DDC slave 0x50 serving the EDID -
 *    then fills BUF_DATA_0+ for reads, reports ACK/NACK via AUX_RX_COMM
 *    / AUX_CH_STA, raises INT_STA.RPLY_RECEIV (plus AUX_ERR on NACK),
 *    and self-clears AUX_EN, which is exactly what the guest's
 *    readx_poll_timeout() loops wait for.
 *  - DPCD sink (offsets per THIS kernel's include/drm/display/drm_dp.h):
 *    rev 1.4, MAX_LINK_RATE 0x14 (HBR2 - RK3588 hardcodes
 *    max_link_rate 0x14), MAX_LANE_COUNT 0x84 (4 lanes + enhanced
 *    frame), MAX_DOWNSPREAD 0 (no NO_AUX_HANDSHAKE flag => the full
 *    clock-recovery + equalizer AUX handshake runs instead of fast
 *    training), PSR unsupported.  The training block is
 *    0x100 LINK_BW_SET / 0x101 LANE_COUNT_SET / 0x102
 *    TRAINING_PATTERN_SET / 0x103-0x106 TRAINING_LANE0-3_SET (the
 *    guest writes pattern 0x21/0x22/0x00 to 0x102, not 0x100),
 *    status 0x202/0x203 report "done" while a pattern is programmed
 *    at 0x102, adjust requests 0x206/0x207 stay at level 0, and
 *    0x600 SET_POWER is stored - so both training phases pass on
 *    their first status read.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "trace/trace-hw_arm.h"
#include "rk3588-edp.h"

#define EDP_REG_WINDOW   0x1000
#define EDP_DPCD_SIZE    0x1000

/* Register offsets (analogix_dp_reg.h) */
#define EDP_COMMON_INT_STA_1  0x3c4   /* ..0x3d0, W1C */
#define EDP_INT_STA           0x3dc   /* W1C */
#define EDP_SYS_CTL_1         0x600
#define EDP_SYS_CTL_2         0x604
#define EDP_SYS_CTL_3         0x608
#define EDP_DEBUG_CTL         0x6c0
#define EDP_AUX_CH_STA        0x780
#define EDP_AUX_RX_COMM       0x78c
#define EDP_BUFFER_DATA_CTL   0x790
#define EDP_AUX_CH_CTL_1      0x794
#define EDP_AUX_ADDR_7_0      0x798
#define EDP_AUX_ADDR_15_8     0x79c
#define EDP_AUX_ADDR_19_16    0x7a0
#define EDP_AUX_CH_CTL_2      0x7a4
#define EDP_BUF_DATA_0        0x7c0

/* Bits */
#define DET_STA               (1u << 2)
#define CHA_STA               (1u << 2)
#define STRM_VALID            (1u << 2)
#define HPD_STATUS            (1u << 6)
#define PLL_LOCK              (1u << 4)
#define RPLY_RECEIV           (1u << 1)
#define AUX_ERR               (1u << 0)
#define AUX_TX_COMM_READ      (1u << 0)
#define AUX_TX_COMM_DP        (1u << 3)   /* AUX_CH_CTL_1: native, else I2C */
#define ADDR_ONLY             (1u << 1)
#define AUX_EN                (1u << 0)
#define BUF_CLR               (1u << 7)
#define AUX_STATUS_NACK       0x1         /* AUX_CH_STA[3:0] */

#define DDC_ADDR              0x50

/*
 * Fixed EDID, byte-identical to the proven 1080p block in rk3588-hdmi.c
 * (EDID 1.3 digital, mfg 0x44AD, 1920x1080@60 native/preferred DTD, name
 * "QEMU rk3588", checksum 0xca - sums to 0 mod 256).  Regenerate from
 * that array if in doubt; hand reformatting once corrupted it (plan.md
 * P6 build fixes).
 */
static const uint8_t rk3588_edp_edid[128] = {
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

struct RkEdpState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    bool strm_valid;            /* STRM_VALID polarity, per guest */

    uint32_t regs[EDP_REG_WINDOW / 4];
    uint8_t dpcd[EDP_DPCD_SIZE];
    uint8_t aux_status;         /* AUX_CH_STA[3:0] of the last AUX txn */
    unsigned i2c_ptr;           /* EDID read pointer (I2C-over-AUX) */
};

static void rk_edp_dpcd_reset(struct RkEdpState *s)
{
    memset(s->dpcd, 0, sizeof(s->dpcd));
    s->dpcd[0x000] = 0x14;      /* DPCD_REV 1.4 */
    s->dpcd[0x001] = 0x14;      /* MAX_LINK_RATE: HBR2 (5.4 Gbps) */
    s->dpcd[0x002] = 0x84;      /* MAX_LANE_COUNT: 4 lanes + enhanced */
    s->dpcd[0x003] = 0x00;      /* MAX_DOWNSPREAD: no fast training */
    s->dpcd[0x090] = 0x00;      /* PSR_SUPPORT: none */
    s->dpcd[0x600] = 0x01;      /* SET_POWER: D0 */
}

static uint8_t rk_edp_dpcd_read(struct RkEdpState *s, uint32_t addr)
{
    if (addr >= 0x202 && addr <= 0x207) {
        switch (addr) {
        case 0x206:             /* ADJUST_REQUEST_LANE0_1 (drm_dp.h) */
        case 0x207:             /* ADJUST_REQUEST_LANE2_3 */
            return 0x00;        /* requested swing/pre-emphasis: level 0 */
        default:
            /*
             * Lane status bytes (this kernel's drm_dp.h):
             * 0x202 LANE0_1_STATUS, 0x203 LANE2_3_STATUS (+0x204/0x205).
             * Report CR/EQ/symbol-lock DONE while the sink trains a
             * pattern (TRAINING_PATTERN_SET @0x102), idle otherwise.
             * Putting the adjust bytes at 0x203/0x205 (spec layout)
             * instead of 0x206/0x207 made the guest extract adjust
             * level 3 out of a 0xff status byte -> "CR Max reached
             * (0,3,3)" and link training -EREMOTEIO.
             */
            return (s->dpcd[0x102] & 0x7) ? 0xff : 0x00;
        }
    }
    return s->dpcd[addr];
}

/*
 * Execute one AUX transaction synchronously (instant sink) and publish
 * the completion state the guest polls for.
 */
static void rk_edp_aux_exec(struct RkEdpState *s)
{
    uint32_t ctl1 = s->regs[EDP_AUX_CH_CTL_1 / 4];
    uint32_t addr = (s->regs[EDP_AUX_ADDR_7_0 / 4] & 0xff) |
                    ((s->regs[EDP_AUX_ADDR_15_8 / 4] & 0xff) << 8) |
                    ((s->regs[EDP_AUX_ADDR_19_16 / 4] & 0x0f) << 16);
    bool addr_only = s->regs[EDP_AUX_CH_CTL_2 / 4] & ADDR_ONLY;
    bool native = ctl1 & AUX_TX_COMM_DP;
    bool rd = ctl1 & AUX_TX_COMM_READ;
    unsigned len = addr_only ? 0 : (((ctl1 >> 4) & 0xf) + 1);
    bool nack = false;
    unsigned i;

    if (native) {
        for (i = 0; i < len; i++) {
            uint32_t a = addr + i;

            if (rd) {
                uint8_t b = (a < sizeof(s->dpcd)) ? rk_edp_dpcd_read(s, a)
                                                  : 0x00;

                s->regs[(EDP_BUF_DATA_0 + 4 * i) / 4] = b;
            } else if (a < sizeof(s->dpcd)) {
                uint8_t b = s->regs[(EDP_BUF_DATA_0 + 4 * i) / 4] & 0xff;

                s->dpcd[a] = b;
                trace_rk3588_edp_dpcd_write(a, b);
            }
        }
    } else {
        /* I2C-over-AUX: only the DDC slave with the EDID answers. */
        uint8_t slave = addr & 0xff;

        if (slave != DDC_ADDR && slave != (DDC_ADDR << 1) &&
            ((addr >> 1) & 0x7f) != DDC_ADDR) {
            nack = true;
        } else if (rd) {
            for (i = 0; i < len; i++) {
                s->regs[(EDP_BUF_DATA_0 + 4 * i) / 4] =
                    (s->i2c_ptr + i < sizeof(rk3588_edp_edid)) ?
                        rk3588_edp_edid[s->i2c_ptr + i] : 0x00;
            }
            s->i2c_ptr += len;
        } else if (len) {
            s->i2c_ptr = s->regs[EDP_BUF_DATA_0 / 4] & 0xff;  /* offset */
        }
    }

    s->aux_status = nack ? AUX_STATUS_NACK : 0;
    s->regs[EDP_AUX_CH_STA / 4] =
        (s->regs[EDP_AUX_CH_STA / 4] & ~0x1fu) | s->aux_status;
    s->regs[EDP_AUX_RX_COMM / 4] = 0;              /* ACK, no defer */
    s->regs[EDP_INT_STA / 4] |= nack ? (AUX_ERR | RPLY_RECEIV)
                                     : RPLY_RECEIV;
    s->regs[EDP_AUX_CH_CTL_2 / 4] &= ~AUX_EN;      /* self-clear */
    trace_rk3588_edp_aux(native ? "dpcd" : "i2c",
                         rd ? "read" : "write", addr, len, s->aux_status);
}

static uint64_t rk_edp_read(void *opaque, hwaddr offset, unsigned size)
{
    struct RkEdpState *s = opaque;
    hwaddr aoff = offset & ~(hwaddr)3;
    uint32_t v;
    uint32_t mask = size >= 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);

    if (aoff >= EDP_REG_WINDOW) {
        return 0;
    }
    v = s->regs[aoff / 4];
    switch (aoff) {
    case EDP_SYS_CTL_1:
        v |= DET_STA;
        break;
    case EDP_SYS_CTL_2:
        v &= ~CHA_STA;
        break;
    case EDP_SYS_CTL_3:
        v |= HPD_STATUS;
        if (s->strm_valid) {
            v |= STRM_VALID;
        } else {
            v &= ~STRM_VALID;
        }
        break;
    case EDP_DEBUG_CTL:
        v |= PLL_LOCK;
        break;
    case EDP_AUX_CH_STA:
        v = (v & ~0x1fu) | s->aux_status;     /* BUSY never set */
        break;
    }
    return (v >> ((offset & 3) * 8)) & mask;
}

static void rk_edp_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    struct RkEdpState *s = opaque;
    hwaddr aoff = offset & ~(hwaddr)3;
    uint32_t *reg;
    unsigned shift;
    uint32_t mask;

    if (aoff >= EDP_REG_WINDOW) {
        return;
    }
    reg = &s->regs[aoff / 4];

    /*
     * Behavioral side effects run on word-aligned 32-bit stores (what
     * both guests issue); sub-word stores merge into the backing word
     * with plain-RW semantics, like the stub window this replaces.
     */
    if ((offset & 3) == 0 && size == 4) {
        switch (aoff) {
        case 0x3c4:             /* COMMON_INT_STA_1..4, INT_STA: W1C */
        case 0x3c8:
        case 0x3cc:
        case 0x3d0:
        case EDP_INT_STA:
            *reg &= ~(uint32_t)value;
            return;
        case 0x18:              /* VIDEO_CTL_1: firmware sets VIDEO_ENABLE
                                  * (bit7) at the end of edp_enable — the
                                  * switch point where SYS_CTL_3 STRM_VALID
                                  * flips from "firmware polarity" (clear,
                                  * so its video-on loop breaks) to "Linux
                                  * polarity" (set, so analogix_dp's
                                  * is_video_stream_on passes).  The kernel
                                  * never writes VIDEO_CTL_1. */
            *reg = value;
            if (value & (1u << 7)) {
                s->strm_valid = true;
            }
            return;
        case EDP_BUFFER_DATA_CTL:
            *reg = value;
            if (value & BUF_CLR) {
                memset(&s->regs[EDP_BUF_DATA_0 / 4], 0, 16 * 4);
            }
            return;
        case EDP_AUX_CH_CTL_2:
            *reg = value;
            if (value & AUX_EN) {
                rk_edp_aux_exec(s);
            }
            return;
        default:
            break;
        }
        *reg = value;
        return;
    }

    shift = (offset & 3) * 8;
    mask = ((((uint32_t)1 << (size * 8)) - 1) << shift);
    *reg = (*reg & ~mask) | (((uint32_t)value << shift) & mask);
}

static const MemoryRegionOps rk_edp_ops = {
    .read = rk_edp_read,
    .write = rk_edp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void rk_edp_reset_hold(Object *obj, ResetType type)
{
    struct RkEdpState *s = RK3588_EDP(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->aux_status = 0;
    s->i2c_ptr = 0;
    rk_edp_dpcd_reset(s);
}

static void rk_edp_realize(DeviceState *dev, Error **errp)
{
    struct RkEdpState *s = RK3588_EDP(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rk_edp_ops, s,
                          "rk3588.edp", EDP_REG_WINDOW);
    sysbus_init_mmio(sbd, &s->iomem);
    /* no IRQs: HPD is static, every guest polls (SPI 163 never fires) */
}

static const Property rk_edp_props[] = {
    /*
     * STRM_VALID polarity: true for the Linux analogix_dp instance
     * (waits for the bit SET), false for the ./rk firmware instance
     * (its video-on loop exits only while the bit is CLEAR).
     */
    DEFINE_PROP_BOOL("strm-valid", struct RkEdpState, strm_valid, false),
};

static void rk_edp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = rk_edp_realize;
    device_class_set_props(dc, rk_edp_props);
    rc->phases.hold = rk_edp_reset_hold;
    dc->desc = "Rockchip RK3588 eDP controller (Analogix DP)";
}

static const TypeInfo rk_edp_type = {
    .name = TYPE_RK3588_EDP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(struct RkEdpState),
    .class_init = rk_edp_class_init,
};

static void rk_edp_register_types(void)
{
    type_register_static(&rk_edp_type);
}
type_init(rk_edp_register_types)