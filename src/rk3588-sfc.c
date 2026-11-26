/* RK3588 serial flash controller, PIO and simple DMA. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/ssi/ssi.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "rk3588-sfc.h"

OBJECT_DECLARE_SIMPLE_TYPE(RK3588SFC, RK3588_SFC)

struct RK3588SFC {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    SSIBus *bus;
    qemu_irq cs;
    uint32_t regs[0x110 / 4];
    uint32_t left;
    bool active;
    bool waiting_addr;
    bool write;
    bool present;
};

static uint8_t sfc_byte(RK3588SFC *s, uint8_t byte)
{
    return s->present ? ssi_transfer(s->bus, byte) : 0xff;
}

static void sfc_finish(RK3588SFC *s)
{
    if (s->active) {
        qemu_set_irq(s->cs, 1);
    }
    s->active = false;
    s->waiting_addr = false;
    s->left = 0;
    s->regs[0x28 / 4] |= 1u << 4;
}

static void sfc_address(RK3588SFC *s, uint32_t value)
{
    unsigned mode = (s->regs[0x100 / 4] >> 14) & 3;
    unsigned bytes = mode == 1 ? 3 : mode == 2 ? 4 :
                     mode == 3 ? (s->regs[0x18 / 4] + 1 + 7) / 8 : 0;
    unsigned dummy = (s->regs[0x100 / 4] >> 8) & 0xf;

    bytes = MIN(bytes, 4);
    while (bytes) {
        sfc_byte(s, value >> (--bytes * 8));
    }
    while (dummy >= 8) {
        sfc_byte(s, 0xff);
        dummy -= 8;
    }
    s->waiting_addr = false;
    if (!s->left) {
        sfc_finish(s);
    }
}

static uint64_t sfc_read(void *opaque, hwaddr off, unsigned size)
{
    RK3588SFC *s = opaque;
    uint32_t val = 0;

    switch (off) {
    case 0x10: return 0;
    case 0x20:
        return (1u << 1) | (s->active && !s->write && s->left ?
                            (MIN((s->left + 3) / 4, 31u) << 16) : (1u << 2)) |
               (31u << 8);
    case 0x24: return 0;
    case 0x2c: return 3;
    case 0x108:
        for (unsigned i = 0; i < 4 && s->left && s->active; i++) {
            val |= (uint32_t)sfc_byte(s, 0xff) << (i * 8);
            s->left--;
        }
        if (!s->left) {
            sfc_finish(s);
        }
        return val;
    default:
        return off < sizeof(s->regs) ? s->regs[off / 4] : 0;
    }
}

static void sfc_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    RK3588SFC *s = opaque;

    if (off >= sizeof(s->regs)) {
        return;
    }
    s->regs[off / 4] = value;
    switch (off) {
    case 0x08:
        s->regs[0x28 / 4] &= ~value;
        break;
    case 0x10:
        sfc_finish(s);
        break;
    case 0x100:
        sfc_finish(s);
        s->left = (value >> 16) & 0x3fff;
        s->write = value & (1u << 12);
        s->waiting_addr = (value >> 14) & 3;
        s->active = true;
        qemu_set_irq(s->cs, 0);
        sfc_byte(s, value);
        if (!s->waiting_addr) {
            sfc_address(s, 0);
        }
        break;
    case 0x104:
        if (s->active && s->waiting_addr) {
            sfc_address(s, value);
        }
        break;
    case 0x108:
        for (unsigned i = 0; i < 4 && s->left && s->active; i++) {
            sfc_byte(s, value >> (i * 8));
            s->left--;
        }
        if (!s->left) {
            sfc_finish(s);
        }
        break;
    case 0x80:
        if ((value & 1) && s->active && !s->waiting_addr) {
            uint32_t addr = s->regs[0x84 / 4];
            while (s->left) {
                uint8_t byte;
                if (s->write) {
                    dma_memory_read(&address_space_memory, addr, &byte, 1,
                                    MEMTXATTRS_UNSPECIFIED);
                    sfc_byte(s, byte);
                } else {
                    byte = sfc_byte(s, 0xff);
                    dma_memory_write(&address_space_memory, addr, &byte, 1,
                                     MEMTXATTRS_UNSPECIFIED);
                }
                s->left--;
                addr++;
            }
            sfc_finish(s);
            s->regs[0x28 / 4] |= 1u << 7;
        }
        break;
    }
}

static const MemoryRegionOps sfc_ops = {
    .read = sfc_read,
    .write = sfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sfc_reset(DeviceState *dev)
{
    RK3588SFC *s = RK3588_SFC(dev);

    sfc_finish(s);
    memset(s->regs, 0, sizeof(s->regs));
}

static void sfc_init(Object *obj)
{
    RK3588SFC *s = RK3588_SFC(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sfc_ops, s, TYPE_RK3588_SFC, 0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->bus = ssi_create_bus(dev, "sfc-spi");
    qdev_init_gpio_out_named(dev, &s->cs, "cs", 1);
}

void rk3588_sfc_set_present(DeviceState *dev)
{
    RK3588_SFC(dev)->present = true;
}

static void sfc_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), sfc_reset);
}

static const TypeInfo sfc_type = {
    .name = TYPE_RK3588_SFC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588SFC),
    .instance_init = sfc_init,
    .class_init = sfc_class_init,
};

static void sfc_register_types(void)
{
    type_register_static(&sfc_type);
}
type_init(sfc_register_types)
