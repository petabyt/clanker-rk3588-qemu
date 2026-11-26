/* Exercise the DMA transfer loop used by the RK3588 IOMMU model. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/rk3588-iommu-walk.h"

struct memory {
    uint32_t dte, pte[2];
    uint8_t page[2][4096];
    unsigned transfers;
};

static bool read32(void *opaque, uint64_t addr, uint32_t *out)
{
    struct memory *m = opaque;
    if (addr == 0x1000) {
        *out = m->dte;
        return true;
    }
    if (addr == 0x2000 || addr == 0x2004) {
        *out = m->pte[(addr - 0x2000) / 4];
        return true;
    }
    return false;
}

static int transfer(void *opaque, uint64_t addr, void *buf, size_t len, bool write)
{
    struct memory *m = opaque;
    int n = addr >= 0x4000 && addr < 0x5000 ? 0 :
            addr >= 0x9000 && addr < 0xa000 ? 1 : -1;
    size_t off = addr & 0xfff;

    if (n < 0 || len > 4096 - off) {
        return 7;
    }
    m->transfers++;
    if (write) {
        memcpy(m->page[n] + off, buf, len);
    } else {
        memcpy(buf, m->page[n] + off, len);
    }
    return 0;
}

int main(void)
{
    struct memory m = { .dte = 0x2001, .pte = {0x4007, 0x9007} };
    uint8_t in[16], out[16] = { 0 };

    for (int i = 0; i < 16; i++) {
        in[i] = i + 1;
    }
#define DMA(addr, buf, len, wr) \
    rk_iommu_v2_transfer(0x1000, true, false, addr, buf, len, wr, \
                         read32, transfer, &m, 1)
    assert(DMA(0xff8, in, sizeof(in), true) == 0);
    assert(m.transfers == 2);
    assert(memcmp(m.page[0] + 0xff8, in, 8) == 0);
    assert(memcmp(m.page[1], in + 8, 8) == 0);
    assert(DMA(0xff8, out, sizeof(out), false) == 0);
    assert(memcmp(in, out, sizeof(in)) == 0);
    m.pte[1] &= ~4u;
    memset(m.page[1], 0, 8);
    assert(DMA(0xff8, in, sizeof(in), true) == 1);
    assert(memcmp(m.page[1], (uint8_t[8]){0}, 8) == 0);
    assert(DMA(0xfffffff8, in, sizeof(in), true) == 1);
    assert(rk_iommu_v2_transfer(0, false, true, 0x4000, in, 8, true,
                                read32, transfer, &m, 1) == 1);
    assert(rk_iommu_v2_transfer(0, false, false, 0x4000, in, 8, true,
                                read32, transfer, &m, 1) == 0);
    assert(rk_iommu_v2_transfer(0, false, false, 0x6000, in, 8, true,
                                read32, transfer, &m, 1) == 7);
    puts("PASS: IOMMU DMA transfer read/write, permissions, bounds and bypass");
    return 0;
}
