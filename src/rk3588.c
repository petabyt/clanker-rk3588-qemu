/*
 * Rockchip RK3588 machine model.
 *
 * P1: stock arm-gicv3 at GIC600 addresses, DW-APB UARTs, generic
 * timer PPI wiring, and -bios loaded at 0x0 with the CPU resetting in
 * EL3 at pc=0 — the same handoff the real bootrom performs (it loads
 * the image at 0 and branches there; secondaries park in EL3 WFE).
 * Later milestones add SoC stubs; see plan.md.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qobject/qlist.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/datadir.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/intc/arm_gic_common.h"
#include "hw/intc/arm_gicv3_common.h"
#include "system/address-spaces.h"
#include "system/physmem.h"
#include "system/reset.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
#include "dw-apb-uart.h"
#include "rk3588-dwmshc.h"
#include "rk3588-stubs.h"
#include "rk3588-display.h"
#include "rk3588-hdmi.h"
#include "rk3588-vop2.h"
#include "rk3588-sdhci.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"
#include "system/block-backend-io.h"

/*
 * Physical layout (from rk/src/rk3588/rk3588.h): the device MMIO window
 * starts at 0xF0000000, so RAM fills [0, 0xF0000000) and any remainder
 * continues above 4G, leaving [0xF0000000, 4G) free for devices.
 */
#define RK3588_RAM_BELOW_DEVICES    0xF0000000
#define RK3588_RAM_ABOVE_4G         0x100000000ULL

/* Two clusters of four cores (0x000-0x003 and 0x100-0x103). */
#define RK3588_CLUSTER_SIZE         4
#define RK3588_NR_CPUS              8

/* Guest firmware derives µs delays from CNTFRQ_EL0 (rk asm_set_cnt_freq). */
#define RK3588_CNTFRQ_HZ            24000000

/* MMIO bases (rk/src/rk3588/rk3588.h). */
#define RK3588_GICD_BASE            0xFE600000ULL
#define RK3588_GICR_BASE            0xFE680000ULL
#define RK3588_UART0_BASE           0xFD890000ULL
#define RK3588_UART2_BASE           0xFEB50000ULL

/*
 * P8 bootrom-lite: the makeboot --v2 (Rockchip SD) header layout and the
 * bootrom's staging contract (arm-docs/rk3588_bootrom.c): the DDR init
 * blob is called at 0xFF001000 with (x0=0xff000078, x1=8), then the main
 * image is called at 0 with the same arguments.  SRAM at 0xFF000000 holds
 * the staging blob; its stack grows down from the top of SRAM.
 */
#define RK3588_V2_MAGIC             0x534e4b52u /* "RKNS" */
#define RK3588_V2_HEADER_OFF        0x8000
#define RK3588_SRAM_BASE            0xFF000000ULL
/* TRM Chapter_1_System_Overview: SYSTEM_SRAM(1MB) at 0xFF000000. Was 0x80000
   (512 KB); the DDR blob reads a table at 0xFF080008 (inside the 1 MB). */
#define RK3588_SRAM_SIZE            0x100000
#define RK3588_SRAM1_BASE           0xFF100000ULL
#define RK3588_SRAM1_SIZE           0x10000
#define RK3588_DDR_STAGE            (RK3588_SRAM_BASE + 0x1000)
#define RK3588_BOOTDATA             (RK3588_SRAM_BASE + 0x78)

/*
 * Layout mirrors tools/main.h: RkHeaderV2 is packed, but its nested
 * ImageEntry is not (2 bytes of padding after `address`), giving entries
 * a stride of 88 bytes — image[1] therefore starts at header+0x58.
 */
struct RkV2ImageEntry {
    uint16_t offset;   /* in 512-byte sectors, relative to the header */
    uint16_t size;     /* in 512-byte sectors */
    uint16_t address;
    uint32_t flag;
    uint32_t counter;
    uint8_t res[8];
    uint8_t hash[0x40];
};

struct RkV2Header {
    uint32_t signature;
    uint32_t res1;
    uint16_t hash_field_offset;
    uint16_t n_images;
    uint32_t boot_flag;
    uint8_t res2[0x68];
    struct RkV2ImageEntry images[4];
} __attribute__((packed));

/*
 * ATF (BL31) boot path: -bios carries a TF-A ELF (entry 0x40000 for
 * rk3588), BL33 (U-Boot by default, EDK2 with -machine bl33-base=...)
 * is staged separately — typically
 *   -device loader,file=u-boot.bin,addr=0x800000,force-raw=on
 * and an optional -dtb is picked up by arm_load_kernel() at address 0
 * (the firmware-boot path hands the blob to "the bootloader" that way).
 *
 * The parameter block mirrors u-boot's common/spl/spl_atf.c builder
 * (bl2_plat_get_bl31_params_default): a deprecated-but-supported v1
 * bl31_params — TF-A's bl31_params_parse_helper() accepts PARAM_VERSION_1
 * when ERROR_DEPRECATED=0 (TF-A default) and copies the BL32/BL33
 * entry_point_info structs we point it at.  Struct layouts come from
 * TF-A include/export/common/{param_header,ep_info,bl_common}_exp.h.
 *
 * Low-RAM layout of the ATf-mode boot (see also P7, plan.md — the EDK2
 * firmware FIT loads its FV at 0x200000, so nothing the firmware path
 * uses may live in [0x200000, 0x700000)):
 *   [0x40000,  0xFA000)  TF-A BL31 (incl. its 512KB fdt_buffer .bss)
 *   [0x100000, ~0x168000) -dtb ROM copy (load_device_tree expands to
 *                          (size+10000)*2 ≈ 421KB; re-copied on reset)
 *   [0x190000, 0x190160) v1 bl31_params block
 *   [0x1FE000, 0x200000) ATAGS page (RkAtagsLib — SPL writes it on hw)
 *   [0x200000, 0x700000) EDK2 firmware FV (staged via -device loader)
 *   [0x7C0000, 0x7F0000) EDK2 NV variable store (RAM-backed here)
 *   [0x800000, ...)      U-Boot CONFIG_TEXT_BASE (default BL33)
 */
#define RK3588_ATF_PARAMS        0x00190000ULL  /* param block, below ATAGS */
#define RK3588_ATF_FDT           0x00100000ULL  /* DTB, above BL31, below params */
/* Must stay inside TF-A's post-MMU TZRAM mapping (bl31_plat_arch_setup
 * maps BL31_START..BL31_END = [0x40000, 0xDA000)): exception entry forces
 * PSTATE.SP=1, so the vectors push frames onto SP_EL3.  0xFF000 is above
 * __BL31_END__ and unmapped (it faulted at 0xFEFF0), so use the top of
 * cpu7's .stacks slot — mapped RW, and never used since secondaries are
 * start-powered-off. */
#define RK3588_ATF_SP            0x00064340ULL  /* SP_EL3 until TF-A sets its own */
#define RK3588_BL33_BASE         0x00800000ULL  /* u-boot CONFIG_TEXT_BASE */
/* BL33 entry handed to TF-A in bl33_ep_info; overridable with
 * -machine bl33-base=<addr> (e.g. 0x200000 for the EDK2 firmware FV). */
static uint64_t rk3588_bl33_base = RK3588_BL33_BASE;
/* SD-card boot: the FIT's edk2 node loads at 0x200000 (see plan.md P10). */
#define RK3588_SD_BL33_BASE      0x00200000ULL
static bool rk3588_bl33_explicit;

struct RkParamHeader {
    uint8_t type;
    uint8_t version;
    uint16_t size;
    uint32_t attr;
} __attribute__((packed));

struct RkImageInfo {            /* image_info_t */
    struct RkParamHeader h;
    uint64_t image_base;
    uint32_t image_size;
    uint32_t image_max_size;
} __attribute__((packed));

struct RkEpInfo {               /* entry_point_info_t (AArch64) */
    struct RkParamHeader h;
    uint64_t pc;                /* ENTRY_POINT_INFO_PC_OFFSET == 8 */
    uint32_t spsr;
    uint32_t rsvd;
    uint64_t args[8];           /* aapcs64_params_t at offset 0x18 */
} __attribute__((packed));

struct RkBl31Params {           /* v1 (deprecated) bl31_params */
    struct RkParamHeader h;
    uint64_t bl31_image_info;
    uint64_t bl32_ep_info;
    uint64_t bl32_image_info;
    uint64_t bl33_ep_info;
    uint64_t bl33_image_info;
} __attribute__((packed));

#define RK_PARAM_EP             0x01
#define RK_PARAM_IMAGE_BINARY   0x02
#define RK_PARAM_BL31           0x03
#define RK_PARAM_VERSION_1      0x01
#define RK_EP_SECURE            0x00
#define RK_EP_NON_SECURE        0x01
/*
 * SPSR_64(el, MODE_SP_ELX, DISABLE_ALL_EXCEPTIONS) from TF-A arch.h:
 * MODE_RW_64=0<<4, MODE_EL1=1<<2 / MODE_EL2=2<<2, SP_ELX=1,
 * DAIF (4 bits) << SPSR_DAIF_SHIFT(6) = 0x3c0.
 */
#define RK_SPSR_EL1H            0x3c5
#define RK_SPSR_EL2H            0x3c9

/*
 * SPI count behind GICD_TYPER: INTIDs 32..511.  The stock GICv3 model
 * wants num-irq = SPIs + GIC_INTERNAL (32-aligned); 480 SPIs covers
 * every interrupt on the part with room for later wiring decisions.
 */
#define RK3588_SPI_COUNT            480

static struct arm_boot_info rk3588_bootinfo;

/* Set when -bios carries a makeboot --v2 image; drives the P8 reset path. */
static bool rk3588_bootrom_lite;
static hwaddr rk3588_v2_ddr_off, rk3588_v2_os_off;
static uint64_t rk3588_v2_ddr_len, rk3588_v2_os_len;

/* Set when -bios is a TF-A (BL31) ELF; drives the ATF reset path. */
static bool rk3588_atf;
static uint64_t rk3588_atf_entry;
static hwaddr rk3588_atf_fdt;   /* 0 = no -dtb was given */

/*
 * Bootrom-lite staging for a makeboot --v2 image: run the DDR init blob
 * first (from SRAM), then the main image at 0.  The reset handler below
 * wires the entry registers; x30=0 makes the blob's final `ret` fall
 * straight into the main image (the real bootrom instead checks the blob's
 * return value first — skipped here, QEMU DRAM needs no training result).
 *
 * The header is parsed from the -bios file at init time; the actual image
 * copy runs in the reset handler, because load_image_targphys() stages the
 * file through rom_add_blob_fixed() and the bytes only reach RAM when
 * rom_reset() runs (it is registered first, so it has already run by then).
 */
/*
 * P10 Phase 2: no -bios, so the bootrom-lite reads the idbloader (TPL + SPL)
 * from the SD card itself, the way the real bootrom reads sector 0x40.  The
 * card is read through its block backend (not the dw-mshc model): the
 * bootrom's own SD driver is not modelled, and the SPL still reads the FIT
 * through the controller.  Returns true if the card holds a v2 header.
 */
static uint8_t *rk3588_v2_ddr_buf, *rk3588_v2_os_buf;

static void rk3588_parse_v2_from_sd(void)
{
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;
    struct RkV2Header hdr;
    int64_t cap;
    uint64_t ddr_end, os_end;

    if (!blk || (cap = blk_getlength(blk)) < 0) {
        return;
    }
    if (cap < RK3588_V2_HEADER_OFF + (int64_t)sizeof(hdr) ||
        blk_pread(blk, RK3588_V2_HEADER_OFF, sizeof(hdr), &hdr, 0) < 0) {
        return;
    }
    if (hdr.signature != RK3588_V2_MAGIC || hdr.n_images != 2) {
        return; /* no idbloader on the card: plain boot */
    }

    rk3588_v2_ddr_off = RK3588_V2_HEADER_OFF + (hwaddr)hdr.images[0].offset * 512;
    rk3588_v2_os_off = RK3588_V2_HEADER_OFF + (hwaddr)hdr.images[1].offset * 512;
    rk3588_v2_ddr_len = (uint64_t)hdr.images[0].size * 512;
    rk3588_v2_os_len = (uint64_t)hdr.images[1].size * 512;
    ddr_end = rk3588_v2_ddr_off + rk3588_v2_ddr_len;
    os_end = rk3588_v2_os_off + rk3588_v2_os_len;

    if (rk3588_v2_ddr_len == 0 || rk3588_v2_os_len == 0 ||
        rk3588_v2_ddr_len > RK3588_SRAM_SIZE - 0x1000 ||
        os_end > cap || ddr_end > cap ||
        rk3588_v2_os_len > RK3588_RAM_BELOW_DEVICES) {
        error_report("rk3588: malformed idbloader on SD card");
        exit(1);
    }

    rk3588_v2_ddr_buf = g_malloc(rk3588_v2_ddr_len);
    rk3588_v2_os_buf = g_malloc(rk3588_v2_os_len);
    if (blk_pread(blk, rk3588_v2_ddr_off, rk3588_v2_ddr_len,
                  rk3588_v2_ddr_buf, 0) < 0 ||
        blk_pread(blk, rk3588_v2_os_off, rk3588_v2_os_len,
                  rk3588_v2_os_buf, 0) < 0) {
        error_report("rk3588: could not read idbloader from SD card");
        exit(1);
    }

    error_report("rk3588: v2 idbloader on SD: tpl@%lx+%lx spl@%lx+%lx",
                 (unsigned long)rk3588_v2_ddr_off,
                 (unsigned long)rk3588_v2_ddr_len,
                 (unsigned long)rk3588_v2_os_off,
                 (unsigned long)rk3588_v2_os_len);
    rk3588_bootrom_lite = true;
    if (!rk3588_bl33_explicit) {
        rk3588_bl33_base = RK3588_SD_BL33_BASE;
    }
}

static void rk3588_parse_v2_header(MachineState *ms)
{
    struct RkV2Header hdr;
    char *fn;
    char *data = NULL;
    gsize len = 0;

    if (!ms->firmware) {
        rk3588_parse_v2_from_sd();
        return;
    }
    fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, ms->firmware);
    if (!fn || !g_file_get_contents(fn, &data, &len, NULL)) {
        g_free(fn);
        return;
    }
    g_free(fn);

    if (len < RK3588_V2_HEADER_OFF + sizeof(hdr)) {
        g_free(data);
        return;
    }
    memcpy(&hdr, data + RK3588_V2_HEADER_OFF, sizeof(hdr));
    if (hdr.signature != RK3588_V2_MAGIC || hdr.n_images != 2) {
        g_free(data); /* raw image (P0-P4 flow): nothing to stage */
        return;
    }

    rk3588_v2_ddr_off = RK3588_V2_HEADER_OFF + (hwaddr)hdr.images[0].offset * 512;
    rk3588_v2_os_off = RK3588_V2_HEADER_OFF + (hwaddr)hdr.images[1].offset * 512;
    rk3588_v2_ddr_len = (uint64_t)hdr.images[0].size * 512;
    rk3588_v2_os_len = (uint64_t)hdr.images[1].size * 512;

    if (rk3588_v2_ddr_len == 0 || rk3588_v2_os_len == 0 ||
        rk3588_v2_ddr_len > RK3588_SRAM_SIZE - 0x1000 ||
        rk3588_v2_os_off + rk3588_v2_os_len > len ||
        rk3588_v2_ddr_off + rk3588_v2_ddr_len > len) {
        error_report("rk3588: malformed makeboot v2 header");
        exit(1);
    }
    g_free(data);

    error_report("rk3588: v2 image, ddr@%lx+%lx os@%lx+%lx",
                 (unsigned long)rk3588_v2_ddr_off,
                 (unsigned long)rk3588_v2_ddr_len,
                 (unsigned long)rk3588_v2_os_off,
                 (unsigned long)rk3588_v2_os_len);
    rk3588_bootrom_lite = true;
}

/* Copies the two payloads into place (call at init, right after load+parse). */
static void rk3588_stage_v2_images(void)
{
    void *buf;

    if (rk3588_v2_ddr_buf) {
        /* Card source (Phase 2): payloads were already read into RAM buffers. */
        physical_memory_write(RK3588_DDR_STAGE, rk3588_v2_ddr_buf,
                              rk3588_v2_ddr_len);
        physical_memory_write(0, rk3588_v2_os_buf, rk3588_v2_os_len);
        g_free(rk3588_v2_ddr_buf);
        g_free(rk3588_v2_os_buf);
        rk3588_v2_ddr_buf = rk3588_v2_os_buf = NULL;
        return;
    }

    buf = g_malloc(MAX(rk3588_v2_ddr_len, rk3588_v2_os_len));
    physical_memory_read(rk3588_v2_ddr_off, buf, rk3588_v2_ddr_len);
    physical_memory_write(RK3588_DDR_STAGE, buf, rk3588_v2_ddr_len);

    /* The bootrom "loads the main image to address 0 and jumps there". */
    physical_memory_read(rk3588_v2_os_off, buf, rk3588_v2_os_len);
    physical_memory_write(0, buf, rk3588_v2_os_len);
    g_free(buf);
}

/*
 * Minimal ELF64 loader for the ATF image: writes each PT_LOAD segment
 * (plus the zeroed .bss tail, memsz > filesz) straight into guest memory
 * and returns e_entry.  Host and target are both little-endian; only
 * ET_EXEC / EM_AARCH64 images are accepted.  Segment destinations must
 * land in RAM, the device-window SRAMs, or above 4G.
 */
static uint64_t rk3588_load_elf64(const char *what, const uint8_t *data,
                                   size_t len, uint64_t ram_size)
{
    uint64_t entry, phoff, ram_hi;
    uint16_t phentsize, phnum, machine;
    size_t i;

    if (len < 64 || memcmp(data, "\177ELF", 4) || data[4] != 2 ||
        data[5] != 1 /* little-endian */) {
        error_report("rk3588: %s: not a little-endian ELF64 image", what);
        exit(1);
    }
    memcpy(&machine, data + 0x12, 2);
    memcpy(&entry, data + 0x18, 8);
    memcpy(&phoff, data + 0x20, 8);
    memcpy(&phentsize, data + 0x36, 2);
    memcpy(&phnum, data + 0x38, 2);
    if (machine != 183 /* EM_AARCH64 */ || entry == 0 || phentsize < 56 ||
        phoff + (uint64_t)phnum * phentsize > len) {
        error_report("rk3588: %s: bad ELF64 program headers", what);
        exit(1);
    }
    ram_hi = ram_size - MIN(ram_size, (uint64_t)RK3588_RAM_BELOW_DEVICES);

    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = data + phoff + i * phentsize;
        uint32_t ptype;
        uint64_t poff, paddr, filesz, memsz;
        bool ok;

        memcpy(&ptype, ph + 0, 4);
        if (ptype != 1 /* PT_LOAD */) {
            continue;
        }
        memcpy(&poff, ph + 8, 8);
        memcpy(&paddr, ph + 24, 8);
        memcpy(&filesz, ph + 32, 8);
        memcpy(&memsz, ph + 40, 8);
        if (poff + filesz > len || memsz < filesz || paddr + memsz < paddr) {
            error_report("rk3588: %s: malformed PT_LOAD segment", what);
            exit(1);
        }
        if (memsz == 0) {
            continue;
        }
        ok = paddr + memsz <= RK3588_RAM_BELOW_DEVICES ||
             (paddr >= RK3588_RAM_ABOVE_4G &&
              paddr + memsz <= RK3588_RAM_ABOVE_4G + ram_hi) ||
             (paddr >= RK3588_SRAM_BASE &&
              paddr + memsz <= RK3588_SRAM_BASE + RK3588_SRAM_SIZE) ||
             (paddr >= RK3588_SRAM1_BASE &&
              paddr + memsz <= RK3588_SRAM1_BASE + RK3588_SRAM1_SIZE);
        if (!ok) {
            error_report("rk3588: %s: PT_LOAD at 0x%lx+%lx outside guest memory",
                         what, (unsigned long)paddr, (unsigned long)memsz);
            exit(1);
        }
        physical_memory_write(paddr, data + poff, filesz);
        if (memsz > filesz) {
            void *z = g_malloc0(memsz - filesz);

            physical_memory_write(paddr + filesz, z, memsz - filesz);
            g_free(z);
        }
    }
    error_report("rk3588: ATF ELF entry 0x%lx", (unsigned long)entry);
    return entry;
}

/*
 * Build the TF-A v1 parameter block (see the RkBl31Params comment) in
 * RAM: params header + BL32 (absent, pc=0) + BL33 (U-Boot at EL2) +
 * image_info placeholders.  Written at init, before any reset handler
 * runs; nothing else touches the region.
 */
static void rk3588_build_atf_params(void)
{
    uint8_t buf[0x160];
    struct RkBl31Params *bp = (void *)buf;
    struct RkEpInfo *ep;
    struct RkImageInfo *ii;

    memset(buf, 0, sizeof(buf));

    bp->h.type = RK_PARAM_BL31;
    bp->h.version = RK_PARAM_VERSION_1;
    bp->h.size = sizeof(*bp);
    bp->bl31_image_info = RK3588_ATF_PARAMS + 0x100;
    bp->bl32_ep_info = RK3588_ATF_PARAMS + 0x40;
    bp->bl32_image_info = RK3588_ATF_PARAMS + 0x118;
    bp->bl33_ep_info = RK3588_ATF_PARAMS + 0x80;
    bp->bl33_image_info = RK3588_ATF_PARAMS + 0x130;

    ep = (void *)(buf + 0x40);
    ep->h.type = RK_PARAM_EP;
    ep->h.version = RK_PARAM_VERSION_1;
    ep->h.size = sizeof(*ep);
    ep->h.attr = RK_EP_SECURE;
    ep->spsr = RK_SPSR_EL1H;    /* pc stays 0 => no BL32 */

    ep = (void *)(buf + 0x80);
    ep->h.type = RK_PARAM_EP;
    ep->h.version = RK_PARAM_VERSION_1;
    ep->h.size = sizeof(*ep);
    ep->h.attr = RK_EP_NON_SECURE;
    ep->pc = rk3588_bl33_base;
    ep->spsr = RK_SPSR_EL2H;
    ep->args[0] = 0;            /* BL33 gets mpidr & 0xffff in x0 */

    ii = (void *)(buf + 0x100);
    for (int n = 0; n < 3; n++, ii = (void *)((uint8_t *)ii + sizeof(*ii))) {
        ii->h.type = RK_PARAM_IMAGE_BINARY;
        ii->h.version = RK_PARAM_VERSION_1;
        ii->h.size = sizeof(*ii);
    }

    physical_memory_write(RK3588_ATF_PARAMS, buf, sizeof(buf));
}

static void rk3588_bootrom_lite_reset(void *opaque)
{
    ARMCPU *cpu = ARM_CPU(first_cpu);
    CPUARMState *env;

    if (!rk3588_bootrom_lite) {
        return;
    }
    /*
     * Registered after arm_load_kernel(), so do_cpu_reset() (PC from
     * bootinfo.entry) has already run; override the PC with the DDR blob
     * entry and wire the bootrom call contract.  (The images were staged
     * at init time — see rk3588_load_firmware().)
     */
    env = &cpu->env;
    env->xregs[0] = RK3588_BOOTDATA;
    env->xregs[1] = 8;
    env->xregs[30] = 0; /* blob returns to the main image at 0 */
    env->xregs[31] = RK3588_SRAM_BASE + RK3588_SRAM_SIZE; /* SP_EL3 in SRAM */
    cpu_set_pc(CPU(cpu), RK3588_DDR_STAGE);
}

/*
 * ATF path: like the bootrom handler, runs after do_cpu_reset() and only
 * in ATF mode.  Wires the bl31_entrypoint() contract — x0 = bl31_params *,
 * x1 = FDT (our relocated copy at RK3588_ATF_FDT, see rk3588_init; 0 when
 * no -dtb was given is also safe: TF-A then hits BADMAGIC and skips
 * aux-param parsing) — plus a scratch SP: bl31_entrypoint() makes function
 * calls (inv_dcache_range/zeromem) from el3_entrypoint_common before
 * plat_set_my_stack() switches to BL31's own .stacks.
 */
static void rk3588_atf_reset(void *opaque)
{
    ARMCPU *cpu = ARM_CPU(first_cpu);
    CPUARMState *env;

    if (!rk3588_atf) {
        return;
    }
    env = &cpu->env;
    env->xregs[0] = RK3588_ATF_PARAMS;
    env->xregs[1] = rk3588_atf_fdt;
    env->xregs[2] = 0;
    env->xregs[3] = 0;
    env->xregs[31] = RK3588_ATF_SP; /* SP_EL3 until TF-A installs its own */
    cpu_set_pc(CPU(cpu), rk3588_atf_entry);
}

static DeviceState *rk3588_create_gic(MachineState *ms)
{
    DeviceState *gic = qdev_new(gicv3_class_name());
    QList *redist_regions = qlist_new();

    qdev_prop_set_uint32(gic, "revision", 3);
    qdev_prop_set_uint32(gic, "num-cpu", ms->smp.cpus);
    qdev_prop_set_uint32(gic, "num-irq", RK3588_SPI_COUNT + GIC_INTERNAL);
    qdev_prop_set_bit(gic, "has-security-extensions", true);

    qlist_append_int(redist_regions, MIN(ms->smp.cpus, RK3588_NR_CPUS));
    qdev_prop_set_array(gic, "redist-region-count", redist_regions);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 0, RK3588_GICD_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 1, RK3588_GICR_BASE);
    return gic;
}

/*
 * Timer/maintenance/PMU PPIs and the GIC's interrupt outputs, using the
 * same wiring as virt/sbsa-ref (bsa.h PPI numbers).
 */
static void rk3588_connect_gic(MachineState *ms, DeviceState *gic)
{
    unsigned int ncpus = ms->smp.cpus;
    const int timer_irq[] = {
        [GTIMER_PHYS]        = ARCH_TIMER_NS_EL1_IRQ,
        [GTIMER_VIRT]        = ARCH_TIMER_VIRT_IRQ,
        [GTIMER_HYP]         = ARCH_TIMER_NS_EL2_IRQ,
        [GTIMER_SEC]         = ARCH_TIMER_S_EL1_IRQ,
        [GTIMER_HYPVIRT]     = ARCH_TIMER_NS_EL2_VIRT_IRQ,
        [GTIMER_S_EL2_PHYS]  = ARCH_TIMER_S_EL2_IRQ,
        [GTIMER_S_EL2_VIRT]  = ARCH_TIMER_S_EL2_VIRT_IRQ,
    };
    int i, irq;

    for (i = 0; i < ncpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        int intidbase = RK3588_SPI_COUNT + i * GIC_INTERNAL;

        for (irq = 0; irq < ARRAY_SIZE(timer_irq); irq++) {
            qdev_connect_gpio_out(cpudev, irq,
                                  qdev_get_gpio_in(gic,
                                                   intidbase + timer_irq[irq]));
        }
        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(gic,
                                        intidbase + ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(gic,
                                        intidbase + VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(SYS_BUS_DEVICE(gic), i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + ncpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + 2 * ncpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + 3 * ncpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + 4 * ncpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_NMI));
        sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + 5 * ncpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VINMI));
    }
}

/*
 * UART0 and UART2 at their rk3588-base.dtsi bases, wired to their real
 * GIC SPIs (uart0=331, uart2=333).  The DW model's level IRQ only
 * reaches the CPU once the guest's GIC config enables the SPI, so the
 * polled-mode firmware guests (rk, U-Boot) are unaffected.  Linux's
 * 8250_dw refuses to probe without a DT interrupt, so the wiring is
 * what makes ttyS0 (and /dev/console) exist.
 */
static void rk3588_create_uarts(DeviceState *gic)
{
    static const struct {
        hwaddr base;
        int serial_index;
        int spi;
    } uarts[] = {
        { RK3588_UART0_BASE, 1, 331 },
        { RK3588_UART2_BASE, 0, 333 }, /* firmware console (plat_get_uart_base) */
    };
    int i;

    for (i = 0; i < ARRAY_SIZE(uarts); i++) {
        DeviceState *dev = qdev_new(TYPE_DW_APB_UART);
        SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
        Chardev *chr = serial_hd(uarts[i].serial_index);

        if (chr) {
            qdev_prop_set_chr(dev, "chardev", chr);
        }
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, uarts[i].base);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(gic, uarts[i].spi));
    }
}

/*
 * Display pipeline for the Linux DRM stack (P6 HDMI):
 *  - VOP2 at 0xfdd90000, frame-start IRQ on GIC SPI 156 (vop@fdd90000,
 *    rk3588-base.dtsi; shared with vop_mmu there, unshared in our DT);
 *  - HDMI TX (dw-hdmi-qp) at 0xfde80000 with its five real SPIs:
 *    avp=169, cec=170, earc=171, main=172, hpd=360.  Only "main" is
 *    behaviourally raised (I2CM/DDC completions); the HPD "level" the
 *    glue polls lives in sys_grf 0x384 (stub read hook, connected).
 */
static void rk3588_create_display_devices(DeviceState *gic)
{
    static const int hdmi_spis[] = { 169, 170, 171, 172, 360 };
    DeviceState *dev;
    SysBusDevice *sbd;
    int i;

    dev = qdev_new(TYPE_RK3588_VOP2);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0xfdd90000);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(gic, 156));

    dev = qdev_new(TYPE_RK3588_HDMI);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, 0xfde80000);
    for (i = 0; i < ARRAY_SIZE(hdmi_spis); i++) {
        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(gic, hdmi_spis[i]));
    }
}

/*
 * SD slot (sdmmc@fe2c0000).  -drive if=sd,file=...[,readonly=on] attaches an
 * SD card; without it the slot is empty and PA4 reads "no card" (P7 path).
 */
static void rk3588_create_sdmmc(void)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_DWMSHC);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;

    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, RK3588_SDMMC_BASE);

    if (blk) {
        DeviceState *card = qdev_new(TYPE_SD_CARD);

        qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
        qdev_realize_and_unref(card, qdev_get_child_bus(dev, "sd-bus"),
                               &error_fatal);
    }
}

/*
 * -bios: the bootrom's payload loader places the image at 0 and jumps to it.
 *
 * Deliberately not load_image_targphys(): that stages the file as a ROM
 * which only reaches RAM in rom_reset(), registered by
 * machine_run_board_init() *after* this machine's init — too late for our
 * bootrom-lite staging in the first reset handler.  Write the bytes
 * straight into RAM here instead.
 */
static void rk3588_load_firmware(MachineState *ms, uint64_t max_size)
{
    char *fn;
    char *data = NULL;
    gsize len = 0;

    if (!ms->firmware) {
        return;
    }
    fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, ms->firmware);
    if (!fn) {
        error_report("rk3588: could not find firmware image '%s'",
                     ms->firmware);
        exit(1);
    }
    if (!g_file_get_contents(fn, &data, &len, NULL)) {
        error_report("rk3588: could not load firmware image '%s'",
                     ms->firmware);
        exit(1);
    }
    g_free(fn);
    if (len > max_size) {
        error_report("rk3588: firmware image '%s' (%zu bytes) exceeds RAM",
                     ms->firmware, len);
        exit(1);
    }
    if (len >= 4 && memcmp(data, "\177ELF", 4) == 0) {
        /*
         * ATF (BL31) boot: load the ELF segments at their physical
         * addresses instead of staging raw bytes at 0.  U-Boot (BL33)
         * comes in separately via -device loader,addr=0x800000.
         */
        rk3588_atf_entry = rk3588_load_elf64(ms->firmware,
                                              (const uint8_t *)data, len,
                                              ms->ram_size);
        rk3588_atf = true;
        rk3588_atf_mode = true;
        g_free(data);
        return;
    }
    physical_memory_write(0, data, len);
    g_free(data);
}

static void rk3588_init(MachineState *ms)
{
    MemoryRegion *sysmem = get_system_memory();
    uint64_t low_size = MIN(ms->ram_size, (uint64_t)RK3588_RAM_BELOW_DEVICES);
    MemoryRegion *low_ram = g_new(MemoryRegion, 1);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    MemoryRegion *sram1 = g_new(MemoryRegion, 1);
    DeviceState *gic;
    int i;

    memory_region_init_alias(low_ram, OBJECT(ms), "rk3588.ram-low",
                             ms->ram, 0, low_size);
    memory_region_add_subregion(sysmem, 0, low_ram);

    if (ms->ram_size > low_size) {
        MemoryRegion *high_ram = g_new(MemoryRegion, 1);

        memory_region_init_alias(high_ram, OBJECT(ms), "rk3588.ram-high",
                                 ms->ram, low_size, ms->ram_size - low_size);
        memory_region_add_subregion(sysmem, RK3588_RAM_ABOVE_4G, high_ram);
    }

    /*
     * MODEL GUESS (plan.md, DQ test section): the DDR TPL's DQ write/read
     * test stores to channel bases ch<<33 (TRM interleave bases, channels
     * 1..3). Real 8 GB boards complete that test without a fault, which
     * suggests the out-of-range addresses wrap onto installed DRAM.
     * Modelled as low-RAM aliases at 8/16/24 GB, overlap priority -1 so
     * real RAM and other regions win if they ever overlap.
     */
    for (int ch = 1; ch < 4; ch++) {
        MemoryRegion *wrap = g_new(MemoryRegion, 1);

        memory_region_init_alias(wrap, OBJECT(ms), "rk3588.ram-chwrap",
                                 ms->ram, 0, low_size);
        memory_region_add_subregion_overlap(sysmem, (hwaddr)ch << 33,
                                            wrap, -1);
    }

    /* Bootrom staging SRAM: holds the P8 DDR blob and its stack. */
    memory_region_init_ram(sram, NULL, "rk3588.sram",
                           RK3588_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, RK3588_SRAM_BASE, sram);

    /*
     * SRAM1: the DDR blob's 8KB print/log ring lives here (cursor kept
     * biased by 0xF00000, so a fresh cursor of 0 addresses 0xFF100000).
     */
    memory_region_init_ram(sram1, NULL, "rk3588.sram1",
                           RK3588_SRAM1_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, RK3588_SRAM1_BASE, sram1);

    rk3588_load_firmware(ms, low_size);
    rk3588_parse_v2_header(ms);
    if (rk3588_bootrom_lite) {
        rk3588_stage_v2_images();
    }
    if (rk3588_atf) {
        rk3588_build_atf_params();
    }

    for (i = 0; i < ms->smp.cpus; i++) {
        Object *cpuobj = object_new(ms->cpu_type);
        /*
         * QEMU's default affinity packs all 8 cores into 0x000-0x007;
         * RK3588 has two clusters of four (A76 + A55).
         */
        uint64_t affinity = i < RK3588_CLUSTER_SIZE ?
                            i : 0x100 | (i - RK3588_CLUSTER_SIZE);

        object_property_set_int(cpuobj, "cntfrq", RK3588_CNTFRQ_HZ,
                                &error_abort);
        object_property_set_int(cpuobj, "mp-affinity", affinity, &error_abort);
        object_property_set_bool(cpuobj, "has_el3", true, &error_abort);
        if (i > 0) {
            /*
             * The bootrom parks secondary cores in an EL3 WFE loop at
             * 0xffff00e0 (see arm-docs/rk3588_bootrom.c); QEMU has no
             * bootrom, so keep them off until a later milestone brings
             * them up (PSCI / parking patch — plan.md).
             */
            object_property_set_bool(cpuobj, "start-powered-off", true,
                                     &error_abort);
        }
        qdev_realize(DEVICE(cpuobj), NULL, &error_abort);
    }

    gic = rk3588_create_gic(ms);
    rk3588_connect_gic(ms, gic);
    rk3588_create_uarts(gic);
    rk3588_create_display_devices(gic);
    rk3588_create_sdmmc();
    rk3588_create_emmc(gic);
    rk3588_create_stub_regions(get_system_memory());
    rk3588_display_init(ms);

    /*
     * Standard ARM boot bookkeeping: registers the reset handler and
     * implements -kernel/-dtb if the user passes them.  Without those
     * options we take the firmware path, which leaves the reset PC at 0
     * in EL3 where the -bios image sits.
     */
    rk3588_bootinfo.ram_size = ms->ram_size;
    rk3588_bootinfo.board_id = -1;
    rk3588_bootinfo.loader_start = 0;
    if (rk3588_atf) {
        rk3588_bootinfo.entry = rk3588_atf_entry;
        /*
         * arm_load_kernel()'s default DTB install goes to RAM base 0,
         * but load_device_tree() expands the blob to ~2x + slack
         * ((size+10000)*2 ≈ 420KB) — that would smash BL31 (.ro at
         * 0x40000, .data/.stacks at 0x5e000).  Install the DTB ourselves,
         * clear of BL31, the param block and the scratch stack, and hand
         * its address to TF-A in x1 (arm_load_dtb registers it as a ROM
         * image, so it is re-copied on every reset).
         */
        rk3588_bootinfo.skip_dtb_autoload = true;
    } else {
        rk3588_bootinfo.entry = rk3588_bootrom_lite ? RK3588_DDR_STAGE : 0;
    }
    rk3588_bootinfo.firmware_loaded = ms->firmware != NULL;
    arm_load_kernel(ARM_CPU(first_cpu), ms, &rk3588_bootinfo);
    if (rk3588_atf && ms->dtb) {
        int rc = arm_load_dtb(RK3588_ATF_FDT, &rk3588_bootinfo,
                              RK3588_ATF_PARAMS, &address_space_memory, ms,
                              ARM_CPU(first_cpu));

        if (rc <= 0) {
            error_report("rk3588: could not place -dtb at 0x%lx",
                         (unsigned long)RK3588_ATF_FDT);
            exit(1);
        }
        rk3588_atf_fdt = RK3588_ATF_FDT;
    }
    /*
     * P8: after do_cpu_reset, complete the bootrom call contract for the
     * DDR blob (registration order is FIFO — hw/core/resetcontainer.c).
     */
    qemu_register_reset(rk3588_bootrom_lite_reset, NULL);
    /* ATF: same deal — wire x0..x3/SP after do_cpu_reset. */
    qemu_register_reset(rk3588_atf_reset, NULL);
}

static void rk3588_machine_init(MachineClass *mc)
{
    mc->desc = "Rockchip RK3588";
    mc->init = rk3588_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a76");
    mc->min_cpus = 1;
    mc->default_cpus = RK3588_NR_CPUS;
    mc->max_cpus = RK3588_NR_CPUS;
    mc->default_ram_size = 4 * GiB;
    mc->default_ram_id = "rk3588.ram";
}

/*
 * Hand-rolled instead of DEFINE_MACHINE() so the class can expose the
 * bl33-base property; -machine key=val reaches it through
 * object_set_props_from_keyval() (vl.c qemu_apply_machine_options),
 * which runs before machine_run_board_init() calls mc->init.
 */
static void rk3588_get_bl33_base(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    uint64_t val = rk3588_bl33_base;

    visit_type_uint64(v, name, &val, errp);
}

static void rk3588_set_bl33_base(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    uint64_t val;

    if (visit_type_uint64(v, name, &val, errp)) {
        rk3588_bl33_base = val;
        rk3588_bl33_explicit = true;
    }
}

static void rk3588_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add(oc, "bl33-base", "uint64",
                              rk3588_get_bl33_base, rk3588_set_bl33_base,
                              NULL, NULL);
    object_class_property_set_description(oc, "bl33-base",
                                          "BL33 entry address passed to TF-A "
                                          "(0x800000 = U-Boot, "
                                          "0x200000 = EDK2 firmware FV)");
    rk3588_machine_init(mc);
}

static const TypeInfo rk3588_machine_type = {
    .name           = MACHINE_TYPE_NAME("rk3588"),
    .parent         = TYPE_MACHINE,
    .class_init     = rk3588_machine_class_init,
    .instance_size  = sizeof(MachineState),
};

static void rk3588_register_types(void)
{
    type_register_static(&rk3588_machine_type);
}
type_init(rk3588_register_types)
