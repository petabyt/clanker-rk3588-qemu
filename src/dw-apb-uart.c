/*
 * DesignWare APB UART (DW_apb-uart), as used for RK3588 UART0-UART5.
 *
 * Register set covers what rk/src/pl011.c (the firmware's DW UART
 * driver) touches: RBR/THR/DLL at 0x00, IER/DLM at 0x04, IIR/FCR at
 * 0x08, LCR/MCR/LSR/MSR/SCR, shadow regs 0x30-0x6C, USR, SRR and the
 * FIFO control block.  TX drains synchronously into the char backend;
 * RX is a small FIFO.  IIR/IER are modelled well enough for
 * interrupt-driven 8250 guests later (U-Boot/Linux poll, but the
 * firmware enables ERBFI).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "chardev/char-serial.h"
#include "qemu/module.h"
#include "dw-apb-uart.h"

#define DW_UART_REG_WINDOW 0x1000
#define DW_UART_RX_FIFO    16

/* LSR */
#define DW_LSR_DR   (1 << 0)
#define DW_LSR_THRE (1 << 5)
#define DW_LSR_TEMT (1 << 6)

/* IIR */
#define DW_IIR_IP     (1 << 0)      /* 0 = interrupt pending */
#define DW_IIR_ID_TX  (0x1 << 1)    /* THR empty */
#define DW_IIR_ID_RX  (0x6 << 1)    /* received data available */
#define DW_IIR_FIFOEN (3 << 6)

/* MCR */
#define DW_MCR_DTR   (1 << 0)
#define DW_MCR_RTS   (1 << 1)
#define DW_MCR_OUT1  (1 << 2)
#define DW_MCR_OUT2  (1 << 3)
#define DW_MCR_LOOP  (1 << 4)

/* MSR: status bits 4-7, W1C delta bits 0-3 */
#define DW_MSR_DCTS  (1 << 0)
#define DW_MSR_DDSR  (1 << 1)
#define DW_MSR_DRI   (1 << 2)
#define DW_MSR_DDCD  (1 << 3)
#define DW_MSR_CTS   (1 << 4)
#define DW_MSR_DSR   (1 << 5)
#define DW_MSR_RI    (1 << 6)
#define DW_MSR_DCD   (1 << 7)

/* USR */
#define DW_USR_TFNF (1 << 0)
#define DW_USR_TFE  (1 << 1)
#define DW_USR_RFNE (1 << 2)
#define DW_USR_RFF  (1 << 3)
/*
 * Bit 2 redefined for this model: the EDK2 firmware's
 * Dw8250SerialPortLib poll-waits for USR&0x04 *after every THR write*
 * ("transmission finished", 500k-read budget per character) before it
 * sends the next byte.  On DW hardware bit 2 is spec-RFNE, which stays
 * clear when no input is pending, so the guest burned the entire
 * budget on each character and serial output crawled (~100 B/s).
 * Both of our guests poll RX readiness via LSR_DR (u-boot never reads
 * USR at all), so nothing observes the RFNE meaning here; TX drains
 * synchronously (qemu_chr_fe_write_all), so "finished" always holds.
 */
#define DW_USR_TX_FINISHED DW_USR_RFNE

struct DWUartState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq;

    uint8_t ier;
    uint8_t fcr;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t msr;
    uint8_t scr;
    uint16_t divisor;
    bool dlab;
    bool thre_ip; /* THRE interrupt pending */

    uint8_t rx_fifo[DW_UART_RX_FIFO];
    int rx_count;
};

static void dw_uart_update_irq(DWUartState *s)
{
    bool rx = (s->rx_count > 0) && (s->ier & 0x01);
    bool tx = s->thre_ip && (s->ier & 0x02);

    qemu_set_irq(s->irq, rx || tx);
}

/*
 * Modem status: in MCR loopback mode the outputs are routed back
 * (RTS->CTS, DTR->DSR, OUT1->RI, OUT2->DCD); outside loopback no
 * modem lines are modelled, so carrier/dsr/cts read as asserted —
 * the Linux 8250 autoconfig loopback test requires the loopback
 * routing (MSR CTS|DCD after MCR LOOP|OUT2|RTS), and the initial
 * console open would otherwise block on the missing carrier.
 */
static void dw_uart_update_msr(DWUartState *s)
{
    uint8_t status;

    if (s->mcr & DW_MCR_LOOP) {
        status = 0;
        if (s->mcr & DW_MCR_RTS) {
            status |= DW_MSR_CTS;
        }
        if (s->mcr & DW_MCR_DTR) {
            status |= DW_MSR_DSR;
        }
        if (s->mcr & DW_MCR_OUT1) {
            status |= DW_MSR_RI;
        }
        if (s->mcr & DW_MCR_OUT2) {
            status |= DW_MSR_DCD;
        }
    } else {
        status = DW_MSR_DCD | DW_MSR_DSR | DW_MSR_CTS;
    }

    if ((status ^ s->msr) & DW_MSR_CTS) {
        s->msr |= DW_MSR_DCTS;
    }
    if ((status ^ s->msr) & DW_MSR_DSR) {
        s->msr |= DW_MSR_DDSR;
    }
    if ((status ^ s->msr) & DW_MSR_RI) {
        s->msr |= DW_MSR_DRI;
    }
    if ((status ^ s->msr) & DW_MSR_DCD) {
        s->msr |= DW_MSR_DDCD;
    }
    s->msr = (s->msr & 0x0f) | status;
}

static void dw_uart_tx(DWUartState *s, uint8_t c)
{
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_write_all(&s->chr, &c, 1);
    }
    /* The transmit path is modelled as instant, so THR is empty again. */
    s->thre_ip = true;
    dw_uart_update_irq(s);
}

static uint8_t dw_uart_rx_pop(DWUartState *s)
{
    uint8_t c = 0;

    if (s->rx_count > 0) {
        c = s->rx_fifo[0];
        memmove(&s->rx_fifo[0], &s->rx_fifo[1], --s->rx_count);
    }
    dw_uart_update_irq(s);
    /* Free RX space: drain a mux/chardev input ring (cf. hw/char/serial.c). */
    qemu_chr_fe_accept_input(&s->chr);
    return c;
}

static uint8_t dw_uart_iir(DWUartState *s)
{
    uint8_t iir = (s->fcr & 0x01) ? DW_IIR_FIFOEN : 0;

    if ((s->ier & 0x01) && s->rx_count > 0) {
        iir |= DW_IIR_ID_RX;
    } else if ((s->ier & 0x02) && s->thre_ip) {
        iir |= DW_IIR_ID_TX;
        s->thre_ip = false; /* reading IIR clears the THRE interrupt */
    } else {
        iir |= DW_IIR_IP;
    }
    dw_uart_update_irq(s);
    return iir;
}

static uint64_t dw_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    DWUartState *s = opaque;
    uint32_t off = addr & ~3U;
    uint32_t v = 0;

    if (off >= 0x30 && off <= 0x6C) {
        off = 0x00; /* shadow RBR */
    }

    switch (off) {
    case 0x00:
        v = s->dlab ? (s->divisor & 0xff) : dw_uart_rx_pop(s);
        break;
    case 0x04:
        v = s->dlab ? (s->divisor >> 8) : s->ier;
        break;
    case 0x08:
        v = dw_uart_iir(s);
        break;
    case 0x0C:
        v = s->lcr;
        break;
    case 0x10:
        v = s->mcr;
        break;
    case 0x14: /* LSR: transmit is always instantaneous */
        v = DW_LSR_THRE | DW_LSR_TEMT;
        if (s->rx_count > 0) {
            v |= DW_LSR_DR;
        }
        break;
    case 0x18: /* MSR: delta bits are W1C */
        v = s->msr;
        s->msr &= 0xf0;
        break;
    case 0x1C:
        v = s->scr;
        break;
    case 0x7C: /* USR */
        v = DW_USR_TFNF | DW_USR_TFE;
        v |= DW_USR_TX_FINISHED;
        if (s->rx_count >= DW_UART_RX_FIFO) {
            v |= DW_USR_RFF;
        }
        break;
    case 0x84: /* RFL */
        v = s->rx_count;
        break;
    default:
        v = 0;
        break;
    }

    if (size < 4) {
        v >>= (addr & 3) * 8;
        v &= (1U << (size * 8)) - 1;
    }
    return v;
}

static void dw_uart_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    DWUartState *s = opaque;
    uint32_t off = addr & ~3U;
    uint32_t v = val;

    if (size < 4) {
        v <<= (addr & 3) * 8;
    }
    if (off >= 0x30 && off <= 0x6C) {
        off = 0x00; /* shadow THR */
    }

    switch (off) {
    case 0x00:
        if (s->dlab) {
            s->divisor = (s->divisor & 0xff00) | (v & 0xff);
        } else {
            dw_uart_tx(s, v & 0xff);
        }
        break;
    case 0x04:
        if (s->dlab) {
            s->divisor = (s->divisor & 0x00ff) | ((v & 0xff) << 8);
        } else {
            /*
             * DW IER implements bits [4:0] (ERBFI/ETBEI/ELSI/EDSSI +
             * PTIME); bits [7:5] are reserved and read back zero —
             * Linux autoconfig() probes IER[6] (Xscale UUE) and would
             * misclassify the port if the full byte stuck.
             */
            s->ier = v & 0x1f;
            if (s->ier & 0x02) {
                s->thre_ip = true; /* THR is empty */
            }
            dw_uart_update_irq(s);
        }
        break;
    case 0x08: /* FCR */
        s->fcr = v & 0xff;
        if (v & 0x02) { /* RX FIFO reset */
            s->rx_count = 0;
            qemu_chr_fe_accept_input(&s->chr);
        }
        dw_uart_update_irq(s);
        break;
    case 0x0C:
        s->lcr = v & 0xff;
        s->dlab = v & 0x80;
        break;
    case 0x10:
        s->mcr = v & 0xff;
        dw_uart_update_msr(s);
        break;
    case 0x14: /* LSR: no W1C state modelled */
    case 0x18: /* MSR */
        break;
    case 0x1C:
        s->scr = v & 0xff;
        break;
    case 0x88: /* SRR: soft reset of the FIFO/UART state */
        if (v & 0x07) {
            s->rx_count = 0;
            s->thre_ip = false;
            qemu_chr_fe_accept_input(&s->chr);
            dw_uart_update_irq(s);
        }
        break;
    default: /* read-only or write-only FIFO control regs */
        break;
    }
}

static const MemoryRegionOps dw_uart_ops = {
    .read = dw_uart_read,
    .write = dw_uart_write,
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

static int dw_uart_can_receive(void *opaque)
{
    DWUartState *s = opaque;

    return s->rx_count < DW_UART_RX_FIFO;
}

static void dw_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    DWUartState *s = opaque;
    int i;

    for (i = 0; i < size && s->rx_count < DW_UART_RX_FIFO; i++) {
        s->rx_fifo[s->rx_count++] = buf[i];
    }
    dw_uart_update_irq(s);
}

static void dw_uart_event(void *opaque, QEMUChrEvent event)
{
    /* No BREAK or modem-control modelling. */
}

static int dw_uart_be_change(void *opaque)
{
    DWUartState *s = opaque;

    qemu_chr_fe_set_handlers(&s->chr, dw_uart_can_receive, dw_uart_receive,
                             dw_uart_event, dw_uart_be_change, s, NULL, true);
    return 0;
}

static void dw_uart_realize(DeviceState *dev, Error **errp)
{
    dw_uart_be_change(DW_APB_UART(dev));
}

static void dw_uart_reset(DeviceState *dev)
{
    DWUartState *s = DW_APB_UART(dev);

    s->ier = 0;
    s->fcr = 0;
    s->lcr = 0;
    s->mcr = 0;
    s->msr = DW_MSR_DCD | DW_MSR_DSR | DW_MSR_CTS;
    s->scr = 0;
    s->divisor = 0;
    s->dlab = false;
    s->thre_ip = false;
    s->rx_count = 0;
}

static void dw_uart_instance_init(Object *obj)
{
    DWUartState *s = DW_APB_UART(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &dw_uart_ops, s,
                          TYPE_DW_APB_UART, DW_UART_REG_WINDOW);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property dw_uart_props[] = {
    DEFINE_PROP_CHR("chardev", DWUartState, chr),
};

static void dw_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = dw_uart_realize;
    device_class_set_legacy_reset(dc, dw_uart_reset);
    device_class_set_props(dc, dw_uart_props);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo dw_uart_type_info = {
    .name = TYPE_DW_APB_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWUartState),
    .instance_init = dw_uart_instance_init,
    .class_init = dw_uart_class_init,
};

static void dw_uart_register_types(void)
{
    type_register_static(&dw_uart_type_info);
}

type_init(dw_uart_register_types)
