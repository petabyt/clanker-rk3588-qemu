#ifndef RK3588_DWMSHC_H
#define RK3588_DWMSHC_H

#include "hw/sd/sd.h"

/*
 * RK3588 SD slot: Synopsys DesignWare mobile storage host ("dw-mshc",
 * compatible rockchip,rk3588-dw-mshc, sdmmc@fe2c0000 in the image DTB).
 * Serves commands/responses from the SD card on its sd-bus, with PIO
 * (FIFO at 0x200) and internal-DMA (IDMAC, 32-bit descriptors) data paths.
 * Polled only; the IRQ line is not wired on this machine (see rk3588.c).
 */
#define TYPE_RK3588_DWMSHC "rk3588-dwmshc"
#define RK3588_SDMMC_BASE  0xFE2C0000ULL
#define RK3588_SDMMC_SIZE  0x4000

/*
 * Card-present state of the SD slot, from the bus insertion callback.
 * The GPIO0 stub reads PA4 (card detect, active low) from this.
 */
extern bool rk3588_sd_card_inserted;

#endif
