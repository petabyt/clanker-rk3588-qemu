/* Host unit test for RK3588 v2 descriptor decoding and permission checks. */
#include <assert.h>
#include <stdio.h>
#include "../src/rk3588-iommu-walk.h"

struct table {
    uint64_t dte_addr, pte_addr;
    uint32_t dte, pte;
};

static bool read32(void *opaque, uint64_t addr, uint32_t *value)
{
    struct table *t = opaque;
    if (addr == t->dte_addr) {
        *value = t->dte;
        return true;
    }
    if (addr == t->pte_addr) {
        *value = t->pte;
        return true;
    }
    return false;
}

int main(void)
{
    uint32_t iova = 0x80403abc;
    uint32_t root = 0x12345a20; /* table at 0x2a12345000 */
    struct table t = {
        .dte_addr = UINT64_C(0x2a12345000) + (iova >> 22) * 4,
        .pte_addr = UINT64_C(0x3b23456000) + ((iova >> 12) & 0x3ff) * 4,
        .dte = 0x23456b31,
        .pte = 0x34567c47,
    };
    uint64_t phys = 0;

    assert(rk_iommu_v2_walk(root, iova, false, read32, &t, &phys));
    assert(phys == UINT64_C(0x4c34567abc));
    assert(rk_iommu_v2_walk(root, iova, true, read32, &t, &phys));
    t.pte &= ~4u;
    assert(!rk_iommu_v2_walk(root, iova, true, read32, &t, &phys));
    assert(rk_iommu_v2_walk(root, iova, false, read32, &t, &phys));
    t.pte &= ~2u;
    assert(!rk_iommu_v2_walk(root, iova, false, read32, &t, &phys));
    t.pte = 0x34567c46;
    assert(!rk_iommu_v2_walk(root, iova, true, read32, &t, &phys));
    t.pte = 0x34567c47;
    t.dte &= ~1u;
    assert(!rk_iommu_v2_walk(root, iova, false, read32, &t, &phys));
    t.dte |= 1;
    t.pte_addr += 4; /* physical table read fails */
    assert(!rk_iommu_v2_walk(root, iova, false, read32, &t, &phys));
    puts("PASS: v2 IOMMU walk, 40-bit addresses, validity and permissions");
    return 0;
}
