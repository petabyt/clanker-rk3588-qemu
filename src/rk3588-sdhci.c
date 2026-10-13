/*
 * RK3588 eMMC host (sdhci@fe2e0000), compatible "rockchip,rk3588-dwcmshc"
 * (mainline driver: sdhci-of-dwcmshc.c, sdhci_dwcmshc_rk3588_pdata).
 *
 * The controller is a Synopsys DWC MSHC SDHCI: a standard SDHCI register
 * block (QEMU's "generic-sdhci", TYPE_SYSBUS_SDHCI, complete with IRQs,
 * ADMA2 and the clock-stable handshake) plus a Rockchip vendor area.
 * Driver facts (7.2.9 sdhci-of-dwcmshc.c):
 *
 *  - P_VENDOR_AREA1 (0xe8) & 0xfff = vendor_specific_area1; must be
 *    non-zero or the driver's init writes land on core SDHCI registers
 *    (area1 + 0x8).  Real rk35xx value 0x500: HOST_CTRL3 0x508 (disable
 *    cmd conflict check), EMMC_CONTROL 0x52c (CARD_IS_EMMC bit 0),
 *    EMMC_ATCTRL 0x540, AT_STAT 0x544.  P_VENDOR_AREA2 (0xea) = 0x540
 *    (the CQHCI window when the DT carries "supports-cqe" - see below).
 *  - DLL block 0x800-0x840: DWCMSHC_EMMC_DLL_{CTRL,RXCLK,TXCLK,STRBIN}
 *    0x800-0x80c, CMDOUT 0x810, MISC_CON 0x81c (MISC_INTCLK_EN bit 1),
 *    DLL_STATUS0 0x840 (bit 8 = locked, bit 9 = timeout).  All vendor
 *    registers are < 0x1000, i.e. inside the inner SDHCI's MMIO window;
 *    the driver treats them as fire-and-forget config except DLL_STATUS0,
 *    which is polled above 52 MHz (HS200/HS400, answered by an overlay).
 *  - rk35xx reset (rk35xx_sdhci_reset) reads DECMSHC_EMMC_MISC_CON,
 *    asserts/deasserts the DT reset array, then rewrites MISC_CON with
 *    MISC_INTCLK_EN.
 *
 * Everything below 0x1000 except 0xe8-0xeb is the inner SDHCI's window;
 * 0x1000-0xffff is unused vendor space and is plain storage so that any
 * access inside the 0x10000 DT reg window is mapped.
 *
 * The card is a QEMU "emmc" device (real eMMC protocol: CMD1/CMD2/CMD3,
 * EXT_CSD with the sector count synthesized from the image size, boot
 * partitions, optional RPMB) attached to the inner sd-bus; images come
 * from -drive if=sd unit 1 (unit 0 is the SD slot, see rk3588.c).
 *
 * eMMC HS200/HS400: capareg also advertises SDR50/SDR104 (BUS_SPEED
 * support), so dwcmshc enters the HS200 path: dwcmshc_rk35xx_set_clock
 * programs the DLL block (0x800-0x81c, plain vendor storage) and polls
 * DLL_STATUS0 (+0x840) for the LOCKED bit - a read-only overlay answers
 * "locked, no timeout" so the poll succeeds.  HS200/HS400 tuning runs via
 * sdhci_send_tuning: CMD21 (SEND_TUNING_BLOCK_HS200) PIO cycles whose
 * completion depends on Buffer Read Ready; the inner QEMU sdhci clears
 * CTRL2 EXECUTE_TUNING and sets SAMPLING_CLKSEL on the first cycle, so
 * Linux converges to "Tuning successful".  HS400/HS400es are then just
 * EXT_CSD switches + vendor EMMC_CONTROL bits (plain storage).
 *
 * CQE (Command Queue Engine): the stock DTB carries "supports-cqe", so
 * dwcmshc_cqhci_init() runs sdhci DWc CQE setup with the CQHCI register
 * file at vendor area 2 (+0x540; note the real chip overlaps EMMC_ATCTRL,
 * vendor area 1 + 0x40, at the same address - the overlay handles both).
 * The engine below is a synchronous model: doorbell (TDBR) writes execute
 * the tagged task immediately through the inner sd-bus (DCMD: any command;
 * data tasks: CMD23 + CMD18/25 + data via the transfer descriptors from
 * the task list at TDLBA + CMD12), then set CQ_IS TCC + TCN and raise the
 * CQE completion bit (INT_STATUS bit 14) in the inner sdhci - dwcmshc's
 * CQE irq chain reads it back out through dwcmshc_cqe_irq_handler ->
 * cqhci_irq.  32-bit ADMA2 (no BUS64BIT in capareg) => 8-byte task
 * descriptors, 16-byte slots, 8-byte transfer descriptors.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "qemu/units.h"
#include "hw/sd/sdhci.h"
#include "hw/sd/sd.h"
#include "system/block-backend.h"
#include "system/address-spaces.h"

#include "rk3588-sdhci.h"

#define RK3588_EMMC_BASE    0xFE2E0000ULL
#define RK3588_EMMC_SIZE    0x10000
#define RK3588_EMMC_SPI     205

/* vendor area 1 = 0x500, DLL block = 0x800, DLL_STATUS0 = 0x840 */
#define RK3588_EMMC_CQ_BASE    (RK3588_EMMC_BASE + 0x540)
#define RK3588_EMMC_CQ_SIZE    0x60
#define RK3588_EMMC_DLL_STATUS0 (RK3588_EMMC_BASE + 0x840)

/*
 * Capabilities (SDHCI spec v3 field layout, hw/sd/sdhci-internal.h):
 * TOCLKFREQ 50 MHz / TOUNIT MHz, BASECLKFREQ 200 MHz, MAXBLOCKLENGTH 2
 * (1024-byte transfers), EMBEDDED_8BIT (8-bit bus), ADMA2, HIGHSPEED,
 * V33, BUS_SPEED SDR50|SDR104 (HS200/HS400 for eMMC).  No UHS-II/64-bit:
 * 32-bit ADMA2 keeps the CQHCI task descriptors at 8 bytes.
 * SLOT_TYPE must stay 0 (removable): QEMU's sdhci_check_capareg() rejects
 * embedded slot types, and the guest mmc core relies on the DT
 * "non-removable" for card-detect semantics anyway.
 *
 * SDMA (bit 22) must be advertised in addition to ADMA2 even though
 * Linux picks ADMA2: QEMU's Transfer Mode register write handler
 * silently strips SDHC_TRNS_DMA (and so degrades every data command
 * to the controller-internal PIO path) unless SDMA support bit is set,
 * while the guest keeps using ADMA-mode buffer interrupts - which
 * then never fire and the request times out.
 */
#define RK3588_EMMC_CAPAREG \
    ((uint64_t)50 | (1 << 7) | (200 << 8) | (2 << 16) | (1 << 18) | \
     (1 << 19) | (1 << 21) | (1 << 22) | (1 << 24) | (3ull << 32))

/*
 * rk3588 SDHCI subclass: the sdclk-enable bits of CLOCK_CONTROL
 * (SDHC_CLOCK 0x2c) are non-functional on Rockchip's implementation —
 * the card clock comes from the external CCLK_EMMC and the driver
 * (dwcmshc_rk3568_set_clock) never sets SDHCI_CLOCK_CARD_EN, only
 * SDHCI_CLOCK_INT_EN.  QEMU's generic SDHCI refuses to issue commands
 * while the card clock bit is clear (sdhci_can_issue_command), so the
 * write hook forces the bit on CLOCK_CONTROL writes before delegating
 * to the stock handler.
 */
#define TYPE_RK3588_SDHCI "rk3588-sdhci"

static void (*rk3588_sdhci_orig_write)(void *opaque, hwaddr off,
                                        uint64_t val, unsigned size);
static MemoryRegionOps rk3588_sdhci_ops;
static bool rk3588_sdhci_ops_ready;

static void rk3588_sdhci_write(void *opaque, hwaddr off,
                                        uint64_t val, unsigned size)
{
    if (off == 0x2c) {          /* SDHC_CLOCK: bit 2 = SDCLK_EN */
        val |= 1 << 2;
    }
    rk3588_sdhci_orig_write(opaque, off, val, size);
}

static void rk3588_sdhci_init(Object *obj)
{
    SDHCIState *s = SYSBUS_SDHCI(obj);

    if (!rk3588_sdhci_ops_ready) {
        rk3588_sdhci_ops = *s->io_ops;
        rk3588_sdhci_orig_write = s->io_ops->write;
        rk3588_sdhci_ops.write = rk3588_sdhci_write;
        rk3588_sdhci_ops_ready = true;
    }
    s->io_ops = &rk3588_sdhci_ops;
}

static const TypeInfo rk3588_sdhci_types[] = {
    {
        .name = TYPE_RK3588_SDHCI,
        .parent = TYPE_SYSBUS_SDHCI,
        .instance_init = rk3588_sdhci_init,
    },
};
DEFINE_TYPES(rk3588_sdhci_types)

/* Vendor area pointer window at +0xe8: dword 0 = 0x05400500, i.e.
 * P_VENDOR_AREA1 (0xe8) = 0x500 and P_VENDOR_AREA2 (0xea) = 0x540. */
static uint64_t rk3588_emmc_vptr_read(void *opaque, hwaddr off, unsigned size)
{
    /* 0xe8 dword = 0x05400500 covers both halfword reads (0xe8 -> 0x0500,
     * 0xea -> 0x0540); 0xec-0xef reads 0. */
    if (off < 4) {
        return 0x05400500 >> (off * 8) & ((1ull << (size * 8)) - 1);
    }
    return 0;
}

static void rk3588_emmc_vptr_write(void *opaque, hwaddr off, uint64_t val,
                                  unsigned size)
{
    /* vendor area pointers are read-only in hardware */
}

static const MemoryRegionOps rk3588_emmc_vptr_ops = {
    .read = rk3588_emmc_vptr_read,
    .write = rk3588_emmc_vptr_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * DLL_STATUS0 (+0x840): bit 8 = DLL locked, bit 9 = timeout.
 * dwcmshc_rk35xx_set_clock polls LOCKED (and treats TIMEOUT as failure)
 * for 500 ms after programming the DLL above 52 MHz; answer "locked"
 * immediately so the HS200/HS400 clock switch succeeds.
 */
static uint64_t rk3588_emmc_dll_status_read(void *opaque, hwaddr off,
                                           unsigned size)
{
    return 0x100 >> (off * 8) & ((1ull << (size * 8)) - 1);
}

static void rk3588_emmc_dll_status_write(void *opaque, hwaddr off,
                                        uint64_t val, unsigned size)
{
    /* DLL_STATUS0 is a read-only status register */
}

static const MemoryRegionOps rk3588_emmc_dll_status_ops = {
    .read = rk3588_emmc_dll_status_read,
    .write = rk3588_emmc_dll_status_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*****************************************************************************
 * CQHCI engine (vendor area 2, +0x540).  Register offsets are relative to
 * the CQHCI window, i.e. Linux's CQHCI_* (drivers/mmc/host/cqhci.h).
 *****************************************************************************/

#define RK_CQ_VER      0x00    /* RO: 5.10 */
#define RK_CQ_CAP      0x04    /* RO: no crypto support */
#define RK_CQ_CFG      0x08
#define RK_CQ_CTL      0x0c
#define RK_CQ_IS       0x10
#define RK_CQ_ISTE     0x14
#define RK_CQ_ISGE     0x18
#define RK_CQ_IC       0x1c
#define RK_CQ_TDLBA    0x20
#define RK_CQ_TDLBAU   0x24
#define RK_CQ_TDBR     0x28    /* doorbell: write = execute tasks */
#define RK_CQ_TCN      0x2c    /* RW1C: completed task notification */
#define RK_CQ_DQS      0x30
#define RK_CQ_DPT      0x34
#define RK_CQ_TCLR     0x38
#define RK_CQ_TDPE     0x3c
#define RK_CQ_SSC1     0x40
#define RK_CQ_SSC2     0x44
#define RK_CQ_CRDCT    0x48
#define RK_CQ_RMEM     0x50
#define RK_CQ_TERRI    0x54
#define RK_CQ_CRI      0x58
#define RK_CQ_CRA      0x5c

#define RK_CQ_IS_TCC   (1 << 1)
#define RK_CQ_CFG_ENABLE    (1 << 0)
#define RK_CQ_CTL_HALT      (1 << 0)
#define RK_CQ_SLOT_SZ       16      /* 8-byte task + 8-byte link desc */
#define RK_CQ_DCMD_SLOT     31
#define RK_CQ_MAX_TRAN_DESCS 32

/* sdhci Normal Interrupt Status bit 14: the CQE completion interrupt */
#define RK_SDHCI_INT_CQE    0x4000

typedef struct Rk3588Cqhci {
    SDHCIState *host;      /* inner generic-sdhci (sd-bus + IRQ line) */
    uint32_t cfg, ctl, is, iste, isge, tdlba, tdlbau, tcn, ssc1, ssc2;
    int lba_shift;         /* card LBA width: 9 = byte addr, 0 = sector */
} Rk3588Cqhci;

static uint32_t rk_cq_ld32(Rk3588Cqhci *cq, uint64_t addr)
{
    uint8_t b[4];

    address_space_read(&address_space_memory, addr,
                       MEMTXATTRS_UNSPECIFIED, b, sizeof(b));
    return ldl_le_p(b);
}

static uint64_t rk_cq_ld64(Rk3588Cqhci *cq, uint64_t addr)
{
    uint8_t b[8];

    address_space_read(&address_space_memory, addr,
                       MEMTXATTRS_UNSPECIFIED, b, sizeof(b));
    return ldq_le_p(b);
}

/*
 * The eMMC card may be byte-addressed (QEMU's small eMMC images: size
 * <= 2GiB, EXT_CSD SEC_CNT <= 2GiB/512 => the kernel multiplies LBAs
 * itself and the card expects byte addresses) or sector-addressed
 * (bigger images: LBA goes to the card as-is).  Probe EXT_CSD SEC_CNT
 * (offset 212, 512-byte data) once and remember the shift.
 */
static int rk_cq_lba_shift(Rk3588Cqhci *cq)
{
    SDBus *bus = &cq->host->sdbus;
    SDRequest req;
    uint8_t resp[16];
    uint8_t ext_csd[512];
    uint32_t sectors;

    if (cq->lba_shift >= 0) {
        return cq->lba_shift;
    }

    req.cmd = 8;                        /* SEND_EXT_CSD */
    req.arg = 0;
    req.crc = 0;
    sdbus_do_command(bus, &req, resp, sizeof(resp));
    sdbus_read_data(bus, ext_csd, sizeof(ext_csd));

    sectors = ldl_le_p(&ext_csd[212]);
    cq->lba_shift = sectors > (2u * GiB / 512) ? 0 : 9;
    return cq->lba_shift;
}

/*
 * Execute one tagged task from the task list at TDLBA and return the
 * completion bit for its tag.  8-byte task descriptor (dma64 == false):
 *   data task:  [63:32] block addr, [31:16] block count, [12] read dir,
 *               [3:5] ACT 0x5, [2] INT, [1] END, [0] VALID
 *   dcmd (slot 31): [21:16] command index, [23:24] resp type, [4..12)
 *               (at +4) 64-bit command argument
 * followed by an 8-byte link descriptor whose [+4] dword points at the
 * per-tag transfer descriptor table (8-byte entries: [16:31] length,
 * bit [1] END, [+4] dword data address).
 */
static uint32_t rk_cq_exec_task(Rk3588Cqhci *cq, unsigned tag)
{
    SDBus *bus = &cq->host->sdbus;
    SDRequest req;
    uint8_t resp[16];
    uint64_t desc_base = (uint64_t)cq->tdlbau << 32 | cq->tdlba;
    uint64_t desc = desc_base + tag * RK_CQ_SLOT_SZ;
    uint64_t attr = rk_cq_ld64(cq, desc);

    if (!(attr & 1)) {
        return 0;
    }

    if (tag == RK_CQ_DCMD_SLOT) {
        req.cmd = (attr >> 16) & 0x3f;
        req.arg = rk_cq_ld64(cq, desc + 4);
        req.crc = 0;
        sdbus_do_command(bus, &req, resp, sizeof(resp));
        return 1u << tag;
    }

    unsigned blocks = (attr >> 16) & 0xffff;
    uint64_t blk_addr = attr >> 32;
    bool read = (attr >> 12) & 1;
    uint64_t tran = rk_cq_ld32(cq, desc + 8 + 4);
    uint64_t card_addr = blk_addr << rk_cq_lba_shift(cq);

    req.crc = 0;
    req.arg = blocks;
    req.cmd = 23;                         /* SET_BLOCK_COUNT */
    sdbus_do_command(bus, &req, resp, sizeof(resp));
    if (getenv("RK3588_CQ_DEBUG")) {
        fprintf(stderr, "CQ task tag=%u %s blocks=%u addr=%#llx card_addr=%#llx "
                "tran=%#llx cmd23 resp %08x\n",
                tag, read ? "read" : "write", blocks,
                (unsigned long long)blk_addr, (unsigned long long)card_addr,
                (unsigned long long)tran, ldl_be_p(resp));
    }
    req.arg = card_addr;
    req.cmd = read ? 18 : 25;             /* READ/WRITE_MULTIPLE_BLOCK */
    sdbus_do_command(bus, &req, resp, sizeof(resp));
    if (getenv("RK3588_CQ_DEBUG")) {
        fprintf(stderr, "CQ task tag=%u cmd%u(%#llx) resp %08x\n",
                tag, req.cmd, (unsigned long long)req.arg,
                ldl_be_p(resp));
    }

    for (unsigned i = 0; i < RK_CQ_MAX_TRAN_DESCS; i++) {
        uint32_t tr_attr = rk_cq_ld32(cq, tran + i * 8);
        uint32_t len = tr_attr >> 16;
        uint64_t addr = rk_cq_ld32(cq, tran + i * 8 + 4);
        void *data = g_malloc(len ? len : 1);

        if (read) {
            sdbus_read_data(bus, data, len);
            if (getenv("RK3588_CQ_DEBUG")) {
                fprintf(stderr, "CQ read tag=%u len=%u -> %#llx: "
                        "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                        tag, len, (unsigned long long)addr,
                        ((uint8_t *)data)[0], ((uint8_t *)data)[1],
                        ((uint8_t *)data)[2], ((uint8_t *)data)[3],
                        ((uint8_t *)data)[4], ((uint8_t *)data)[5],
                        ((uint8_t *)data)[6], ((uint8_t *)data)[7]);
            }
            address_space_write(&address_space_memory, addr,
                                MEMTXATTRS_UNSPECIFIED, data, len);
        } else {
            address_space_read(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, data, len);
            sdbus_write_data(bus, data, len);
        }
        g_free(data);

        if (tr_attr & 2) {                /* END */
            break;
        }
    }

    req.cmd = 12;                         /* STOP_TRANSMISSION */
    req.arg = 0;
    sdbus_do_command(bus, &req, resp, sizeof(resp));

    return 1u << tag;
}

/*
 * Raise the CQE completion interrupt: OR the CQE bit into the inner
 * sdhci's INT_STATUS and recompute the IRQ line level exactly like
 * sdhci_update_irq() (the bit itself is unknown to the generic model,
 * but the line is a plain OR of status & signal-enable).
 */
static void rk_cq_complete(Rk3588Cqhci *cq, uint32_t done_mask)
{
    SDHCIState *sh = cq->host;

    cq->is |= RK_CQ_IS_TCC;
    cq->tcn |= done_mask;

    sh->norintsts |= RK_SDHCI_INT_CQE;
    qemu_set_irq(sh->irq,
                 (sh->norintsts & sh->norintsigen) ||
                 (sh->errintsts & sh->errintsigen));
}

static uint64_t rk_cq_read(void *opaque, hwaddr off, unsigned size)
{
    Rk3588Cqhci *cq = opaque;
    uint32_t val = 0;

    switch (off) {
    case RK_CQ_VER:
        val = 0x0510;             /* CQHCI version 5.10 */
        break;
    case RK_CQ_CFG:
        val = cq->cfg;
        break;
    case RK_CQ_CTL:
        val = cq->ctl;
        break;
    case RK_CQ_IS:
        val = cq->is;
        break;
    case RK_CQ_ISTE:
        val = cq->iste;
        break;
    case RK_CQ_ISGE:
        val = cq->isge;
        break;
    case RK_CQ_TDLBA:
        val = cq->tdlba;
        break;
    case RK_CQ_TDLBAU:
        val = cq->tdlbau;
        break;
    case RK_CQ_TCN:
        val = cq->tcn;
        break;
    case RK_CQ_SSC1:
        val = cq->ssc1;
        break;
    case RK_CQ_SSC2:
        val = cq->ssc2;
        break;
    default:
        /* CAP, IC, DQS, DPT, TCLR, TDPE, CRDCT, RMEM, TERRI, CRI, CRA */
        break;
    }

    return val >> (off & 3) * 8 & ((1ull << (size * 8)) - 1);
}

static void rk_cq_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Rk3588Cqhci *cq = opaque;
    uint32_t done_mask = 0;

    switch (off) {
    case RK_CQ_CFG:
        cq->cfg = val;
        break;
    case RK_CQ_CTL:
        cq->ctl = val;
        break;
    case RK_CQ_IS:
        cq->is &= ~val;            /* W1C */
        break;
    case RK_CQ_ISTE:
        cq->iste = val;
        break;
    case RK_CQ_ISGE:
        cq->isge = val;
        break;
    case RK_CQ_TDLBA:
        cq->tdlba = val;
        break;
    case RK_CQ_TDLBAU:
        cq->tdlbau = val;
        break;
    case RK_CQ_TDBR:
        /* Doorbell: execute each requested task synchronously. */
        for (unsigned tag = 0; tag < 32; tag++) {
            if (val & (1u << tag)) {
                done_mask |= rk_cq_exec_task(cq, tag);
            }
        }
        if (done_mask) {
            rk_cq_complete(cq, done_mask);
        }
        break;
    case RK_CQ_TCN:
        cq->tcn &= ~val;           /* W1C */
        break;
    case RK_CQ_SSC1:
        cq->ssc1 = val;
        break;
    case RK_CQ_SSC2:
        cq->ssc2 = val;
        break;
    default:
        /* CAP, IC, DQS, DPT, TCLR, TDPE, CRDCT, RMEM, TERRI, CRI, CRA */
        break;
    }
}

static const MemoryRegionOps rk3588_cq_ops = {
    .read = rk_cq_read,
    .write = rk_cq_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void rk3588_create_emmc(DeviceState *gic)
{
    DeviceState *host = qdev_new(TYPE_RK3588_SDHCI);
    SysBusDevice *sbd = SYS_BUS_DEVICE(host);
    DriveInfo *di;
    MemoryRegion *mr;

    /*
     * Vendor storage first, page-aligned (TCG requires page-aligned
     * leaf regions): one RAM page under the SDHCI register block and
     * a RAM tail for the rest of the 0x10000 DT window.  The inner
     * SDHCI (0x100 bytes, SDHC_REGISTERS_MAP_SIZE) is mapped after and
     * so wins the first 0x100 bytes over the page's RAM.
     */
    mr = g_new(MemoryRegion, 1);
    memory_region_init_ram(mr, OBJECT(gic), "rk3588-emmc-page0",
                           0x1000, &error_fatal);
    memory_region_add_subregion(get_system_memory(), RK3588_EMMC_BASE, mr);
    mr = g_new(MemoryRegion, 1);
    memory_region_init_ram(mr, OBJECT(gic), "rk3588-emmc-vendor",
                           RK3588_EMMC_SIZE - 0x1000, &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                RK3588_EMMC_BASE + 0x1000, mr);

    qdev_prop_set_uint64(host, "capareg", RK3588_EMMC_CAPAREG);
    qdev_prop_set_uint8(host, "sd-spec-version", 3);
    qdev_prop_set_uint8(host, "uhs", UHS_I);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, RK3588_EMMC_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(gic, RK3588_EMMC_SPI));

    /* Vendor area pointer overlay (priority over the inner window). */
    mr = g_new(MemoryRegion, 1);
    memory_region_init_io(mr, OBJECT(gic), &rk3588_emmc_vptr_ops, NULL,
                          "rk3588-emmc-vendor-ptr", 8);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        RK3588_EMMC_BASE + 0xe8, mr, 1);

    /* DLL_STATUS0 overlay: answer "DLL locked" for the HS200/HS400 poll. */
    mr = g_new(MemoryRegion, 1);
    memory_region_init_io(mr, OBJECT(gic), &rk3588_emmc_dll_status_ops, NULL,
                          "rk3588-emmc-dll-status", 4);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        RK3588_EMMC_DLL_STATUS0, mr, 1);

    /* CQHCI engine (vendor area 2, overlaps EMMC_ATCTRL at +0x540). */
    {
        Rk3588Cqhci *cq = g_new0(Rk3588Cqhci, 1);

        cq->host = SYSBUS_SDHCI(host);
        cq->lba_shift = -1;             /* probed on first doorbell */
        mr = g_new(MemoryRegion, 1);
        memory_region_init_io(mr, OBJECT(gic), &rk3588_cq_ops, cq,
                              "rk3588-emmc-cqhci", RK3588_EMMC_CQ_SIZE);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            RK3588_EMMC_CQ_BASE, mr, 1);
    }

    /* eMMC card from -drive if=sd unit 1. */
    di = drive_get(IF_SD, 0, 1);
    if (di) {
        DeviceState *card = qdev_new(TYPE_EMMC);
        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di),
                                &error_fatal);
        qdev_realize_and_unref(card, qdev_get_child_bus(host, "sd-bus"),
                               &error_fatal);
    }
}
