#ifndef RK3588_SDHCI_H
#define RK3588_SDHCI_H

#include "hw/core/boards.h"

/*
 * RK3588 eMMC host (sdhci@fe2e0000, "rockchip,rk3588-dwcmshc"): QEMU's
 * generic SDHCI + vendor-area bits; see src/rk3588-sdhci.c.  The card
 * is a QEMU "emmc" device from -drive if=sd unit 1.
 */
void rk3588_create_emmc(DeviceState *gic);

#endif
