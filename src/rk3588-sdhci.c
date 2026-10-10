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
 *    (CQE; our DT has no supports-cqe so it is never used).
 *  - DLL block 0x800-0x840: DWCMSHC_EMMC_DLL_{CTRL,RXCLK,TXCLK,STRBIN}
 *    0x800-0x80c, CMDOUT 0x810, MISC_CON 0x81c (MISC_INTCLK_EN bit 1),
 *    DLL_STATUS0 0x840 (bit 8 = locked, bit 9 = timeout).  All vendor
 *    registers are < 0x1000, i.e. inside the inner SDHCI's MMIO window;
 *    the driver treats them as fire-and-forget config except DLL_STATUS0,
 *    which is only polled above 52 MHz (HS200/HS400 - excluded here by
 *    not advertising SDR104/HS200 in capareg, so the <= 52 MHz branch of
 *    dwcmshc_rk3568_set_clock runs, which only writes the DLL regs).
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
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/sd/sdhci.h"
#include "hw/sd/sd.h"
#include "system/block-backend.h"
#include "system/address-spaces.h"

#include "rk3588-sdhci.h"

#define RK3588_EMMC_BASE    0xFE2E0000ULL
#define RK3588_EMMC_SIZE    0x10000
#define RK3588_EMMC_SPI     205

/*
 * Capabilities (SDHCI spec v3 field layout, hw/sd/sdhci-internal.h):
 * TOCLKFREQ 50 MHz / TOUNIT MHz, BASECLKFREQ 200 MHz, MAXBLOCKLENGTH 2
 * (1024-byte transfers), EMBEDDED_8BIT (8-bit bus), ADMA2, HIGHSPEED,
 * V33.  No SDR50/SDR104/UHS-II/64-bit: the card stays at <= 52 MHz
 * SDR, which keeps the driver out of the DLL-lock (>52 MHz) and
 * auto-tuning paths the model does not implement.  SLOT_TYPE must
 * stay 0 (removable): QEMU's sdhci_check_capareg() rejects embedded
 * slot types, and the guest mmc core relies on the DT "non-removable"
 * for card-detect semantics anyway.
 */
#define RK3588_EMMC_CAPAREG \
    ((uint64_t)50 | (1 << 7) | (200 << 8) | (2 << 16) | (1 << 18) | \
     (1 << 19) | (1 << 21) | (1 << 24))

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

static void rk3588_sdhci_write(void *opaque, hwaddr off, uint64_t val,
                               unsigned size)
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
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, RK3588_EMMC_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(gic, RK3588_EMMC_SPI));

    /* Vendor area pointer overlay (priority over the inner window). */
    mr = g_new(MemoryRegion, 1);
    memory_region_init_io(mr, OBJECT(gic), &rk3588_emmc_vptr_ops, NULL,
                          "rk3588-emmc-vendor-ptr", 8);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        RK3588_EMMC_BASE + 0xe8, mr, 1);

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
