/*
 * RK3588 peripheral register-file stubs for the ./rk firmware.
 *
 * Two register classes exist in the firmware's usage:
 *
 *  - hiword-mask ("write enable") blocks: CRU/PMU/GRF/SGRF/IOC/GPIO.
 *    Every firmware access goes through rk_clr_set_bits(), which stores
 *    (data << 0) | (mask << 16) in a single 32-bit write.  Hardware
 *    updates bits 15:0 only where bits 31:16 of the write are set; the
 *    mask half itself is never stored (reads return 0 there).  Register
 *    sequences that configure one field at a time rely on this.
 *
 *  - plain read/write blocks: VOP2/eDP/PWM/PHY combo.  Firmware does
 *    read-modify-write (|=/&=) on these, so masked semantics would turn
 *    every update into a no-op.
 *
 * A few status registers need read hooks so firmware polls terminate:
 *  - CRU  PLL CON6 (V0PLL +0x178, PPLL +0x218): bit15 = PLL locked
 *  - PMU  +0x4000: SoC version, must read 0x3588
 *  - eDP  sys_ctl_1 0x600 bit2 DET_STA, sys_ctl_3 0x608 bit6 HPD,
 *        dp_debug_ctl 0x6c0 bit4 PLL lock
 *  - OTP  0x84 INT_ST bit1 RD_DONE (U-Boot cpuid read)
 *  - TRNG 0xf0 VERSION = 0x46bc, 0x14 ISTAT bit0 RAND_RDY (U-Boot RNG)
 * Everything else reads back what was written (power-gate status reads
 * as 0 => "already on" fast path; CHA_STA / STRM_VALID read as 0 =>
 * success).
 *
 * Non-QOM by design: these are anonymous register files with no
 * behaviour, all machine wiring stays in rk3588.c.
 */

#include "qemu/osdep.h"
#include "qom/object.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "rk3588-stubs.h"
#include "rk3588-dwmshc.h"
#include "rk3588-crypto.h"
#include "rk3588-ddr.h"

typedef uint64_t (*RkStubReadHook)(uint32_t stored, hwaddr off);
typedef void (*RkStubWriteHook)(hwaddr off, uint64_t value);

/* Set by rk3588.c when -bios is a TF-A (BL31) ELF; see rk-stubs header. */
bool rk3588_atf_mode;

typedef struct RkStubResetVal {
    hwaddr off;
    uint32_t val;
} RkStubResetVal;

typedef struct RkStubBlock {
    const char *name;
    hwaddr base;
    hwaddr size;
    bool hiword;
    bool track_engine;
    RkStubReadHook read_hook;
    RkStubWriteHook write_hook;
    /* optional power-on value for one register (offset from base) */
    hwaddr reset_off;
    uint32_t reset_val;
    /* optional power-on value table (offsets from base); applied first */
    const struct RkStubResetVal *reset_vals;
    unsigned n_reset_vals;
    /* offsets [plain_lo, plain_hi) are plain 32-bit RW, not hiword-masked */
    hwaddr plain_lo, plain_hi;
} RkStubBlock;

typedef struct RkStub {
    uint32_t *regs;
    bool hiword;
    bool track_engine;
    bool engine_ready;
    uint8_t *tally;     /* per-64K-block doorbell toggle for +0x514 */
    unsigned tally_n;
    RkStubReadHook read_hook;
    RkStubWriteHook write_hook;
    hwaddr plain_lo, plain_hi;
} RkStub;

/*
 * Power-off request word: plat_shutdown() (rk/src/rk3588/io.c) writes
 * 'OFF!' (0x4f464621) to the last word of the PMU1GRF window before
 * falling into the reset path.  A real board cuts power through the
 * RK806 PMIC (no driver in the firmware yet), so this scratch write is
 * the QEMU-facing contract: latch a guest shutdown request.  Checked
 * before hiword-mask processing, matching the raw 32-bit store.
 */
static void rk_stub_poweroff_write(hwaddr off, uint64_t value)
{
    if (off == 0xffc && value == 0x4f464621) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}

/*
 * ATF (BL31) boot path, gated by rk3588_atf_mode.  Rockchip TF-A signals
 * power-off/reset through SoC glue, not the PSCI return path:
 *
 *  - rockchip_soc_system_off() (plat/rockchip/rk3588/drivers/pmu/pmu.c)
 *    flips pmic_sleep (GPIO0_A2) to gpio output-high:
 *      mmio_write_32(GPIO0_BASE + GPIO_SWPORT_DR_L,
 *                     BITS_WITH_WMASK(1, 0x1, 2));   -- equals 0x00040004
 *    then parks in WFI.  GPIO_SWPORT_DR_L is offset 0x00; the value is
 *    the raw 32-bit store (data bits 15:0, write-enables 31:16) before
 *    hiword-mask processing in rk_stub_write().
 *
 *  - rockchip_soc_soft_reset() triggers the first global reset with
 *      mmio_write_32(CRU_BASE + CRU_GLB_SRST_FST, GLB_SRST_FST_CFG_VAL);
 *      -- CRU offset 0xc08, value 0xfdb9 --  then parks in WFI.
 */
static void rk_stub_atf_poweroff_write(hwaddr off, uint64_t value)
{
    if (rk3588_atf_mode && off == 0x00 && value == 0x00040004) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}

static void rk_stub_atf_reset_write(hwaddr off, uint64_t value)
{
    if (rk3588_atf_mode && off == 0xc08 && value == 0xfdb9) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

/*
 * Main CRU window (0xfd7c0000): the Linux clk-rk3588 driver polls these
 * exact PLLCON(6) offsets for lock (bit 15), and its registration-time
 * recalc_rate() READS every CLKSEL_CON (0x300..0x5cc) — several of which
 * are congruent 0x18/0x38 mod 0x40, so the generic rule below must NOT
 * apply here (it would corrupt divider reads with bit 15).
 */
static uint64_t rk_stub_cru_main_read(uint32_t stored, hwaddr off)
{
    switch (off) {
    case 0x178:  /* V0PLL PLLCON(6) */
    case 0x198:  /* AUPLL */
    case 0x1b8:  /* CPLL */
    case 0x1d8:  /* GPLL */
    case 0x1f8:  /* NPLL */
        stored |= 1u << 15;    /* PLL locked */
        break;
    }
    return stored;
}

static uint64_t rk_stub_cru_read(uint32_t stored, hwaddr off)
{
    /*
     * PLL lock status: every PLL's PLLCON(6) sits at con_offset + 0x18
     * inside its CRU window, so lock offsets always end in 0x18 or 0x38.
     * (V0PLL 0x178, AUPLL 0x198, CPLL 0x1b8, GPLL 0x1d8, NPLL 0x1f8,
     *  PPLL php+0x218, B0 0x50018, B1 0x52038, LPLL 0x58058, ...).
     */
    if ((off & 0x3f) == 0x18 || (off & 0x3f) == 0x38) {
        stored |= 1u << 15;    /* PLL locked */
    }
    return stored;
}

/*
 * PMU_PWR_GATE_SFTCON2 (0x8154): pd_ddr01_dwn_sftena (bit 0) and
 * pd_ddr23_dwn_sftena (bit 1), each with a write-enable at bit 16/17.
 * PMU_PWR_GATE_STS1 (0x8184) reports the power state of VD_DDR01/23
 * (bit 0 / bit 1). The TPL DDR-resume path enables power-down, polls
 * STS1 until it is set, then clears the enable and polls until it is
 * clear. The status is modelled as the last enable written.
 */
static uint32_t rk_pmu_pwr_gate_en;

/*
 * PMU_BUS_IDLE_REQ0/1 (0x810c/0x8110) -> IDLE_ACK (0x8118) and
 * PMU2_IDLE (0x8120, and 0x8124 for the domains with a m_offset of 4):
 * the Linux rockchip-pm-domains driver (rk3588_pmu: req_offset 0x10c,
 * ack_offset 0x118, idle_offset 0x120) polls ack/idle until they mirror
 * the request bits - same bit for requests in the low word (0x10c), +16
 * for the high one (0x110).  The TPL DDR-resume path is the same rule
 * (SFTCON1 bits 12-15 -> ack bits 28-31, kept below as a legacy OR).
 * No BIU traffic is modelled, so the ack is immediate.
 */
static uint32_t rk_pmu_idle_req[2];

/* PMU2_CPU_AUTO_PWR_CON has one word per core (TF-A PLATFORM_CORE_COUNT). */
#define RK3588_PMU_NR_CPUS  8

static void rk_stub_pmu_write(hwaddr off, uint64_t value)
{
    if (off == 0x8154) {
        uint32_t wen = (value >> 16) & 0x3;
        uint32_t data = value & 0x3;

        rk_pmu_pwr_gate_en = (rk_pmu_pwr_gate_en & ~wen) | (data & wen);
    }
    /*
     * PMU2_CPU_AUTO_PWR_CON(cpu) (0x8010+4*cpu, hiword write-enable).
     * TF-A cpus_power_domain_on() (atf/.../pmu.c:733-736) clears
     * core_pm_en (bit0) then sets core_pm_sft_wakeup_en (bit3) and
     * returns PSCI success without waiting — the real PMU then powers
     * the core and it fetches pmu_cpuson_entrypoint (0xFF100000).
     * QEMU has no PMU power model, so treat "sft_wakeup_en being set"
     * as the wake event and bring the vCPU up ourselves (perf.md A2).
     */
    if (off >= 0x8010 && off < 0x8010 + RK3588_PMU_NR_CPUS * 4 &&
        ((off - 0x8010) & 3) == 0) {
        uint32_t cpu = (off - 0x8010) / 4;
        uint32_t wen = value >> 16;
        uint32_t data = value & 0xffff;

        /* Setting core_pm_sft_wakeup_en (bit 3) requests the wake. */
        if ((data & wen & (1u << 3)) || (wen == 0 && (data & (1u << 3)))) {
            rk3588_pmu_cpu_wake(cpu);
        }
    }
    if (off == 0x810c || off == 0x8110) {
        uint32_t idx = (off - 0x810c) / 4;
        uint32_t wen = value >> 16;

        if (wen) {
            rk_pmu_idle_req[idx] =
                (rk_pmu_idle_req[idx] & ~wen) | (value & wen);
        } else {
            rk_pmu_idle_req[idx] = value;    /* plain regmap write */
        }
    }
}

static uint64_t rk_stub_pmu_read(uint32_t stored, hwaddr off)
{
    if (off == 0x4000) {
        return 0x3588;
    }
    if (off == 0x8184) {
        return (stored & ~0x3u) | rk_pmu_pwr_gate_en;
    }
    /*
     * PMU2_CLUSTER_ST (0x8080): BL31 pmu.c polls CLUSTER_STS_NONBOOT_CPUS_DWN
     * (0xfe) after powering off the non-boot cores; with no power model
     * the cores are already down, so report that.
     */
    if (off == 0x8080) {
        return stored | 0xfe;
    }
    /*
     * PMU_BIU_IDLE_ACK_STS0 (0x8118), PMU2 idle status (0x8120) and the
     * m_offset-4 idle word (0x8124): mirror the request bits (see
     * rk_stub_pmu_write above); bits 28-31 additionally acknowledge the
     * DDRSCH0-3 idle requests for the TPL DDR-resume path (SFTCON1
     * 0x8110, bits 12-15).
     */
    if (off == 0x8118 || off == 0x8120 || off == 0x8124) {
        uint32_t mirror = (rk_pmu_idle_req[0] & 0xffff) |
                          ((rk_pmu_idle_req[1] & 0xffff) << 16);

        return stored | mirror | 0xf0000000u;
    }
    /*
     * Linux rockchip-pm-domains (drivers/pmdomain/rockchip/pm-domains.c,
     * syscon window fd8d8000; offsets relative to fd8d0000 below): report
     * every domain powered and every chain up, with no power model.
     *   +0x8180 status   (1'b1 = off)          -> 0: all domains on
     *   +0x81f0 chain status (1'b1 = up)       -> all chains up
     *   +0x81f8 mem status  (1'b1 = off)       -> 0: all mem on
     *   +0x8290 repair status (1'b1 = on)      -> all domains on
     * power_on then short-circuits in rockchip_pd_power(); power_off
     * polls fail after the 10 ms readx timeout and the domain stays on.
     *
     * B3 note: making power_off actually succeed (tracking pwr writes
     * and reflecting them in status/repair reads) broke pm-genpd at
     * probe: need_regulator domains (gpu/npu/vcodec) call
     * rockchip_pd_power(pd, false) before pm_genpd_init; with a
     * working power_off, is_on returns false and pm_genpd_init gets
     * start_off=true, after which pm_genpd_add_subdomain() returns
     * -EINVAL for the npu->nputop edge and the whole rockchip-pm-domain
     * probe fails.  Deferred until a fix that keeps need_regulator
     * domains "on" from genpd's perspective (e.g. skip pwr tracking
     * for the probe-time write, or fix the genpd interaction).
     */
    if (off == 0x8180 || off == 0x81f8) {
        return 0;
    }
    if (off == 0x81f0 || off == 0x8290) {
        return 0xffffffffu;
    }
    return stored;
}

static uint64_t rk_stub_edp_read(uint32_t stored, hwaddr off)
{
    switch (off) {
    case 0x600:
        stored |= 1u << 2;     /* DET_STA */
        break;
    case 0x608:
        stored |= 1u << 6;     /* HPD */
        break;
    case 0x6c0:
        stored |= 1u << 4;     /* PLL lock */
        break;
    }
    return stored;
}

static uint64_t rk_stub_otp_read(uint32_t stored, hwaddr off)
{
    if (off == 0x84) {
        stored |= 1u << 1;     /* OTPC_INT_ST: RD_DONE */
    }
    return stored;
}

/*
 * TRNG (fe378000) behavioural model for the Linux rockchip-rng driver
 * (perf.md B4).  rk3588_rng_init() polls TRNG_V1_STAT (0x04) until SEEDED
 * (bit9) is set with GENERATING (bit30) / RESEEDING (bit31) clear; a
 * zero read-back never satisfies that, which is the observed
 * "timed out waiting for hwrng to reseed" (-ETIMEDOUT after 10ms).
 *
 * The read path (rk3588_rng_read) writes CTRL=RAND then polls ISTAT (0x14)
 * for RAND_RDY (bit0, already faked) and copies 32 bytes out of RAND0..7
 * (0x20..0x3c).  The driver polls — no IRQ, so no SPI wiring is needed.
 * Random bytes come from a simple xorshift seeded by the host clock;
 * quality is irrelevant here, the point is that the poll terminates and
 * the guest gets non-zero data.
 */
static uint32_t rk_rng_prng_state;
static bool rk_rng_prng_seeded;

static uint32_t rk_rng_prng(void)
{
    uint32_t x;

    if (!rk_rng_prng_seeded) {
        rk_rng_prng_state = (uint32_t)g_get_monotonic_time() ^ 0x9e3779b9u;
        rk_rng_prng_seeded = true;
    }
    x = rk_rng_prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rk_rng_prng_state = x;
}

static uint64_t rk_stub_rng_read(uint32_t stored, hwaddr off)
{
    switch (off) {
    case 0x04:
        return 1u << 9;        /* TRNG_V1_STAT: SEEDED, idle */
    case 0xf0:
        return 0x46bc;         /* TRNG_V1_VERSION */
    case 0x14:
        stored |= 1u << 0;     /* ISTAT: RAND_RDY */
        break;
    }
    /* RAND0..RAND7 (0x20..0x3c): random payload */
    if (off >= 0x20 && off <= 0x3c) {
        return rk_rng_prng();
    }
    return stored;
}

/*
 * SDRAM OS regs (0xFD58A208 + 8*bank) live inside the grf-boot page and
 * are what SdramGetMemorySize() (SdramLib) uses to size DRAM for the EDK2
 * MMU map.  All-zero reads decode as DDR4/rank1/9col/16row -> 256MB per
 * channel -> 512MB total, which leaves everything above 0x29400000 (the
 * 0x40000000 SEC stack!) unmapped after ArmConfigureMmu.  Set CS0_ROW_LO
 * (OsReg bits[7:6]) to 3 => row=16 => 1<<11 = 2048MB per bank, 4GB total;
 * MIN()'d with 0xF0000000 that covers all of QEMU's low RAM exactly.
 * Only reads are affected — nothing writes these offsets.
 */
static uint64_t rk_stub_grf_boot_read(uint32_t stored, hwaddr off)
{
    if (off == 0x208 || off == 0x210) {
        stored |= 0xc0;        /* SYS_REG2: CS0_ROW_LO = 3 => 16 rows */
    }
    return stored;
}

/*
 * HDMI0 hotplug level: the Linux dw_hdmi_qp-rockchip glue detects the
 * connector through RK3588_GRF_SOC_STATUS1 (sys_grf +0x384), bit16 =
 * RK3588_HDMI0_LEVEL_INT.  The model has no cable state, so the
 * "monitor" is permanently plugged (the hpd IRQ is never asserted; a
 * real board powers up with the sink attached too).
 */
static uint64_t rk_stub_sysgrf_read(uint32_t stored, hwaddr off)
{
    if (off == 0x384) {
        stored |= 1u << 16;      /* HDMI0 HPD: connected */
    }
    return stored;
}

/*
 * Samsung HDPTX PHY GRF (hdptxphy0_grf, 0xfd5e0000) for the Linux
 * phy-rockchip-samsung-hdptx driver: CON0 @0x00 (MODE_SEL bit0, BGR
 * bit5, BIAS bit6, PLL_EN bit7) is plain hiword storage; STATUS @0x80
 * is read-only and must present lock/ready bits as soon as the driver
 * enables the PLL (its polls run with 400us/5ms budgets):
 *   bit3 PLL_LOCK_DONE, bit2 PHY_CLK_RDY: follow PLL_EN;
 *   bit1 PHY_RDY: additionally needs the lane-top 4-lane enable
 *       (PHY window LNTOP_REG(0207) @0x81c != 0, written before the
 *       lane-lock poll);
 *   bit0 SB_RDY (DP AUX path): follows DP mode + BGR.
 * The PHY register window itself stays plain storage, so the clock
 * recalc_rate reads return what the driver programmed.
 */
static uint32_t rk_hdptx_con0;
static uint32_t rk_hdptx_lntop_lane_en;

static void rk_stub_hdptx_grf_write(hwaddr off, uint64_t value)
{
    if (off == 0x00) {
        /* hiword write-mask: the driver programs CON0 bit by bit
           (BGR/BIAS, then PLL_EN, then BGR/BIAS again before the
           lane-lock poll), so the bits accumulate. */
        uint32_t mask = (value >> 16) & 0xffff;

        rk_hdptx_con0 = (rk_hdptx_con0 & ~mask) | ((uint32_t)value & mask);
    }
}

static void rk_stub_hdptx_phy_write(hwaddr off, uint64_t value)
{
    if (off == 0x81c) {
        rk_hdptx_lntop_lane_en = value;
    }
}

static uint64_t rk_stub_hdptx_grf_read(uint32_t stored, hwaddr off)
{
    if (off == 0x80) {
        uint32_t st = 0;

        if (rk_hdptx_con0 & 0x80) {            /* PLL_EN */
            st |= (1 << 3) | (1 << 2);          /* PLL_LOCK_DONE|CLK_RDY */
            if (rk_hdptx_lntop_lane_en) {
                st |= 1 << 1;                   /* PHY_RDY */
            }
        }
        if ((rk_hdptx_con0 & 0x21) == 0x21) {   /* DP mode + BGR */
            st |= 1 << 0;                       /* SB_RDY */
        }
        return st;
    }
    return stored;
}

/*
 * Rockchip SPI (SpiLib; RK806 PMIC driver on spi2).  Register offsets are
 * those of SpiLib's struct SPI_REG, confirmed against RK3588Dxe's
 * SPI_PioTransfer/SPI_Stop code (TXFLR@0x1c, RXFLR@0x20, SR@0x24,
 * TXDR@0x400, RXDR@0x800).  Transfers run as a separate TX-only then
 * RX-only CS window: RXFLR must report data waiting so the RX-only
 * window's `while (remain)` loop drains instead of spinning forever at
 * 0; TXFLR stays 0 (TX room always open); SR never has BUSY set so
 * SPI_Stop's unbounded busy poll exits immediately.  SPI_FlushFifo
 * (while-RXFLR drain) has no callers, so a constant non-zero RXFLR is
 * safe.  RXDR reads return the untouched backing store (zero).
 */
static uint64_t rk_stub_spi_read(uint32_t stored, hwaddr off)
{
    switch (off) {
    case 0x20:                  /* RXFLR: fake RX bytes waiting */
        return 0x20;
    case 0x24:                  /* SR: never report BUSY (bit0) */
        return 0;
    }
    return stored;
}

/*
 * DesignWare EQOS MAC (gmac0/gmac1): GmacPlatformDxe's PhyRead/PhyWrite
 * poll GMAC_MAC_MDIO_ADDRESS's GB busy bit (bit0), expecting the hardware
 * to clear it when the clause-22 operation completes; backing RAM would
 * keep it set forever and burn the full 1000 x 10us timeout per access.
 * MDIO_DATA reads return the untouched backing store (zero -> unknown PHY
 * ID, which the driver treats as non-fatal).
 */
static uint64_t rk_stub_gmac_read(uint32_t stored, hwaddr off)
{
    if (off == 0x200) {         /* GMAC_MAC_MDIO_ADDRESS: self-clear GB */
        stored &= ~1u;
    }
    return stored;
}

/*
 * BL31 SCMI clock queries (rk3588_clk.c clk_scmi_*_get_rate): when the
 * cluster mux selects its pvtPLL, the driver reads the PVTPLL STATUS
 * register from the cluster GRF (freq in MHz, & 0x3FFF).  STATUS is
 * read-only hardware, never written, so backing RAM would return 0 and
 * make EDK2's OPP voltage lookup see 0 Hz.  Report a plausible fixed
 * frequency instead (GetOppVoltage just needs Hz <= some OPP entry).
 */
static uint64_t rk_stub_pvtpll_read(uint32_t stored, hwaddr off)
{
    switch (off) {
    case 0x18:                  /* CPUB / GPU STATUS */
    case 0x24:                  /* NPU STATUS */
    case 0x60:                  /* CPUL STATUS */
    case 0x80:                  /* DSU STATUS */
        return 1500;
    }
    return stored;
}

/*
 * Pcie30PhyLib polls PCIE3PHY_GRF_PHY0/PHY1_STATUS1 (0x904/0xa04) for
 * SRAM_INIT_DONE (bit0) after deasserting PHY reset (500 x 100us).
 * Report done so the init succeeds instead of timing out non-fatally.
 */
static uint64_t rk_stub_pcie3phy_read(uint32_t stored, hwaddr off)
{
    if (off == 0x904 || off == 0xa04) {
        return 1;
    }
    return stored;
}

/*
 * PCIe APB client registers (Rk3588PciHostBridgeLib PciHostBridgeInit):
 * PciSetRcMode/PciEnableLtssm write +0x0/+0x1c/+0x2c/+0x180, then
 * PciIsLinkUp polls PCIE_CLIENT_LTSSM_STATUS (+0x300) for
 * RDLH_LINK_UP|SMLH_LINK_UP and state 0x11 (10 x 100ms retries before
 * EFI_TIMEOUT).  Report link-up so segment init completes at once.
 * Region covers all five controller windows (5 x 64KB).
 */
static uint64_t rk_stub_pcie_apb_read(uint32_t stored, hwaddr off)
{
    if ((off & 0xffff) == 0x300) {
        return 0x30011;         /* RDLH_LINK_UP | SMLH_LINK_UP | LINK_UP */
    }
    return stored;
}

/*
 * PCIe ECAM config space: report VID/DID 0xFFFF at the head of every
 * 4KB function so PciBusDxe treats all slots as empty instead of
 * attaching to thousands of phantom vendor-0 devices.  Backing store
 * stays 0 elsewhere (PciValidateCfg0 then skips the ECAM-compliance
 * PCD update, which is fine).
 */
static uint64_t rk_stub_pcie_ecam_read(uint32_t stored, hwaddr off)
{
    if ((off & 0xfff) == 0) {
        return 0xffffffff;
    }
    return stored;
}

/*
 * Rockchip USBDP combo PHY (UsbDpPhyDxe, Samsung IP): the driver
 * regmap_read_poll_timeout()s four "done" status words in the PMA block
 * (window base + 0x8000) during PLL/CDR bring-up.  Report them done
 * immediately so init doesn't burn its poll timeouts (or fail).
 */
static uint64_t rk_stub_usbdp_read(uint32_t stored, hwaddr off)
{
    switch (off) {
    case 0x8350:                 /* CMN ANA LCPLL LOCK+AFC done (bits 7:6) */
        return (stored & ~0xc0u) | 0xc0;
    case 0x8354:                 /* CMN ANA ROPLL LOCK+AFC done (bits 1:0) */
        return (stored & ~0x3u) | 0x3;
    case 0x8b84:                 /* TRSV LN0 RX CDR lock done (bit 0) */
    case 0x9b84:                 /* TRSV LN2 RX CDR lock done (bit 0) */
        return stored | 0x1;
    }
    return stored;
}

/*
 * GPIO0 EXT_PORTA: input pins read back 0 from the stub backing store.
 * PA4 is the SD-card-detect input (active low, RkSdmmcGetCardPresenceState).
 * It is driven from the real SD slot (rk3588-dwmshc.c): high when no card
 * is attached, so the P7 firmware path without -drive if=sd is unchanged;
 * low (card present) when a card is attached.
 */
static uint64_t rk_stub_gpio0_read(uint32_t stored, hwaddr off)
{
    if (off == 0x70 && !rk3588_sd_card_inserted) {
        stored |= 1u << 4;      /* PA4: SD CD deasserted (no card) */
    }
    return stored;
}

/*
 * DesignWare SDHCI (eMMC, sdhci@fe2b0000) used by EDK2's DwcSdhciDxe +
 * generic SdMmcPciHcDxe.  The controller must look idle and "done" so the
 * driver's bounded waits (SdMmcHcWaitMmioSet/SdMmcWaitTrb, 1e6 iterations
 * with 1us stalls each) exit immediately instead of burning seconds per
 * command.  There is no card behind it: PRESENT_STATE stays 0 so the media
 * check reports no card inserted and the boot path skips storage.
 */
static uint64_t rk_stub_sdhci_read(uint32_t stored, hwaddr off)
{
    switch (off) {
    case 0x02c:
        /* word: CLOCK_CTRL (15:0), TIMEOUT_CTRL (23:16), SW_RST (31:24) */
        stored &= ~(0x0fu << 24);   /* SW_RST self-clears => reset done */
        stored |= 0x02;             /* CLOCK_CTRL: internal clock stable */
        break;
    case 0x030:
        /* NOR_INT_STS (15:0) fakes command + transfer + DMA + buffer
           ready; ERR_INT_STS (31:16) stays 0 (no errors).  Card
           insertion/removal event bits (6/7) stay 0. */
        stored = 0x0000003b;
        break;
    case 0x040:
        stored |= 0x00003400;       /* CAP: base clock 52 MHz (bits 15:8) */
        break;
    case 0x840:
        /* EMMC_DLL_STATUS0: report DLL lock achieved, no timeout */
        stored = (stored & ~(1u << 9)) | (1u << 8);
        break;
    }
    return stored;
}

static uint64_t rk_stub_read(void *opaque, hwaddr offset, unsigned size)
{
    RkStub *s = opaque;
    hwaddr aoff = offset & ~(hwaddr)3;
    uint32_t v = s->regs[aoff / 4];
    uint32_t mask = size >= 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);

    if (s->read_hook) {
        v = s->read_hook(v, aoff);
    }
    /*
     * DDR config-engine status model used by the blob (window regs,
     * reg = offset & 0xfff; engine sub-block = offset >> 16):
     *  0x80  START write: bit31 reads back self-cleared.
     *  0x90  access status: bit0 busy stays clear, bit16 read-done
     *        (helpers ff004fcc / ff005dbc).
     *  0x14  reads 0 (idle) until a command is issued to +0xC80,
     *        then done/ready (bit0).
     *  0xb4  training-done flag, bit18 (polls ff00ab00, ff00b9e8).
     *  0x3cc done flag, polled once.
     *  0xc84 mirrors bit0 of command reg +0xC80.
     *  0x514 toggle status (init 1): flips on every +0x510 write whose
     *        value has bit0 clear; the blob polls it for both polarities
     *        (ff00baac=1, ff0059b0=0, ff005a34=1).
     */
    if (s->track_engine) {
        hwaddr reg = aoff & 0xfff;

        if (reg == 0x514) {
            /*
             * GUESS (flip-on-read, replaces a fixed-polarity rule):
             * +0x514 bit0 is a phase the blob polls with alternating
             * expectations at identical write sequences (ff00baa0 sets
             * 0x20 of +0x510 then wants bit0=1; ff00a540 sets 0x20 then
             * wants bit0=0; ff00a5f8 clears 0x20 then wants bit0=1), so
             * no fixed function of the preceding writes fits.  Model:
             * bit0 flips on every read, starting at 1, so any polling
             * loop exits within two reads.  The board's end-of-training
             * +0x514 is 1.
             */
            v = (v & ~1u) | s->tally[aoff >> 16];
            s->tally[aoff >> 16] ^= 1;
        } else if (reg == 0xc84) {
            v = s->regs[(aoff - 4) / 4] & 1;
        } else if (reg == 0x80) {
            v &= ~0x80000000u;    /* START is self-clearing */
        } else if (reg == 0x3cc) {
            v |= 1;             /* done flag, polled once */
        } else if (reg == 0x14 && s->engine_ready) {
            v |= 1;
        } else if (reg == 0xb4) {
            /* training-done style flag polled at ff00ab00/ff00b9e8 */
            v |= 1u << 18;
        } else if (reg == 0x90) {
            /* engine access status: bit0 busy (stays clear), bit16
               read-done, polled by helper ff005dbc; ff004fcc waits
               only for bit0 */
            v |= 1u << 16;
        }
    }
    return (v >> ((offset & 3) * 8)) & mask;
}

static void rk_stub_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    RkStub *s = opaque;
    hwaddr aoff = offset & ~(hwaddr)3;
    uint32_t *reg = &s->regs[aoff / 4];

    if (s->write_hook) {
        s->write_hook(aoff, value);
    }

    if (s->track_engine) {
        hwaddr r = aoff & 0xfff;

        if (r == 0xc80) {
            s->engine_ready = true;
        }
    }
    if (size < 4) {
        /* Sub-word stores hit distinct byte lanes of one register word
           (e.g. SDHCI packs CLOCK_CTRL/TIMEOUT/SW_RST in one word). */
        unsigned shift = (offset & 3) * 8;
        uint32_t mask = (((uint32_t)1 << (size * 8)) - 1) << shift;

        *reg = (*reg & ~mask) | (((uint32_t)value << shift) & mask);
    } else if (s->hiword && !(aoff >= s->plain_lo && aoff < s->plain_hi)) {
        uint32_t mask = (value >> 16) & 0xffff;
        uint32_t data = value & 0xffff;

        *reg = (*reg & 0xffff & ~mask) | (data & mask);
    } else {
        *reg = value;
    }
}

static const MemoryRegionOps rk_stub_ops = {
    .read = rk_stub_read,
    .write = rk_stub_write,
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

#define MK_MASK(name_, base_, ...) \
    { .name = (name_), .base = (base_), .size = 0x1000, .hiword = true, ##__VA_ARGS__ }

static const RkStubBlock rk_stub_blocks[] = {
    /* GRF / SGRF / IOC family (all hiword-mask) */
    MK_MASK("pmu0-sgrf",  0xfd580000),
    MK_MASK("pmu1-sgrf",  0xfd582000),
    /* sec GRF is two pages (fd586000 + fd587000, see bootrom fd587030) */
    /*
     * DSU_SGRF_SOC_CON(4) at 0xfd587010 bit4 is the crypto/ddr-resume flag.
     * Stock BL31 (0x587f4) takes its cold-boot save path only when the bit
     * reads 1; reading 0 sends it through an unsaved-context restore that
     * zeroes x20-x23 and loses the BL31 params pointer. Reset value is 1
     * and the save path's hiword write clears it, as on the part.
     */
    { .name = "grf-sec", .base = 0xfd586000, .size = 0x2000, .hiword = true,
      .reset_off = 0x1010, .reset_val = 0x10 },
    /* PMU0_GRF: OS_REG8..11 (0x80-0x8f) are plain RW (TRM ch.6); the TPL
       writes the 0xef08a53c magic to 0x80 and reads it back. */
    MK_MASK("grf-usb",    0xfd588000),
    MK_MASK("grf-boot",   0xfd58a000, .read_hook = rk_stub_grf_boot_read,
      .write_hook = rk_stub_poweroff_write),
    MK_MASK("sys-grf",    0xfd58c000, .read_hook = rk_stub_sysgrf_read),
    /* Cluster GRFs: BL31 SCMI clk set_rate writes pvtPLL CON regs and
       get_rate reads pvtPLL STATUS (rk3588_clk.c) */
    MK_MASK("bigcore0-grf", 0xfd590000, .read_hook = rk_stub_pvtpll_read),
    MK_MASK("bigcore1-grf", 0xfd592000, .read_hook = rk_stub_pvtpll_read),
    MK_MASK("litcore-grf",  0xfd594000, .read_hook = rk_stub_pvtpll_read),
    MK_MASK("dsu-grf",      0xfd598000, .read_hook = rk_stub_pvtpll_read),
    MK_MASK("vop-grf",    0xfd5a4000),
    MK_MASK("vo0-grf",    0xfd5a6000),
    MK_MASK("vo1-grf",    0xfd5a8000),
    MK_MASK("usb-grf",    0xfd5ac000),
    /* PHP GRF (GmacPlatformDxe CLK_CON1/GMAC_CON0, Pcie30PhyLib
       PCIESEL_CON, ComboPhy) + Combo PHY GRFs (ComboPhy InitComPhy) */
    MK_MASK("php-grf",    0xfd5b0000),
    MK_MASK("pcie3phy-grf", 0xfd5b8000, .read_hook = rk_stub_pcie3phy_read),
    MK_MASK("pipe-phy0-grf", 0xfd5bc000),
    MK_MASK("pipe-phy1-grf", 0xfd5c0000),
    MK_MASK("hdptxphy0-grf", 0xfd5e0000,
      .read_hook = rk_stub_hdptx_grf_read,
      .write_hook = rk_stub_hdptx_grf_write),
    MK_MASK("hdptxphy1-grf", 0xfd5e4000),
    MK_MASK("pipe-phy2-grf", 0xfd5c4000),
    MK_MASK("usbdpphy0-grf", 0xfd5c8000),
    MK_MASK("usbdpphy1-grf", 0xfd5cc000),
    MK_MASK("usb2phy0-grf", 0xfd5d0000),
    MK_MASK("usb2phy1-grf", 0xfd5d4000),
    MK_MASK("usb2phy2-grf", 0xfd5d8000),
    MK_MASK("usb2phy3-grf", 0xfd5dc000),
    MK_MASK("mipidcphy0-grf", 0xfd5e8000),
    MK_MASK("mipidcphy1-grf", 0xfd5ec000),
    { .name = "ioc", .base = 0xfd5f0000, .size = 0x10000, .hiword = true },

    /*
     * CRU family (hiword-mask, PLL lock reads).
     *
     * Linux clk-rk3588 recalc_rate() reads PLLCON0/1/2 at registration
     * (rate = 24MHz * M / (P * 2^S) per the rk3588_pll_rates tuples),
     * so the PLLs are preloaded with canonical firmware rates:
     *   GPLL 1188MHz, CPLL/NPLL 1000/850, PPLL 100, V0PLL/AUPLL 1000.
     * CON0 = M, CON1 = P | S<<6, CON2 = K, all per RK3588_PLL_RATE().
     */
    { .name = "cru", .base = 0xfd7c0000, .size = 0x1000, .hiword = true,
      .read_hook = rk_stub_cru_main_read,
      .write_hook = rk_stub_atf_reset_write,
      .reset_vals = (const RkStubResetVal[]) {
        /* V0PLL PLL_CON(88) @ 0x160: 1000 MHz */
        { 0x160, 500 }, { 0x164, 3 | (2 << 6) }, { 0x168, 0 },
        /* AUPLL PLL_CON(96) @ 0x180: 1000 MHz */
        { 0x180, 500 }, { 0x184, 3 | (2 << 6) }, { 0x188, 0 },
        /* CPLL PLL_CON(104) @ 0x1a0: 1000 MHz */
        { 0x1a0, 500 }, { 0x1a4, 3 | (2 << 6) }, { 0x1a8, 0 },
        /* GPLL PLL_CON(112) @ 0x1c0: 1188 MHz */
        { 0x1c0, 198 }, { 0x1c4, 2 | (1 << 6) }, { 0x1c8, 0 },
        /* NPLL PLL_CON(120) @ 0x1e0: 850 MHz */
        { 0x1e0, 425 }, { 0x1e4, 3 | (2 << 6) }, { 0x1e8, 0 },
      }, .n_reset_vals = 15 },
    { .name = "php-ppll-cru", .base = 0xfd7c8000, .size = 0x1000,
      .hiword = true, .read_hook = rk_stub_cru_read,
      .reset_vals = (const RkStubResetVal[]) {
        /* PPLL PLL_CON(128) @ 0x200: 100 MHz */
        { 0x200, 400 }, { 0x204, 3 | (5 << 6) }, { 0x208, 0 },
      }, .n_reset_vals = 3 },
    MK_MASK("secure-cru",   0xfd7d0000, .read_hook = rk_stub_cru_read),
    MK_MASK("sbus-cru",     0xfd7d8000, .read_hook = rk_stub_cru_read),
    MK_MASK("pmu1-cru",     0xfd7f0000, .read_hook = rk_stub_cru_read),
    /* DDR blob PLL-config table writes 0x80008000 to entries at
       0xfd800000, 0xfd804000, ... (hiword) */
    { .name = "fd80-cru", .base = 0xfd800000, .size = 0x10000,
      .hiword = true, .read_hook = rk_stub_cru_read },
    /* Per-core PLL windows inside the CRU space (B0/B1/LPLL lock polls).
       Preloaded at 1188 MHz so armclk recalc gets a sane parent rate. */
    { .name = "bigcore0-cru", .base = 0xfd810000, .size = 0x1000,
      .hiword = true, .read_hook = rk_stub_cru_read,
      .reset_vals = (const RkStubResetVal[]) {
        { 0x00, 198 }, { 0x04, 2 | (1 << 6) }, { 0x08, 0 },
      }, .n_reset_vals = 3 },
    { .name = "bigcore1-cru", .base = 0xfd812000, .size = 0x1000,
      .hiword = true, .read_hook = rk_stub_cru_read,
      .reset_vals = (const RkStubResetVal[]) {
        { 0x20, 198 }, { 0x24, 2 | (1 << 6) }, { 0x28, 0 },
      }, .n_reset_vals = 3 },
    MK_MASK("litcr-cru",    0xfd814000, .read_hook = rk_stub_cru_read),
    { .name = "dsu-cru", .base = 0xfd818000, .size = 0x1000,
      .hiword = true, .read_hook = rk_stub_cru_read,
      .reset_vals = (const RkStubResetVal[]) {
        { 0x40, 198 }, { 0x44, 2 | (1 << 6) }, { 0x48, 0 },
      }, .n_reset_vals = 3 },

    /* PMU (hiword-mask, version + power status reads) */
    { .name = "pmu", .base = 0xfd8d0000, .size = 0x10000, .hiword = true,
      .read_hook = rk_stub_pmu_read, .write_hook = rk_stub_pmu_write },

    /* RK3588 SARADC (adc@fec10000, u-boot "adc@fec10000" HW-ID channel):
       EDK2's early board detect writes CONV/DAT at +0x00/+0x04; plain
       registers, no hiword write-enables. */
    { .name = "saradc", .base = 0xfec10000, .size = 0x1000 },

    /* GPIO (hiword-mask, gen_pin_mask uses pin + 16 mask bits) */
    { .name = "gpio0", .base = 0xfd8a0000, .size = 0x1000, .hiword = true,
      .read_hook = rk_stub_gpio0_read,
      .write_hook = rk_stub_atf_poweroff_write },
    MK_MASK("gpio1", 0xfec20000),
    MK_MASK("gpio2", 0xfec30000),
    MK_MASK("gpio3", 0xfec40000),
    MK_MASK("gpio4", 0xfec50000),
    /*
     * MAILBOX0 (MCU_PMU), TRM Ch.16. BL31 read-modify-writes B2A_INTEN
     * (+0x28) during init (0x5aafc in stock atf-1). Plain storage is
     * enough for that; the B2A status/W1C path is not modelled yet.
     */
    { .name = "mailbox0", .base = 0xfec60000, .size = 0x1000 },

    /* DDR secure-range GRFs (RMW writes, hiword-mask) */
    MK_MASK("ddrgrf0", 0xfe010000),
    MK_MASK("ddrgrf1", 0xfe030000),
    MK_MASK("ddrgrf2", 0xfe038000),

    /* HP "STIMER" (U-Boot CONFIG_ROCKCHIP_STIMER_BASE): the DDR blob's
       cntfrq/timebase setup writes CTRL +0x04, INT_CLR +0x08,
       LOAD_COUNT0/1 +0x14/+0x18 before falling back to cntpct_el0 delays. */
    { .name = "stimer", .base = 0xfd8c8000, .size = 0x1000 },

    /* Secure system timers: BL31's bl31_main zeroes channel regs (write 0
       at fed300b0, fed300a0, fed300a4 => STIMER1_CONTROL/LOAD/CURRENT);
       pm_pd_regs.c also lists STIMER0/1 +0x00..+0x80 for suspend paths. */
    { .name = "stimer0", .base = 0xfe3d0000, .size = 0x1000 },
    { .name = "stimer1", .base = 0xfed30000, .size = 0x1000 },

    /* DDR blob probes fixed bases 0xf7000000/0xf7010000/0xf7100800/
       0xf8000000/0xf9000000/... (reg reads need bits[2:0] clear —
       zeroed reads pass); not an RK3588 block, cover the candidate
       range up to the GPU at 0xfb000000 */
    { .name = "ddr-probe", .base = 0xf7000000, .size = 0x4000000,
      .track_engine = true,
      .read_hook = rk3588_ddr_probe_read,
      .write_hook = rk3588_ddr_probe_write },

    /* DDR blob power/clock table: bases 0xfd59c000, 0xfd59d000, ...,
       writes +0x130/+0x134 with plain values (0x80000 then 0x80008)
       around cntpct delays (stays below vop-grf at 0xfd5a4000) */
    { .name = "ddr-cfg1", .base = 0xfd59c000, .size = 0x8000 },

    /* DDR blob writes +0xa94/+0xbf0/+0xbf4 (values like 0x9) to blocks
       at 0xfe0c0000, 0xfe0d0000, ... */
    /* DDR controller windows (blob table DAT_ff015a48.. = 0xfe000000 +
       0x2000*n, decompile names); unmapped before => data abort at 0x200 */
    { .name = "ddr-ctl-win", .base = 0xfe000000, .size = 0x8000,
      .track_engine = true },
    /* v1.24 blob DQ test engine: program window + pass status at +0x54.
       Without this the first program write data-aborts (vector 0x200). */
    { .name = "ddr-test-engine", .base = 0xfe050000, .size = 0x1000,
      .track_engine = true, .read_hook = rk3588_ddr_test_read },
    { .name = "ddr-cfg2", .base = 0xfe0c0000, .size = 0x40000,
      .track_engine = true, .read_hook = rk3588_ddr_cfg2_read },

    /* Plain read/write blocks (firmware RMW => no mask semantics) */
    /* RK3588 combo PIPE PHYs (ComboPhy InitComPhyConfig, plain RMW) */
    { .name = "combo-phy0", .base = 0xfee00000, .size = 0x1000 },
    { .name = "combo-phy1", .base = 0xfee10000, .size = 0x1000 },
    { .name = "combo-phy2", .base = 0xfee20000, .size = 0x1000 },
    /* USBDP combo PHYs (UsbDpPhyDxe): full 64K windows per dts
       (usbdp_phy0@fed80000, usbdp_phy1@fed90000) */
    { .name = "usbdp-phy0", .base = 0xfed80000, .size = 0x10000,
      .read_hook = rk_stub_usbdp_read },
    { .name = "usbdp-phy1", .base = 0xfed90000, .size = 0x10000,
      .read_hook = rk_stub_usbdp_read },
    /* PCIe APB client windows: 5 x 64KB (seg4 = pcie2x1l2 @0xfe190000);
       LTSSM_STATUS link-up poll in the read hook */
    { .name = "pcie-apb", .base = 0xfe150000, .size = 0x50000,
      .read_hook = rk_stub_pcie_apb_read },
    /* DWC DBI: 5 x 4MB at 0xa40000000 (seg4 @0xa41000000): BARs, iATU
       view at +0x300000, link control/status, DBI2 header at +0x100000 */
    { .name = "pcie-dbi", .base = 0xa40000000, .size = 0x1400000 },
    /* ECAM window for the one enabled segment (seg4 = pcie2x1l2).
       v1.1 computes CFG_BASE(seg) = (seg + 0x24) << 30 with
       PCIE_BUS_BASE(seg) == 0 (no bus-number spacing), i.e. the full
       256-bus 28-bit ECAM space at (seg + 0x24) << 30:
       seg4 -> 0xa00000000, covering bus0..255 (bus 0 is served via
       DBI by PciSegmentLib, but ValidateCfg0/scan hits bus 1+ here).
       Guest reads mostly touch only the VID words (hooked to 0xffffffff),
       so the 256 MB backing stays lazily zero-backed. */
    { .name = "pcie-ecam4", .base = 0xa00000000, .size = 0x10000000,
      .read_hook = rk_stub_pcie_ecam_read },
    /* RK3588 dwcmshc eMMC (sdhci@fe2b0000): BAR is 64K per the
       NonDiscoverable Sdhci registration but all registers EDK2 touches
       (incl. DLL at 0x800..0x844) sit in the first page. */
    { .name = "sdhci", .base = 0xfe2b0000, .size = 0x1000,
      .read_hook = rk_stub_sdhci_read },
    /* SD card slot (sdmmc@fe2c0000): real dw-mshc model, rk3588-dwmshc.c */
    /* eMMC dwcmshc SDHCI (sdhci@fe2e0000): real model, rk3588-sdhci.c
       (P7's plain-storage stub here shadowed the device — stubs are
       mapped after the devices, so any block at the same address wins). */
    /* DesignWare EQOS MACs (GmacPlatformDxe MDIO polling + platform
       protocol), plain registers */
    { .name = "gmac0", .base = 0xfe1b0000, .size = 0x2000,
      .read_hook = rk_stub_gmac_read },
    { .name = "gmac1", .base = 0xfe1c0000, .size = 0x2000,
      .read_hook = rk_stub_gmac_read },
    /* RK I2C controllers (i2c0..i2c8) are now real behavioural rk3x
       devices (rk3588-i2c.c, B5 perf.md) mapped at their DTB bases with
       GIC SPI wiring — the passive storage stubs are gone so the Linux
       driver's interrupt handshake completes (and absent addresses NACK
       to a fast -ENXIO instead of -ETIMEDOUT). */

    /* Rockchip SPI controllers (spi0..spi4), plain registers */
    { .name = "spi0", .base = 0xfeb00000, .size = 0x1000,
      .read_hook = rk_stub_spi_read },
    { .name = "spi1", .base = 0xfeb10000, .size = 0x1000,
      .read_hook = rk_stub_spi_read },
    { .name = "spi2", .base = 0xfeb20000, .size = 0x1000,
      .read_hook = rk_stub_spi_read },
    { .name = "spi3", .base = 0xfeb30000, .size = 0x1000,
      .read_hook = rk_stub_spi_read },
    { .name = "spi4", .base = 0xfecb0000, .size = 0x1000,
      .read_hook = rk_stub_spi_read },

    { .name = "pwm0", .base = 0xfd8b0000, .size = 0x1000 },
    { .name = "pwm1", .base = 0xfebd0000, .size = 0x1000 },
    { .name = "pwm2", .base = 0xfebe0000, .size = 0x1000 },
    { .name = "pwm3", .base = 0xfebf0000, .size = 0x1000 },
    /* HDMI0 TX (hdmi@fde80000): real dw-hdmi-qp model, rk3588-hdmi.c */
    /* DW DisplayPort: v1.1's dw_dp_connector_detect() returns -ENODEV
       (TODO stub), so only dw_dp_init/aux_init/hpd_init RMW accesses
       happen; no AUX transactions are ever issued. */
    { .name = "dp0", .base = 0xfde50000, .size = 0x4000 },
    { .name = "dp1", .base = 0xfde60000, .size = 0x4000 },

    /* USB: dwc3 DRD cores (UsbHcdInitDxe reads GSnpsId @0xc120; with 0
       it reports "not a DesignWare USB3 DRD Core" and skips the
       controller gracefully) + EHCI/OHCI USB2 hosts. */
    { .name = "dwc3-0", .base = 0xfc000000, .size = 0x400000 },
    { .name = "dwc3-1", .base = 0xfc400000, .size = 0x400000 },
    { .name = "dwc3-2", .base = 0xfcd00000, .size = 0x400000 },
    { .name = "ehci0", .base = 0xfc800000, .size = 0x40000 },
    { .name = "ohci0", .base = 0xfc840000, .size = 0x40000 },
    { .name = "ehci1", .base = 0xfc880000, .size = 0x40000 },
    { .name = "ohci1", .base = 0xfc8c0000, .size = 0x40000 },
    /* VOP2 (vop@fdd90000): real model, rk3588-vop2.c; gamma-LUT
       window (reg "gamma-lut" 0xfdd95000/0x1000) is plain storage —
       written only when userspace installs a LUT. */
    { .name = "vop2-gamma", .base = 0xfdd95000, .size = 0x1000 },
    { .name = "edp0", .base = 0xfdec0000, .size = 0x1000,
      .read_hook = rk_stub_edp_read },
    { .name = "edp1", .base = 0xfded0000, .size = 0x1000,
      .read_hook = rk_stub_edp_read },
    /* 0x2000 per u-boot rockchip,rk3588-hdptx-phy.yaml reg size;
        RockchipHdptxPhyReset touches offsets up to 0x1804.
       LNTOP_REG(0207) @0x81c writes feed the hdptx GRF STATUS model
       (Linux lane-enable before the lane-lock poll). */
    { .name = "edp0tx", .base = 0xfed60000, .size = 0x2000,
      .write_hook = rk_stub_hdptx_phy_write },
    { .name = "edp1tx", .base = 0xfed70000, .size = 0x2000 },

    /* U-Boot (P4): efuse/OTP + TRNG (version + ready reads) */
    { .name = "otp", .base = 0xfecc0000, .size = 0x400,
      .read_hook = rk_stub_otp_read },
    /* Crypto engine (rk_crypto hash path), see rk3588-crypto.c */
    { .name = "crypto", .base = 0xfe370000, .size = 0x8000,
      .read_hook = rk3588_crypto_read, .write_hook = rk3588_crypto_write },
    /* Secure OTP (OTP_S): the SPL reads fuse words here (same protocol) */
    { .name = "secure-otp", .base = 0xfe3a0000, .size = 0x10000,
      .read_hook = rk_stub_otp_read },
    { .name = "trng", .base = 0xfe378000, .size = 0x200,
      .read_hook = rk_stub_rng_read },
    /*
     * PL330 DMAC register tails.  The stock rk3588s DTB gives each
     * dma-controller a 0x4000 reg window while QEMU's in-tree pl330
     * model maps only 0x1000 (with its periph-ID cells at +0xfe0).
     * Linux 7.2.9's amba_read_periphid() reads pid/cid at the END of
     * the DTB window (base + size - 0x20/- 0x10), so [base+0x1000,
     * base+0x4000) must be mapped and answer with the same ID bytes
     * the model returns at +0xfe0 (pl330_id[]): pid 0x241330 ->
     * 0x41330 under the driver's 0x000fffff mask, cid 0xB105F00D.
     * Plain RW so the driver's channel/mgr register traffic in the
     * first page still lands in the real device, and any stray probe
     * read in the tail reads back storage (0).
     */
    { .name = "dmac0-tail", .base = 0xfea11000, .size = 0x3000,
      .reset_vals = (const RkStubResetVal[]) {
        { 0x2fe0, 0x00000030 }, { 0x2fe4, 0x00000013 },
        { 0x2fe8, 0x00000024 }, { 0x2fec, 0x00000000 },
        { 0x2ff0, 0x0000000d }, { 0x2ff4, 0x000000f0 },
        { 0x2ff8, 0x00000005 }, { 0x2ffc, 0x000000b1 },
      }, .n_reset_vals = 8 },
    { .name = "dmac1-tail", .base = 0xfea31000, .size = 0x3000,
      .reset_vals = (const RkStubResetVal[]) {
        { 0x2fe0, 0x00000030 }, { 0x2fe4, 0x00000013 },
        { 0x2fe8, 0x00000024 }, { 0x2fec, 0x00000000 },
        { 0x2ff0, 0x0000000d }, { 0x2ff4, 0x000000f0 },
        { 0x2ff8, 0x00000005 }, { 0x2ffc, 0x000000b1 },
      }, .n_reset_vals = 8 },
    { .name = "dmac2-tail", .base = 0xfed11000, .size = 0x3000,
      .reset_vals = (const RkStubResetVal[]) {
        { 0x2fe0, 0x00000030 }, { 0x2fe4, 0x00000013 },
        { 0x2fe8, 0x00000024 }, { 0x2fec, 0x00000000 },
        { 0x2ff0, 0x0000000d }, { 0x2ff4, 0x000000f0 },
        { 0x2ff8, 0x00000005 }, { 0x2ffc, 0x000000b1 },
      }, .n_reset_vals = 8 },
    /*
     * Stock-DTB (P12) round: the remaining enabled rk3588s DTB windows
     * that no model covers yet.  Plain storage so the mainline drivers
     * (CONFIG_ARM_SMMU_V3=y etc) read IDs as 0 and fail their probes
     * gracefully instead of taking an external abort that kills init.
     * Revisit per-device as needed (panthor/snd/thermal are =m and do
     * not probe during the initramfs boot).
     */
    { .name = "smmu-v3", .base = 0xfc900000, .size = 0x2000 },
    { .name = "gpu", .base = 0xfb000000, .size = 0x2000 },
    /*
     * NPU / video-codec / rkvdec: the rockchip IOMMU behavioral model
     * (rk3588-iommu.c, B2) is mapped at the iommu@ bases with overlap
     * priority 1 and wins those sub-ranges; these plain stubs cover the
     * surrounding device MMIO so probes fail gracefully.
     */
    { .name = "npu", .base = 0xfdab0000, .size = 0x30000 },
    { .name = "codecs", .base = 0xfdb50000, .size = 0x60000 },
    { .name = "vdec", .base = 0xfdc38000, .size = 0xd000 },
    { .name = "av1", .base = 0xfdc70000, .size = 0x1000 },
    /* av1-iommu (fdca0000) is verisilicon compat, not rk_iommu; leave
       as storage for graceful probe fail. */
    { .name = "av1-iommu", .base = 0xfdca0000, .size = 0x1000 },
    { .name = "vop-mmu-gap", .base = 0xfdd97000, .size = 0xe00 },
    { .name = "i2s-tdm0", .base = 0xfddf0000, .size = 0x1000 },
    { .name = "i2s-tdm2", .base = 0xfe480000, .size = 0x1000 },
    { .name = "tsadc", .base = 0xfec00000, .size = 0x1000 },
    { .name = "wdt", .base = 0xfeaf0000, .size = 0x100 },
    { .name = "dfi", .base = 0xfe060000, .size = 0x1000 },
    /*
     * QoS register files (qos@fdf3xxxx-fdf4xxxx / fdf6 / fdf7 / fdf8):
     * rockchip-pm-domain save/restore_qos() reads PRI/MODE/BW/SAT/EXT
     * (offsets 0x08..0x1c of each 0x20 window) on every domain power
     * transition, so the whole spans need to be mapped.
     */
    { .name = "qos-a", .base = 0xfdf35000, .size = 0xe000 },
    { .name = "qos-b", .base = 0xfdf60000, .size = 0x8000 },
    { .name = "qos-c", .base = 0xfdf70000, .size = 0x3000 },
    { .name = "qos-d", .base = 0xfdf80000, .size = 0x3000 },
};

void rk3588_create_stub_regions(MemoryRegion *sysmem)
{
    for (int i = 0; i < ARRAY_SIZE(rk_stub_blocks); i++) {
        const RkStubBlock *b = &rk_stub_blocks[i];
        MemoryRegion *mr = g_new(MemoryRegion, 1);
        RkStub *s = g_new0(RkStub, 1);

        s->regs = g_new0(uint32_t, b->size / 4);
        for (unsigned j = 0; j < b->n_reset_vals; j++) {
            s->regs[b->reset_vals[j].off / 4] = b->reset_vals[j].val;
        }
        if (b->reset_val) {
            s->regs[b->reset_off / 4] = b->reset_val;
        }
        s->hiword = b->hiword;
        s->plain_lo = b->plain_lo;
        s->plain_hi = b->plain_hi;
        s->track_engine = b->track_engine;
        s->read_hook = b->read_hook;
        s->write_hook = b->write_hook;
        if (b->track_engine) {
            s->tally = g_new0(uint8_t, b->size / 0x10000 + 1);
            memset(s->tally, 1, b->size / 0x10000 + 1);
            s->tally_n = b->size / 0x10000 + 1;
        }

        memory_region_init_io(mr, OBJECT(sysmem), &rk_stub_ops, s,
                              b->name, b->size);
        memory_region_add_subregion(sysmem, b->base, mr);
    }
}
