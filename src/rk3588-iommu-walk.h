/* Rockchip v2 IOMMU descriptor walk, shared by the device and unit test. */
#ifndef RK3588_IOMMU_WALK_H
#define RK3588_IOMMU_WALK_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef bool (*RkIommuRead32)(void *opaque, uint64_t addr, uint32_t *value);
typedef int (*RkIommuTransfer)(void *opaque, uint64_t addr, void *buf,
                               size_t len, bool write);

static inline uint64_t rk_iommu_v2_address(uint32_t entry)
{
    return (entry & UINT64_C(0xfffff000)) |
           ((entry & UINT64_C(0xf00)) << 24) |
           ((entry & UINT64_C(0xf0)) << 32);
}

/* Translate one page, enforcing guest PTE access permissions. */
static inline bool rk_iommu_v2_walk(uint32_t root, uint32_t iova, bool write,
                                    RkIommuRead32 read32, void *opaque,
                                    uint64_t *physical)
{
    uint32_t dte, pte;
    uint64_t dte_addr = rk_iommu_v2_address(root) + ((iova >> 22) * 4);

    if (!read32(opaque, dte_addr, &dte) || !(dte & 1)) {
        return false;
    }
    if (!read32(opaque, rk_iommu_v2_address(dte) +
                        (((iova >> 12) & 0x3ff) * 4), &pte) ||
        !(pte & 1) || !(pte & (write ? 4 : 2))) {
        return false;
    }
    *physical = rk_iommu_v2_address(pte) | (iova & 0xfff);
    return true;
}

/* Return the transfer callback's error, or fault_result for translation,
 * range or stall failures. Earlier chunks may already have transferred. */
static inline int rk_iommu_v2_transfer(uint32_t root, bool paging, bool stall,
                                       uint32_t iova, void *buf, size_t len,
                                       bool write, RkIommuRead32 read32,
                                       RkIommuTransfer transfer, void *opaque,
                                       int fault_result)
{
    uint8_t *bytes = buf;

    if ((uint64_t)len > UINT64_C(0x100000000) - iova) {
        return fault_result;
    }
    while (len) {
        uint64_t phys = iova;
        size_t chunk = 0x1000 - (iova & 0xfff);
        int result;

        if (chunk > len) {
            chunk = len;
        }
        if (stall || (paging && !rk_iommu_v2_walk(root, iova, write, read32,
                                                 opaque, &phys))) {
            return fault_result;
        }
        result = transfer(opaque, phys, bytes, chunk, write);
        if (result) {
            return result;
        }
        iova += chunk;
        bytes += chunk;
        len -= chunk;
    }
    return 0;
}

#endif
