/*
 * Rockchip IOMMU behavioral window model (see rk3588-iommu.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RK3588_IOMMU_H
#define HW_ARM_RK3588_IOMMU_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"

#define TYPE_RK3588_IOMMU "rk3588-iommu"
OBJECT_DECLARE_SIMPLE_TYPE(RkIommuState, RK3588_IOMMU);

void rk3588_create_iommu_window(uint64_t base);

#endif
