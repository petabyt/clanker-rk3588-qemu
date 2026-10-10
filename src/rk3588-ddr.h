#ifndef RK3588_DDR_H
#define RK3588_DDR_H

#include "system/memory.h"

/*
 * DDR training-blob register models for the RK3588 TPL (see
 * src/rk3588-ddr.c).  Read/write hooks are plugged into the stub regions
 * defined in rk3588-stubs.c.
 */

/* ddr-probe block, base 0xf7000000: MR port and training status polls. */
uint64_t rk3588_ddr_probe_read(uint32_t stored, hwaddr off);
void rk3588_ddr_probe_write(hwaddr off, uint64_t value);

/* ddr-cfg2 block, base 0xfe0c0000: PHY poll bits and eye-scan results. */
uint64_t rk3588_ddr_cfg2_read(uint32_t stored, hwaddr off);

/* ddr-test-engine block, base 0xfe050000 (v1.24 blob): DQ test status. */
uint64_t rk3588_ddr_test_read(uint32_t stored, hwaddr off);

/* true once the +0x10014 read-count guess (F1 stage) has fired (GUESS) */
bool rk3588_ddr_stage_advanced(void);

#endif
