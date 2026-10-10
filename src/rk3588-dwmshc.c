/*
 * RK3588 SD slot controller: DesignWare mobile storage host (dw-mshc).
 *
 * Register layout and bit definitions follow the DesignWare MSHC spec as
 * used by u-boot/include/dwmmc.h (the stock u-boot SD driver) and the
 * vendor SPL's dw-mshc node (rockchip,rk3588-dw-mshc, fifo-depth 0x100).
 *
 * Model scope (P10 Phase 1):
 *  - CMD: response + command dispatch to the attached SD card (sdbus).
 *  - Data: IDMAC (CTRL.IDMAC_EN, 32-bit chained descriptors, processed
 *    synchronously when the command is issued) and PIO (FIFO at 0x200).
 *  - Card detect (CDETECT) and write protect (WRTPRT) come from the card.
 *  - No interrupts (polled); no clock modelling (CLKDIV/CLKENA stored).
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/sd/sd.h"
#include "system/memory.h"
#include "system/dma.h"
#include "qom/object.h"
#include "rk3588-dwmshc.h"

bool rk3588_sd_card_inserted;

#define TYPE_RK3588_DWMSHC_BUS "rk3588-dwmshc-bus"

/* Register offsets (u-boot/include/dwmmc.h) */
#define CTRL        0x000
#define CLKDIV      0x008
#define CLKENA      0x010
#define CTYPE       0x018
#define BLKSIZ      0x01c
#define BYTCNT      0x020
#define INTMASK     0x024
#define CMDARG      0x028
#define CMD         0x02c
#define RESP0       0x030
#define RESP1       0x034
#define RESP2       0x038
#define RESP3       0x03c
#define MINTSTS     0x040
#define RINTSTS     0x044
#define STATUS      0x048
#define FIFOTH      0x04c
#define CDETECT     0x050
#define WRTPRT      0x054
#define VERID       0x06c
#define HCON        0x070
#define BMOD        0x080
#define PLDMND      0x084
#define DATA        0x200
#define DBADDR      0x088
#define IDSTS       0x08c
#define IDINTEN     0x090
#define DSCADDR     0x094
#define BUFADDR     0x098

#define REGS_BYTES  0x100   /* control/status register file */

/* CTRL */
#define CTRL_RESET        (1u << 0)
#define CTRL_FIFO_RESET   (1u << 1)
#define CTRL_DMA_RESET    (1u << 2)
#define CTRL_IDMAC_EN     (1u << 25)

/* CMD */
#define CMD_RESP_EXP      (1u << 6)
#define CMD_RESP_LEN      (1u << 7)
#define CMD_DATA_EXP      (1u << 9)
#define CMD_RW_WRITE     (1u << 10)
#define CMD_SEND_STOP     (1u << 12)
#define CMD_UPD_CLK       (1u << 21)
#define CMD_START         (1u << 31)
#define CMD_INDEX_MASK    0x3fu

/* RINTSTS */
#define INT_RTO           (1u << 8)
#define INT_CMD_DONE      (1u << 2)
#define INT_DTO           (1u << 3)
#define INT_TXDR          (1u << 4)
#define INT_RXDR          (1u << 5)
#define INT_ACD           (1u << 14)  /* auto command (CMD12) done */

/* STATUS */
#define STATUS_FIFO_EMPTY (1u << 2)

/* IDMAC descriptor (des0) and IDSTS */
#define IDMAC_OWN         (1u << 31)
#define IDMAC_CH          (1u << 4)
#define IDMAC_LD          (1u << 2)
#define IDSTS_TI          (1u << 0)
#define IDSTS_RI          (1u << 1)
#define IDSTS_NI          (1u << 8)
#define IDMAC_DESC_SIZE   16

/* BMOD */
#define BMOD_RESET        (1u << 0)

/* VERID: DesignWare MSHC 2.40a */
#define VERID_VALUE       0x5240270au

typedef struct RK3588DwMshcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    SDBus sdbus;
    uint32_t regs[REGS_BYTES / 4];
    uint32_t data_left;     /* PIO bytes still to move */
    bool data_write;        /* PIO direction: host -> card */
    bool autostop;          /* SEND_AUTO_STOP set on the data command */
    bool stop_cmd;          /* multi-block: issue CMD12 at end of data */
} RK3588DwMshcState;

OBJECT_DECLARE_SIMPLE_TYPE(RK3588DwMshcState, RK3588_DWMSHC)

static void dwmshc_reset(DeviceState *dev)
{
    RK3588DwMshcState *s = RK3588_DWMSHC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->data_left = 0;
    s->data_write = false;
    s->autostop = false;
    s->stop_cmd = false;
    /*
     * The card was attached before reset; sd.c only signals insertion on
     * media-change events, so take the state from the bus here.
     */
    rk3588_sd_card_inserted = sdbus_get_inserted(&s->sdbus);
}

static void dwmshc_set_data_done(RK3588DwMshcState *s)
{
    /* FIFO request bits drop once no data is pending (hardware behaviour). */
    s->regs[RINTSTS / 4] &= ~(INT_TXDR | INT_RXDR);
    s->regs[RINTSTS / 4] |= INT_DTO;

    if (s->autostop) {
        /* Auto-stop after the data phase: CMD12 for multi-block, then ACD. */
        if (s->stop_cmd) {
            SDRequest stop = { .cmd = 12, .arg = 0 };
            uint8_t rsp[16] = {0};

            sdbus_do_command(&s->sdbus, &stop, rsp, sizeof(rsp));
        }
        s->autostop = false;
        s->regs[RINTSTS / 4] |= INT_ACD;
    }
}

/*
 * FIFO request level: TXDR/RXDR stay asserted while PIO data remains and the
 * FIFO can move a word.  The model's FIFO never fills, so the request comes
 * straight back after the guest acknowledges it.
 */
static void dwmshc_fifo_request(RK3588DwMshcState *s)
{
    if (s->data_left == 0) {
        return;
    }
    s->regs[RINTSTS / 4] |= s->data_write ? INT_TXDR : INT_RXDR;
}

/* Byte-wise card access: sdbus_read_data asserts on a zero-length read. */
static void dwmshc_card_read(RK3588DwMshcState *s, uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = sdbus_read_byte(&s->sdbus);
    }
}

static void dwmshc_card_write(RK3588DwMshcState *s, const uint8_t *buf,
                              size_t len)
{
    for (size_t i = 0; i < len; i++) {
        sdbus_write_byte(&s->sdbus, buf[i]);
    }
}

/*
 * IDMAC: walk the descriptor chain from DBADDR while OWN is set.  Done
 * synchronously here, so the guest sees the whole transfer complete by the
 * time it polls RINTSTS.
 */
static void dwmshc_idmac_transfer(RK3588DwMshcState *s, bool write)
{
    AddressSpace *as = &address_space_memory;
    uint32_t desc = s->regs[DBADDR / 4];
    unsigned guard = 0;

    while (guard++ < 65536) {
        uint8_t raw[IDMAC_DESC_SIZE];
        uint32_t des0, des1, des2, des3, len;
        uint8_t *buf;

        if (dma_memory_read(as, desc, raw, sizeof(raw),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            break;
        }
        des0 = ldl_le_p(raw + 0);
        des1 = ldl_le_p(raw + 4);
        des2 = ldl_le_p(raw + 8);
        des3 = ldl_le_p(raw + 12);
        if (!(des0 & IDMAC_OWN)) {
            break;
        }

        len = des1 & 0x1fff;
        if (len) {
            buf = g_malloc(len);
            if (write) {
                dma_memory_read(as, des2, buf, len, MEMTXATTRS_UNSPECIFIED);
                dwmshc_card_write(s, buf, len);
            } else {
                dwmshc_card_read(s, buf, len);
                dma_memory_write(as, des2, buf, len, MEMTXATTRS_UNSPECIFIED);
            }
            g_free(buf);
        }

        /* Hand the descriptor back to the host. */
        des0 &= ~IDMAC_OWN;
        stl_le_p(raw, des0);
        dma_memory_write(as, desc, raw, 4, MEMTXATTRS_UNSPECIFIED);

        s->regs[IDSTS / 4] |= (write ? IDSTS_TI : IDSTS_RI) | IDSTS_NI;
        s->regs[DSCADDR / 4] = desc;
        s->regs[BUFADDR / 4] = des2;

        if (des0 & IDMAC_LD) {
            break;
        }
        /* CH: des3 is the next descriptor; otherwise descriptors are packed. */
        desc = (des0 & IDMAC_CH) ? des3 : desc + IDMAC_DESC_SIZE;
    }
    dwmshc_set_data_done(s);
}

static void dwmshc_data_start(RK3588DwMshcState *s, uint32_t cmd)
{
    bool write = cmd & CMD_RW_WRITE;

    /* SEND_AUTO_STOP completes with ACD; CMD12 itself is multi-block only. */
    s->autostop = cmd & CMD_SEND_STOP;
    s->stop_cmd = s->regs[BYTCNT / 4] > s->regs[BLKSIZ / 4];

    if (s->regs[CTRL / 4] & CTRL_IDMAC_EN) {
        dwmshc_idmac_transfer(s, write);
        return;
    }

    /* PIO: the guest moves data through the FIFO window at 0x200. */
    s->data_left = s->regs[BYTCNT / 4];
    s->data_write = write;
    if (s->data_left == 0) {
        dwmshc_set_data_done(s);
        return;
    }
    s->regs[RINTSTS / 4] |= write ? INT_TXDR : INT_RXDR;
}

static void dwmshc_command(RK3588DwMshcState *s, uint32_t cmd)
{
    SDRequest req;
    uint8_t rsp[16] = {0};
    size_t rlen;
    bool expect = cmd & CMD_RESP_EXP;

    if (cmd & CMD_UPD_CLK) {
        return;     /* clock update only, no card command */
    }

    req.cmd = cmd & CMD_INDEX_MASK;
    req.arg = s->regs[CMDARG / 4];
    rlen = sdbus_do_command(&s->sdbus, &req, rsp, sizeof(rsp));

    if (expect) {
        if (rlen == 0) {
            s->regs[RINTSTS / 4] |= INT_RTO;    /* no card, or no answer */
        } else if (cmd & CMD_RESP_LEN) {
            if (rlen == 16) {
                s->regs[RESP0 / 4] = ldl_be_p(&rsp[12]);
                s->regs[RESP1 / 4] = ldl_be_p(&rsp[8]);
                s->regs[RESP2 / 4] = ldl_be_p(&rsp[4]);
                s->regs[RESP3 / 4] = ldl_be_p(&rsp[0]);
            }
        } else {
            s->regs[RESP0 / 4] = ldl_be_p(&rsp[0]);
        }
    }
    s->regs[RINTSTS / 4] |= INT_CMD_DONE;

    /* Data phase only runs when the command was answered. */
    if ((cmd & CMD_DATA_EXP) && (rlen > 0 || !expect)) {
        dwmshc_data_start(s, cmd);
    }
}

static uint32_t dwmshc_fifo_read(RK3588DwMshcState *s)
{
    uint8_t b[4] = {0};
    unsigned n;

    if (s->data_left == 0 || s->data_write) {
        return 0;
    }
    n = MIN(4u, s->data_left);
    dwmshc_card_read(s, b, n);
    s->data_left -= n;
    if (s->data_left == 0) {
        dwmshc_set_data_done(s);
    } else {
        dwmshc_fifo_request(s);
    }
    return ldl_le_p(b);
}

static void dwmshc_fifo_write(RK3588DwMshcState *s, uint32_t val)
{
    uint8_t b[4];
    unsigned n;

    if (s->data_left == 0 || !s->data_write) {
        return;
    }
    stl_le_p(b, val);
    n = MIN(4u, s->data_left);
    dwmshc_card_write(s, b, n);
    s->data_left -= n;
    if (s->data_left == 0) {
        dwmshc_set_data_done(s);
    } else {
        dwmshc_fifo_request(s);
    }
}

static uint64_t rk3588_dwmshc_read(void *opaque, hwaddr off, unsigned size)
{
    RK3588DwMshcState *s = opaque;
    uint32_t val;

    if (off == DATA) {
        return dwmshc_fifo_read(s);
    }
    if (off >= REGS_BYTES) {
        qemu_log_mask(LOG_UNIMP, "rk3588-dwmshc: read @%#" HWADDR_PRIx "\n",
                      off);
        return 0;
    }

    switch (off) {
    case CDETECT:           /* bit n = 1 means no card (active low) */
        val = rk3588_sd_card_inserted ? 0 : 1;
        break;
    case WRTPRT:
        val = 0;
        break;
    case STATUS:
        /*
         * PIO receive: FIFO holds the bytes still to be read (FIFO_COUNT in
         * words, bits 29:17).  Otherwise empty.
         */
        if (s->data_left && !s->data_write) {
            uint32_t words = MIN((s->data_left + 3) / 4, 0x100u);

            val = words << 17;
        } else {
            val = STATUS_FIFO_EMPTY;
        }
        break;
    case MINTSTS:
        val = s->regs[RINTSTS / 4] & s->regs[INTMASK / 4];
        break;
    case VERID:
        val = VERID_VALUE;
        break;
    case HCON:
        val = 0;
        break;
    default:
        val = s->regs[off / 4];
        break;
    }
    return val;
}

static void rk3588_dwmshc_write(void *opaque, hwaddr off, uint64_t value,
                                unsigned size)
{
    RK3588DwMshcState *s = opaque;
    uint32_t val = value;

    if (off == DATA) {
        dwmshc_fifo_write(s, val);
        return;
    }
    if (off >= REGS_BYTES) {
        qemu_log_mask(LOG_UNIMP, "rk3588-dwmshc: write @%#" HWADDR_PRIx
                      " = %#x\n", off, val);
        return;
    }

    switch (off) {
    case CTRL:
        if (val & CTRL_RESET) {
            dwmshc_reset(DEVICE(s));
        }
        /* Self-clearing reset bits. */
        s->regs[CTRL / 4] = val & ~(CTRL_RESET | CTRL_FIFO_RESET |
                                    CTRL_DMA_RESET);
        if (val & (CTRL_FIFO_RESET | CTRL_DMA_RESET)) {
            s->data_left = 0;
        }
        break;
    case CMD:
        s->regs[CMD / 4] = val & ~CMD_START;
        if (val & CMD_START) {
            dwmshc_command(s, val);
        }
        break;
    case RINTSTS:
        s->regs[RINTSTS / 4] &= ~val;      /* write 1 to clear */
        dwmshc_fifo_request(s);            /* FIFO still ready if data left */
        break;
    case IDSTS:
        s->regs[IDSTS / 4] &= ~val;
        break;
    case BMOD:
        s->regs[BMOD / 4] = val & ~BMOD_RESET;
        break;
    case PLDMND:
        /* Poll demand: descriptors are processed at command time. */
        break;
    case STATUS:
    case CDETECT:
    case WRTPRT:
    case MINTSTS:
    case VERID:
    case HCON:
        break;                              /* read-only */
    default:
        s->regs[off / 4] = val;
        break;
    }
}

static const MemoryRegionOps rk3588_dwmshc_ops = {
    .read = rk3588_dwmshc_read,
    .write = rk3588_dwmshc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rk3588_dwmshc_insert(DeviceState *dev, bool inserted)
{
    rk3588_sd_card_inserted = inserted;
}

static void rk3588_dwmshc_bus_class_init(ObjectClass *klass, const void *data)
{
    SDBusClass *sbc = SD_BUS_CLASS(klass);

    sbc->set_inserted = rk3588_dwmshc_insert;
}

static void rk3588_dwmshc_init(Object *obj)
{
    RK3588DwMshcState *s = RK3588_DWMSHC(obj);

    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_RK3588_DWMSHC_BUS,
              DEVICE(s), "sd-bus");
    memory_region_init_io(&s->iomem, obj, &rk3588_dwmshc_ops, s,
                          TYPE_RK3588_DWMSHC, RK3588_SDMMC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static void rk3588_dwmshc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, dwmshc_reset);
}

static const TypeInfo rk3588_dwmshc_types[] = {
    {
        .name           = TYPE_RK3588_DWMSHC,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(RK3588DwMshcState),
        .class_init     = rk3588_dwmshc_class_init,
        .instance_init  = rk3588_dwmshc_init,
    },
    {
        .name           = TYPE_RK3588_DWMSHC_BUS,
        .parent         = TYPE_SD_BUS,
        .instance_size  = sizeof(SDBus),
        .class_init     = rk3588_dwmshc_bus_class_init,
    },
};

DEFINE_TYPES(rk3588_dwmshc_types)
