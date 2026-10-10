/*
 * RK3588 DDR training-blob register models.
 *
 * Everything here is model behaviour for the stock DDR TPL blob
 * (rk3588_ddr_lp4_2112MHz_lp5_2400MHz_v1.16.bin).  Values marked GUESS
 * are not from the TRM or hardware.  Values marked BOARD come from a
 * real 16 GB board's end-of-training register dump.
 */

#include "qemu/osdep.h"
#include "rk3588-ddr.h"

/*
 * DDR mode-register read port (blob helper ff004dbc, decompile name):
 * cmd at +0x80 (START, bit31 self-clears), MR number at +0x84 (bits 15:8),
 * data byte at +0x94.  Channel base 0xf7000000 + n*0x1000000.
 *
 * The TPL's device-identity check (ff00b8xx, decompile name) reads MR14
 * on channel 0 and requires 0x4d (LPDDR4) or 0x5d (LPDDR4X).
 * MR5 = 0xff is observed in logs ("Manufacturer ID:0xff", BOARD).
 * MR8 = 0x18 is a GUESS (density code 6; the exact code depends on the
 * board, so this is not a fixed truth).
 */
static bool ddr_state_fired;    /* +0x10014 read-count guess has fired */

static uint8_t ddr_mr_sel;      /* MR number of the pending command */
static uint8_t ddr_mr_data;     /* data byte written at +0x84 */
static uint8_t ddr_mr_store[256];
static bool ddr_mr_init;

static void ddr_mr_defaults(void)
{
    ddr_mr_store[5] = 0xff;     /* BOARD */
    ddr_mr_store[8] = 0x18;     /* GUESS */
    ddr_mr_store[14] = 0x5d;    /* required by the LPDDR4X identity check */
    ddr_mr_init = true;
}

void rk3588_ddr_probe_write(hwaddr off, uint64_t value)
{
    if (!ddr_mr_init) {
        ddr_mr_defaults();
    }
    switch (off & 0xfff) {
    case 0x80:
        /* START (bit31) with bit0 clear = MR write (blob FUN_ff003fcc) */
        if ((value & 0x80000000u) && !(value & 1)) {
            ddr_mr_store[ddr_mr_sel] = ddr_mr_data;
        }
        break;
    case 0x84:
        ddr_mr_sel = (value >> 8) & 0xff;
        ddr_mr_data = value & 0xff;
        break;
    default:
        break;
    }
}

/*
 * Channel offset helper: the blob addresses channel n's ctl block at
 * 0xf7000000 + n*0x1000000 (channel table DAT_ff015a30), so the same
 * register appears at off = n*0x1000000 + low for n = 0..3.
 */
static bool ddr_is_ch_off(hwaddr off, hwaddr low)
{
    return off >= low && off - low <= 3 * 0x1000000 &&
           (off - low) % 0x1000000 == 0;
}

uint64_t rk3588_ddr_probe_read(uint32_t stored, hwaddr off)
{
    if (!ddr_mr_init) {
        ddr_mr_defaults();
    }
    if ((off & 0xfff) == 0x94) {
        return ddr_mr_store[ddr_mr_sel];
    }
    /*
     * +0x10b88 status (blob poll at ff00a370, every channel).  BOARD:
     * bits 0x36000000 are set at end of training.  The blob waits for
     * them after the F1 frequency switch (ch1 at 0xf8010b88 confirmed
     * with gdb).
     */
    if (ddr_is_ch_off(off, 0x10b88)) {
        return stored | 0x36000000;
    }
    /*
     * +0x10014 training-state field (GUESS, read-counted).  Board trace
     * (DDR_STATE_LOG, rk/tools/ddr_trace_patch.py): last value at ff00a490
     * and ff00a4a4 was 0x2023; at ff00b828 and ff00787c it was 1.  The engine
     * (rk_stub_read) adds bit0 once +0xC80 is written.  Model: after
     * DDR_STATE_N reads return 0x2022 in addition, so the bit0 polls pass
     * first and the ff00a490 wait (&7 == 3, bits5:4 == 2) can then pass.
     * DDR_STATE_N is a guess; the first read (ff00c3f8, needs 0) is unaffected.
     */
    if (ddr_is_ch_off(off, 0x10014)) {
        static unsigned reads;
        if (++reads > 20) {
            /*
             * GUESS (state cycle): the poll at ff00a664 loops while &7==3,
             * i.e. the field must move on from 3, while later polls see 3
             * again (board A490 last = 0x2023) and 1 (board B828 = 1).
             * Model: alternate 20-read windows of state 3 (0x2022 + engine
             * bit0) and state 1 (0x2020 + engine bit0).  Any spinning poll
             * for either value passes within one window.  The window size
             * is a guess.
             */
            ddr_state_fired = true;
            return stored | (((reads - 1) / 20) & 1 ? 0x2020 : 0x2022);
        }
        return stored;
    }
    /*
     * +0x10014 training-state field: earlier note (superseded above).  Polls: ff00b828 wants
     * &7 == 1; ff00a490 wants &7 == 3 and ff00a4a4 wants bits5:4 == 2 (0x23
     * satisfies both); the board ends at 2.  A read-sequence model (1, 0x23,
     * 2) was tried and reordered the stall (reads come from an earlier
     * unidentified poll), so it was removed.
     */
    return stored;
}

bool rk3588_ddr_stage_advanced(void)
{
    return ddr_state_fired;
}

/*
 * DDR DQ test engine (0xfe050000), used by the v1.24 blob's DQ-read
 * training (search fn blob ff0012f4 -> test ff001180/ff001238).  Protocol:
 * address at +0x04/+0x08, size>>2 at +0x0c, mode at +0x2c, +0x58 = 7,
 * then poll +0x54 until nonzero; (v & 7) == 4 means the pattern test
 * passed, anything else is a fail code.  Unmapped, the first program
 * write data-aborts (vector 0x200) and training hangs.  GUESS: always
 * report a passed test.
 */
uint64_t rk3588_ddr_test_read(uint32_t stored, hwaddr off)
{
    if ((off & 0xfff) == 0x54) {
        return 4;
    }
    return stored;
}

/*
 * DDR PHY/ctl block (0xfe0c0000 + n*0x10000).
 */
uint64_t rk3588_ddr_cfg2_read(uint32_t stored, hwaddr off)
{
    switch (off & 0xfff) {
    case 0xa24: return stored | 0x2;   /* ff007470 poll, GUESS */
    case 0x684: return stored | 0x1;   /* ff006444 poll (after |=4), GUESS */

    /*
     * Eye-scan results (channel 0, scan base 0xfe0c0000).  BOARD values
     * at end of training: +0x68c/+0x690 = 0 (no error flags);
     * +0x69c = 0x00340033 (low 10 bits = left edge 0x33, bits 16-25 =
     * right edge 0x34); +0x6a0 = 0x00350037.  0x3ff in an edge field
     * means no window.  Other channels may differ.
     */
    case 0x68c: case 0x690: return 0;
    case 0x69c: return 0x00340033;
    case 0x6a0: return 0x00350037;
    default:    return stored;
    }
}
