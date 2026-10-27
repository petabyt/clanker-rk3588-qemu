/*
 * Rockchip RK3588 SPI controller ("rockchip,rk3588-spi"/"rk3066-spi"),
 * PIO + IRQ, with the board's rk806 PMIC slave on spi2.
 *
 * A register-storage stub cannot work here: the Linux driver
 * (spi-rockchip.c) starts every transfer with rockchip_spi_prepare_irq(),
 * which returns 1 (EINPROGRESS), and only rockchip_spi_isr() ->
 * spi_finalize_current_transfer() completes it.  Without the SPI IRQ the
 * core waits out spi_transfer_wait()'s (2*8*len/speed + 200 ms) budget and
 * fails with "SPI transfer timed out" (-ETIMEDOUT), which is exactly what
 * the previous stub produced for the rk806: probe failed in ~30 ms and the
 * PMIC's regulators never appeared, deferring 7 devices behind them
 * (gpu/panthor, saradc, SD slot, cpufreq, adc-keys, ...).
 *
 * Register map (spi-rockchip.c):
 *   CTRLR0 0x00   CTRL1 0x04   SSIENR 0x08   SER    0x0c   BAUDR  0x10
 *   TXFTLR 0x14   RXFTLR 0x18  TXFLR  0x1c   RXFLR  0x20   SR     0x24
 *   IPR 0x28      IMR   0x2c   ISR    0x30   RISR   0x34   ICR    0x38
 *   DMACR 0x3c    DMATDLR 0x40  DMARDLR 0x44  VERSION 0x48
 *   TXDR 0x400 (data)   RXDR 0x800 (data)
 *
 * SR bits: BUSY 0, TF_FULL 1, TF_EMPTY 2, RF_EMPTY 3, RF_FULL 4.
 * ISR/RISR bits: TF_EMPTY 0, TF_OVF 1, RF_UNDER 2, RF_OVER 3, RF_FULL 4,
 * CS_INACTIVE 6.  The driver masks only INT_RF_FULL (bit 4).
 *
 * Handshake modelled by reacting to the writes the driver makes (it is the
 * bus master and drives every step):
 *   - SER bit0 change    CS asserted -> start of message (parser reset);
 *                        CS deassert only ends the message (the parser is
 *                        kept across it: spi_write_then_read() is TWO
 *                        messages - command+address, then rx-only data).
 *                        A new assert resets only a completed/idle frame.
 *   - RXFTLR write       rockchip_spi_config() runs per spi_transfer, so
 *                        this marks a transfer boundary: stage the pending
 *                        rk806 read-data words (an rx-only transfer has no
 *                        TXDR writes to react to).
 *   - TXDR write         one word clocked out -> feed the frame parser and
 *                        clock one RX word back (SPI is full duplex).
 *   - RXFLR / RXDR read  count / pop the staged words.
 *   - IMR / ICR write    re-evaluate the level IRQ line.
 * IRQ level = (RXFLR > RXFTLR) & IMR.RF_FULL, i.e. exactly the interrupt
 * the PIO ISR drains on.
 *
 * rk806 frame (drivers/mfd/rk8xx-spi.c, 16-bit reg / 8-bit val):
 *   byte0  cmd: bit7 = 1 write / 0 read, bits3:0 = (value bytes - 1)
 *   byte1  register address low   (reg_format_endian = LITTLE)
 *   byte2  register address high
 *   byte3+ write data / (rx-only transfer on reads)
 *
 * The slave is a 256-byte register file (RK806 max_register = 0xff) that
 * echoes writes and reads back stored bytes: that satisfies everything
 * rk8xx_probe() does - regmap-irq mask write + readback (0x78/0x7a),
 * rk806_pre_init_reg[], SYS_CFG3 update_bits - and the regulators are
 * described by DT children, not by register reads.  Reads with no slave
 * on the bus clock out 0xff (nobody answered).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "rk3588-spi.h"

#define RK_SPI_REG_WINDOW  0x1000
#define RK_SPI_FIFO_LEN    32
#define RK806_REG_COUNT    0x100   /* RK806_BUCK_RSERVE_REG5 = 0xff */

/* Register offsets (spi-rockchip.c) */
#define REG_CTRLR0   0x00
#define REG_CTRL1    0x04
#define REG_SSIENR   0x08
#define REG_SER      0x0c
#define REG_BAUDR    0x10
#define REG_TXFTLR   0x14
#define REG_RXFTLR   0x18
#define REG_TXFLR    0x1c
#define REG_RXFLR    0x20
#define REG_SR       0x24
#define REG_IPR      0x28
#define REG_IMR      0x2c
#define REG_ISR      0x30
#define REG_RISR     0x34
#define REG_ICR      0x38
#define REG_DMACR    0x3c
#define REG_DMATDLR  0x40
#define REG_DMARDLR  0x44
#define REG_VERSION  0x48
#define REG_TXDR     0x400
#define REG_RXDR     0x800

/* SR bits */
#define SR_BUSY      (1u << 0)
#define SR_TF_FULL   (1u << 1)
#define SR_TF_EMPTY  (1u << 2)
#define SR_RF_EMPTY  (1u << 3)
#define SR_RF_FULL   (1u << 4)

/* ISR/RISR/IMR bits */
#define INT_TF_EMPTY (1u << 0)
#define INT_RF_FULL  (1u << 4)
#define INT_CS_INACT (1u << 6)

/* rk806 frame parser states */
enum {
    P_IDLE,         /* awaiting the cmd byte (or completed frame) */
    P_ADDR,         /* cmd seen, collecting the two address bytes */
    P_WRITE,        /* collecting `left` data bytes */
    P_READ,         /* frame parsed, read data staged but not clocked */
};

struct RkSpiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    /* controller registers (storage that the driver never re-reads) */
    uint32_t ctrlr0, ctrlr1, ssi_enr, ser, baudr;
    uint32_t txftlr, rxftlr, ipr, imr, dmacr;
    uint32_t dmatdlr, dmardlr;

    /* RX FIFO of the current transfer */
    uint32_t rx_fifo[RK_SPI_FIFO_LEN];
    unsigned rx_count;

    /* rk806 frame parser */
    bool has_pmic;
    int pstate;
    bool p_write;
    unsigned p_left;            /* bytes still expected in P_WRITE */
    unsigned p_pos;             /* data bytes stored so far */
    uint32_t p_addr;
    uint8_t pmic_regs[RK806_REG_COUNT];
    uint8_t stage[RK_SPI_FIFO_LEN];
    unsigned stage_len;
};

static void rk_spi_update_irq(struct RkSpiState *s)
{
    bool rf_full = s->rx_count > s->rxftlr;

    qemu_set_irq(s->irq, rf_full && (s->imr & INT_RF_FULL));
}

static void rk_spi_rx_push(struct RkSpiState *s, uint32_t word)
{
    if (s->rx_count >= RK_SPI_FIFO_LEN) {
        return;                 /* overflow: drop, the driver never does this */
    }
    s->rx_fifo[s->rx_count++] = word;
    rk_spi_update_irq(s);
}

/* End of a completed/idle frame on CS assert; a pending read frame
 * (spi_write_then_read splits cmd and data into two messages) survives. */
static void rk_spi_cs_changed(struct RkSpiState *s, bool asserted)
{
    if (asserted && s->pstate != P_READ) {
        s->pstate = P_IDLE;
        s->p_left = 0;
        s->p_pos = 0;
    }
}

static void rk_spi_frame_byte(struct RkSpiState *s, uint8_t b)
{
    switch (s->pstate) {
    case P_IDLE:
        s->p_write = (b & 0x80) != 0;
        s->p_left = (b & 0x0f) + 1;
        s->p_addr = 0;
        s->p_pos = 0;
        s->pstate = P_ADDR;
        s->stage_len = 0;
        return;
    case P_ADDR:
        s->p_addr |= (uint32_t)b << (8 * s->p_pos++);   /* little-endian */
        if (s->p_pos < 2) {
            return;
        }
        if (s->p_write) {
            s->pstate = s->p_left ? P_WRITE : P_IDLE;
            s->p_pos = 0;
            return;
        }
        /* read: stage the register file contents for the rx-only xfer */
        for (unsigned i = 0; i < s->p_left && i < RK_SPI_FIFO_LEN; i++) {
            unsigned a = (s->p_addr + i) & (RK806_REG_COUNT - 1);

            s->stage[i] = s->has_pmic ? s->pmic_regs[a] : 0xff;
        }
        s->stage_len = (s->p_left > RK_SPI_FIFO_LEN) ? RK_SPI_FIFO_LEN
                                                     : s->p_left;
        s->pstate = P_READ;
        return;
    case P_WRITE:
        if (s->has_pmic && s->p_addr < RK806_REG_COUNT) {
            s->pmic_regs[s->p_addr++ & (RK806_REG_COUNT - 1)] = b;
        } else {
            s->p_addr++;
        }
        if (--s->p_left == 0) {
            s->pstate = P_IDLE; /* frame complete */
        }
        return;
    default:
        return;                 /* P_READ: extra bytes cannot happen */
    }
}

static uint64_t rk_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    struct RkSpiState *s = opaque;

    switch (offset) {
    case REG_CTRLR0:
        return s->ctrlr0;
    case REG_CTRL1:
        return s->ctrlr1;
    case REG_SSIENR:
        return s->ssi_enr;
    case REG_SER:
        return s->ser;
    case REG_BAUDR:
        return s->baudr;
    case REG_TXFTLR:
        return s->txftlr;
    case REG_RXFTLR:
        return s->rxftlr;
    case REG_TXFLR:
        return 0;               /* TX words are consumed as they arrive */
    case REG_RXFLR:
        return s->rx_count;
    case REG_SR:
        return SR_TF_EMPTY | (s->rx_count ? 0 : SR_RF_EMPTY) |
               ((s->rx_count >= RK_SPI_FIFO_LEN) ? SR_RF_FULL : 0);
    case REG_IPR:
        return s->ipr;
    case REG_IMR:
        return s->imr;
    case REG_ISR:
    case REG_RISR:
        return (s->rx_count > s->rxftlr) ? INT_RF_FULL : 0;
    case REG_DMACR:
        return s->dmacr;
    case REG_DMATDLR:
        return s->dmatdlr;
    case REG_DMARDLR:
        return s->dmardlr;
    case REG_VERSION:
        return 0;               /* not VER2_TYPE1/2 -> driver uses 32 words */
    case REG_RXDR: {
        uint32_t w = s->rx_count ? s->rx_fifo[0] : 0;

        if (s->rx_count) {
            memmove(s->rx_fifo, s->rx_fifo + 1,
                    (s->rx_count - 1) * sizeof(s->rx_fifo[0]));
            s->rx_count--;
        }
        rk_spi_update_irq(s);
        return w;
    }
    default:
        break;
    }
    return 0;
}

static void rk_spi_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    struct RkSpiState *s = opaque;
    bool cs;

    switch (offset) {
    case REG_CTRLR0:
        s->ctrlr0 = value;
        return;
    case REG_CTRL1:
        s->ctrlr1 = value;
        return;
    case REG_SSIENR:
        s->ssi_enr = value;
        rk_spi_update_irq(s);
        return;
    case REG_SER:
        cs = (value & 1) != 0;
        if (((s->ser & 1) != 0) != cs) {
            rk_spi_cs_changed(s, cs);
        }
        s->ser = value;
        return;
    case REG_BAUDR:
        s->baudr = value;
        return;
    case REG_TXFTLR:
        s->txftlr = value;
        return;
    case REG_RXFTLR:
        /* transfer boundary (rockchip_spi_config runs per transfer): if a
         * rk806 read frame is waiting for its rx-only transfer, clock its
         * data out now - there will be no TXDR writes to react to. */
        s->rxftlr = value;
        if (s->pstate == P_READ && s->stage_len) {
            for (unsigned i = 0; i < s->stage_len; i++) {
                rk_spi_rx_push(s, s->stage[i]);
            }
            s->stage_len = 0;
            s->pstate = P_IDLE;
        }
        return;
    case REG_IMR:
        s->imr = value;
        rk_spi_update_irq(s);
        return;
    case REG_ICR:
        rk_spi_update_irq(s);   /* W1C: nothing sticky is modelled */
        return;
    case REG_DMACR:
        s->dmacr = value;
        return;
    case REG_DMATDLR:
        s->dmatdlr = value;
        return;
    case REG_DMARDLR:
        s->dmardlr = value;
        return;
    case REG_TXDR:
        if (!s->ssi_enr || !(s->ser & 1)) {
            return;             /* controller disabled or CS deasserted */
        }
        rk_spi_frame_byte(s, value & 0xff);
        /* Full duplex: every clocked word returns one.  The echo of the
         * command/address phase is meaningless to the master; a rk806 read
         * answers with its staged register bytes via the RXFTLR hook. */
        rk_spi_rx_push(s, 0xff);
        return;
    default:
        break;
    }
}

static const MemoryRegionOps rk_spi_ops = {
    .read = rk_spi_read,
    .write = rk_spi_write,
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

static void rk_spi_reset_hold(Object *obj, ResetType type)
{
    struct RkSpiState *s = RK3588_SPI(obj);

    s->ctrlr0 = s->ctrlr1 = s->ssi_enr = s->ser = s->baudr = 0;
    s->txftlr = s->rxftlr = s->ipr = s->imr = s->dmacr = 0;
    s->dmatdlr = s->dmardlr = 0;
    s->rx_count = 0;
    s->pstate = P_IDLE;
    s->p_left = s->p_pos = 0;
    s->stage_len = 0;
    memset(s->rx_fifo, 0, sizeof(s->rx_fifo));
    memset(s->stage, 0, sizeof(s->stage));
    /* External chip: a reset of the SoC power-cycles its view of the
     * PMIC, and rk8xx_probe() rewrites its pre-init registers anyway. */
    memset(s->pmic_regs, 0, sizeof(s->pmic_regs));
    qemu_set_irq(s->irq, 0);
}

static void rk_spi_realize(DeviceState *dev, Error **errp)
{
    struct RkSpiState *s = RK3588_SPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rk_spi_ops, s,
                          "rk3588.spi", RK_SPI_REG_WINDOW);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const Property rk_spi_props[] = {
    DEFINE_PROP_BOOL("has-pmic", struct RkSpiState, has_pmic, false),
};

static void rk_spi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = rk_spi_realize;
    rc->phases.hold = rk_spi_reset_hold;
    dc->desc = "Rockchip RK3588 SPI (DesignWare) controller";
    device_class_set_props(dc, rk_spi_props);
}

static const TypeInfo rk_spi_type = {
    .name = TYPE_RK3588_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(struct RkSpiState),
    .class_init = rk_spi_class_init,
};

static void rk_spi_register_types(void)
{
    type_register_static(&rk_spi_type);
}
type_init(rk_spi_register_types)