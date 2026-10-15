/*
 * Rockchip RK3588 I2C controller (rk3x, "rockchip,rk3399-i2c" compat).
 *
 * A behavioural model of just enough of the rk3x register file + IRQ
 * handshake to satisfy the Linux i2c-rk3x driver (which is interrupt
 * driven: it writes IEN + CON(START), then wait_event_timeout()s for the
 * controller to raise the SPI and set the matching IPD bit) and U-Boot's
 * polled rk_i2c (which spins on IPD).  Passive storage stubs cannot work:
 * the driver's transfer never completes without an IPD done-bit + SPI.
 *
 * Register map (i2c-rk3x.c):
 *   CON    0x00   EN|MOD|START|STOP|LASTACK|ACTACK
 *   MRXADDR 0x08 target address (reads/combined)
 *   MRXRADDR 0x0c target register address
 *   MTXCNT 0x10   bytes to transmit  (write triggers TX)
 *   MRXCNT 0x14   bytes to receive   (write triggers RX)
 *   IEN    0x18   interrupt enable
 *   IPD    0x1c   interrupt pending (write-1-to-clear)
 *   TXBUF  0x100  8 words; byte0 of word0 = (addr<<1) for TX
 *   RXBUF  0x200  8 words
 *
 * Handshake modelled by reacting to the register writes the driver makes
 * (the driver is the bus master and drives every step):
 *   - CON write with START -> assert IPD.START
 *   - MTXCNT write         -> run the addressed write, assert MBTF (or
 *                             NAKRCV if the slave is absent)
 *   - MRXCNT write         -> run the addressed read into RXBUF, assert
 *                             MBRF (or NAKRCV if the slave is absent)
 *   - CON write with STOP  -> assert IPD.STOP
 * The IRQ line is level: asserted while (IPD & IEN) != 0.
 *
 * A single slave is modelled: the HYM8563 RTC at 0x51, and only when the
 * instance has the "has-rtc" property (bus 6 in the OPi5 DTB).  Every
 * other address on any bus NACKs, which the driver turns into a fast
 * -ENXIO instead of the -ETIMEDOUT (-110) storms the storage stubs caused.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "rk3588-i2c.h"
#include "rk3588-i2chid.h"

#define RK_I2C_REG_WINDOW  0x1000

/* Register offsets */
#define REG_CON      0x00
#define REG_CLKDIV   0x04
#define REG_MRXADDR  0x08
#define REG_MRXRADDR 0x0c
#define REG_MTXCNT   0x10
#define REG_MRXCNT   0x14
#define REG_IEN      0x18
#define REG_IPD      0x1c
#define REG_FCNT     0x20
#define TXBUFFER_BASE 0x100
#define RXBUFFER_BASE 0x200

/* REG_CON bits */
#define REG_CON_EN      (1u << 0)
#define REG_CON_MOD_MASK (3u << 1)
#define REG_CON_START   (1u << 3)
#define REG_CON_STOP    (1u << 4)

/* REG_IPD / REG_IEN bits (also used for the IRQ line) */
#define REG_INT_BTF    (1u << 0)
#define REG_INT_BRF    (1u << 1)
#define REG_INT_MBTF   (1u << 2)
#define REG_INT_MBRF   (1u << 3)
#define REG_INT_START  (1u << 4)
#define REG_INT_STOP   (1u << 5)
#define REG_INT_NAKRCV (1u << 6)

#define HYM8563_ADDR   0x51

struct RkI2cState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t con;
    uint32_t clkdiv;
    uint32_t mrxaddr;
    uint32_t mrxraddr;
    uint32_t ien;
    uint32_t ipd;
    uint8_t  txbuf[32];
    uint8_t  rxbuf[32];

    bool has_rtc;                 /* serve the HYM8563 at 0x51 */
    bool has_touchpad;            /* serve hid-over-i2c @0x2c (i2c5) */
};

static void rk_i2c_update_irq(struct RkI2cState *s)
{
    qemu_set_irq(s->irq, (s->ipd & s->ien) != 0);
}

static void rk_i2c_raise(struct RkI2cState *s, uint32_t bit)
{
    s->ipd |= bit;
    rk_i2c_update_irq(s);
}

/* --- HYM8563 RTC slave (register file + live BCD wall-clock) --------- */

static uint8_t bin2bcd(uint8_t x)
{
    return ((x / 10) << 4) | (x % 10);
}

/*
 * The RTC serves the host's real wall-clock time in the BCD calendar
 * registers (0x02..0x08) so the guest's hctosys/fake-hwclock get a sane
 * time; the VL bit (SEC.7) is kept 0 (valid).  Control regs (0x00/0x01)
 * are plain storage.
 */
static void rk_hym8563_read_time(uint8_t *r)
{
    /*
     * NB: QEMU_CLOCK_REALTIME is NOT wall time in this tree —
     * qemu_clock_get_ns(QEMU_CLOCK_REALTIME) returns get_clock() which is
     * CLOCK_MONOTONIC (host uptime).  Using it made hctosys set the guest
     * epoch to uptime (observed: "2070-01-04" = 1970-01-04 + ~3.1d
     * uptime).  Host wall time lives in get_clock_realtime() (what
     * QEMU_CLOCK_HOST maps to).
     */
    time_t t = (time_t)(get_clock_realtime() / 1000000000LL);
    struct tm tm;

    gmtime_r(&t, &tm);
    r[0x02] = bin2bcd(tm.tm_sec);                 /* SEC,  VL=0 (valid) */
    r[0x03] = bin2bcd(tm.tm_min);                 /* MIN */
    r[0x04] = bin2bcd(tm.tm_hour);                /* HOUR */
    r[0x05] = bin2bcd(tm.tm_mday);                /* DAY */
    r[0x06] = tm.tm_wday & 7;                     /* WEEKDAY (0=Sun) */
    r[0x07] = bin2bcd(tm.tm_mon + 1);             /* MONTH (century=0) */
    r[0x08] = bin2bcd((tm.tm_year + 1900) % 100); /* YEAR */
}

/*
 * Addressed write: first TX byte is (addr<<1), then register offset, then
 * data bytes.  Returns true if the slave ACKed (address present), false
 * on NACK (absent address -> driver reports -ENXIO).
 */
static bool rk_i2c_slave_write(struct RkI2cState *s, unsigned int reg,
                               const uint8_t *data, unsigned int len)
{
    /* Only the RTC exists; every other address NACKs. */
    if (!s->has_rtc || ((s->txbuf[0] >> 1) & 0x7f) != HYM8563_ADDR) {
        return false;
    }
    /* RTC control regs are stored; calendar writes are ignored (the model
       keeps serving live host time, which is what the guest wants). */
    (void)reg;
    (void)data;
    (void)len;
    return true;
}

/*
 * Addressed read: fill `buf` with `len` bytes from slave register `reg`.
 * Returns true on ACK, false on NACK.
 */
static bool rk_i2c_slave_read(struct RkI2cState *s, unsigned int addr,
                              unsigned int reg, uint8_t *buf,
                              unsigned int len)
{
    uint8_t rtc[16];

    if (!s->has_rtc || addr != HYM8563_ADDR) {
        return false;
    }
    memset(rtc, 0, sizeof(rtc));
    rk_hym8563_read_time(rtc);
    for (unsigned int i = 0; i < len; i++) {
        unsigned int a = (reg + i) & 0x0f;
        buf[i] = rtc[a];     /* out-of-range regs read 0 */
    }
    return true;
}

/* --- register-file reactions ----------------------------------------- */

/* MTXCNT write: the driver just handed us a transmit burst. */
static void rk_i2c_do_tx(struct RkI2cState *s, uint32_t cnt)
{
    unsigned int addr = (s->txbuf[0] >> 1) & 0x7f;
    unsigned int reg, dlen;

    if (cnt == 0) {
        rk_i2c_raise(s, REG_INT_MBTF);
        return;
    }
    /* byte0 = address; byte1 = register offset; rest = data. */
    reg = (cnt > 1) ? s->txbuf[1] : 0;
    dlen = (cnt > 2) ? cnt - 2 : 0;
    if (s->has_rtc && addr == HYM8563_ADDR) {
        if (!rk_i2c_slave_write(s, reg, &s->txbuf[2], dlen)) {
            rk_i2c_raise(s, REG_INT_NAKRCV);
            return;
        }
        rk_i2c_raise(s, REG_INT_MBTF);
    } else if (s->has_touchpad && addr == RK_I2CHID_ADDR) {
        /* hid-over-i2c registers are 2-byte LE: reg16 = txbuf[1]|txbuf[2]<<8,
           payload follows (0 bytes for a pure register-select write). */
        unsigned int reg16 = (cnt > 2) ? (reg | ((unsigned)s->txbuf[2] << 8))
                                       : reg;
        unsigned int plen = (cnt > 3) ? cnt - 3 : 0;
        if (!rk_i2chid_write(reg16, &s->txbuf[3], plen)) {
            rk_i2c_raise(s, REG_INT_NAKRCV);
            return;
        }
        rk_i2c_raise(s, REG_INT_MBTF);
    } else {
        rk_i2c_raise(s, REG_INT_NAKRCV);
    }
}

/* MRXCNT write: the driver just asked us to receive `cnt` bytes. */
static void rk_i2c_do_rx(struct RkI2cState *s, uint32_t cnt)
{
    unsigned int addr = (s->mrxaddr >> 1) & 0x7f;
    unsigned int reg = s->mrxraddr & 0xff;

    if (cnt > 32) {
        cnt = 32;
    }
    memset(s->rxbuf, 0, sizeof(s->rxbuf));
    if (s->has_rtc && addr == HYM8563_ADDR) {
        if (!rk_i2c_slave_read(s, addr, reg, s->rxbuf, cnt)) {
            rk_i2c_raise(s, REG_INT_NAKRCV);
            return;
        }
        rk_i2c_raise(s, REG_INT_MBRF);
    } else if (s->has_touchpad && addr == RK_I2CHID_ADDR) {
        /* `reg` is mrxraddr & 0xff = LOW byte of the 2-byte LE register;
           all hid-over-i2c registers used here are < 0x100.  A plain
           read (probe byte / i2c_master_recv input frame) lands here
           with a stale/zero reg — the slave serves the input frame
           first regardless of reg. */
        if (!rk_i2chid_read(reg, s->rxbuf, cnt)) {
            rk_i2c_raise(s, REG_INT_NAKRCV);
            return;
        }
        rk_i2c_raise(s, REG_INT_MBRF);
    } else {
        rk_i2c_raise(s, REG_INT_NAKRCV);
    }
}

static uint64_t rk_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    struct RkI2cState *s = opaque;

    switch (offset) {
    case REG_CON:
        return s->con;
    case REG_CLKDIV:
        return s->clkdiv;
    case REG_MRXADDR:
        return s->mrxaddr;
    case REG_MRXRADDR:
        return s->mrxraddr;
    case REG_IEN:
        return s->ien;
    case REG_IPD:
        return s->ipd;
    default:
        break;
    }
    if (offset >= TXBUFFER_BASE && offset < TXBUFFER_BASE + 32) {
        uint32_t w;
        memcpy(&w, &s->txbuf[offset - TXBUFFER_BASE], 4);
        return w;
    }
    if (offset >= RXBUFFER_BASE && offset < RXBUFFER_BASE + 32) {
        uint32_t w;
        memcpy(&w, &s->rxbuf[offset - RXBUFFER_BASE], 4);
        return w;
    }
    return 0;
}

static void rk_i2c_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    struct RkI2cState *s = opaque;

    switch (offset) {
    case REG_CON:
        s->con = value;
        if (value & REG_CON_START) {
            rk_i2c_raise(s, REG_INT_START);
        }
        if (value & REG_CON_STOP) {
            rk_i2c_raise(s, REG_INT_STOP);
        }
        return;
    case REG_CLKDIV:
        s->clkdiv = value;
        return;
    case REG_MRXADDR:
        s->mrxaddr = value;
        return;
    case REG_MRXRADDR:
        s->mrxraddr = value;
        return;
    case REG_MTXCNT:
        rk_i2c_do_tx(s, value);
        return;
    case REG_MRXCNT:
        rk_i2c_do_rx(s, value);
        return;
    case REG_IEN:
        s->ien = value;
        rk_i2c_update_irq(s);
        return;
    case REG_IPD:
        s->ipd &= ~(uint32_t)value;      /* write-1-to-clear */
        rk_i2c_update_irq(s);
        return;
    default:
        break;
    }
    if (offset >= TXBUFFER_BASE && offset < TXBUFFER_BASE + 32) {
        memcpy(&s->txbuf[offset - TXBUFFER_BASE], &value, 4);
    }
}

static const MemoryRegionOps rk_i2c_ops = {
    .read = rk_i2c_read,
    .write = rk_i2c_write,
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

static void rk_i2c_reset_hold(Object *obj, ResetType type)
{
    struct RkI2cState *s = RK3588_I2C(obj);

    s->con = 0;
    s->clkdiv = 0;
    s->mrxaddr = 0;
    s->mrxraddr = 0;
    s->ien = 0;
    s->ipd = 0;
    memset(s->txbuf, 0, sizeof(s->txbuf));
    memset(s->rxbuf, 0, sizeof(s->rxbuf));
    rk_i2c_update_irq(s);
}

static void rk_i2c_realize(DeviceState *dev, Error **errp)
{
    struct RkI2cState *s = RK3588_I2C(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rk_i2c_ops, s,
                          "rk3588.i2c", RK_I2C_REG_WINDOW);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    if (s->has_touchpad) {
        rk_i2chid_setup(dev);       /* input handler for the touchpad */
    }
}

static const Property rk_i2c_props[] = {
    DEFINE_PROP_BOOL("has-rtc", struct RkI2cState, has_rtc, false),
    DEFINE_PROP_BOOL("has-touchpad", struct RkI2cState, has_touchpad, false),
};

static void rk_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = rk_i2c_realize;
    rc->phases.hold = rk_i2c_reset_hold;
    dc->desc = "Rockchip RK3588 I2C (rk3x) controller";
    device_class_set_props(dc, rk_i2c_props);
}

static const TypeInfo rk_i2c_type = {
    .name = TYPE_RK3588_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(struct RkI2cState),
    .class_init = rk_i2c_class_init,
};

static void rk_i2c_register_types(void)
{
    type_register_static(&rk_i2c_type);
}
type_init(rk_i2c_register_types)
