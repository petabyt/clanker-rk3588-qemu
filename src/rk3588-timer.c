/*
 * Rockchip rk3288-style timer, see rk3588-timer.h for the layout.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "hw/core/qdev-properties.h"
#include "rk3588-timer.h"

#define RK3588_TIMER_REGS 0x20

#define REG_LOAD_COUNT 0x00
#define REG_CURRENT_VALUE 0x08
#define REG_CONTROL 0x10
#define REG_INT_STATUS 0x18

#define CTRL_ENABLE 0x1
#define CTRL_MODE_USER_DEFINED 0x2 /* one-shot; 0 = free-running wrap */
#define CTRL_INT_UNMASK 0x4

OBJECT_DECLARE_SIMPLE_TYPE(RK3588TimerState, RK3588_TIMER)

struct RK3588TimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t load;
    uint32_t ctrl;
    uint32_t int_status;
    uint32_t cur_frozen; /* value latched when disabled */
    bool enabled;
    uint64_t start_ns;
    uint32_t freq_hz;
};

/* Down-count since enable, wrap-aware for free-running mode.  Expires
 * (returns load, i.e. "elapsed == 0") never below zero: the one-shot
 * sticks at 0 with INT_STATUS latched. */
static uint32_t rk3588_timer_current(RK3588TimerState *s)
{
    uint64_t now, elapsed, span, rem;

    if (!s->enabled) {
        return s->cur_frozen;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    elapsed = muldiv64(now - s->start_ns, s->freq_hz, 1000000000ULL);
    span = (uint64_t)s->load + 1;
    if ((s->ctrl & CTRL_MODE_USER_DEFINED) && elapsed >= s->load + 1) {
        /* One-shot expired: latch pending + drive the IRQ line. */
        s->int_status |= 1;
        qemu_set_irq(s->irq, (s->ctrl & CTRL_INT_UNMASK) && (s->int_status & 1));
        return 0;
    }
    rem = elapsed % span;
    return s->load - (uint32_t)rem;
}

static void rk3588_timer_update_irq(RK3588TimerState *s)
{
    qemu_set_irq(s->irq, (s->ctrl & CTRL_INT_UNMASK) && (s->int_status & 1));
}

static uint64_t rk3588_timer_read(void *opaque, hwaddr off, unsigned size)
{
    RK3588TimerState *s = opaque;

    switch (off) {
    case REG_LOAD_COUNT:
        return s->load;
    case REG_CURRENT_VALUE:
        return rk3588_timer_current(s);
    case REG_CONTROL:
        return s->ctrl;
    case REG_INT_STATUS:
        return s->int_status;
    default:
        return 0;
    }
}

static void rk3588_timer_write(void *opaque, hwaddr off, uint64_t value,
                              unsigned size)
{
    RK3588TimerState *s = opaque;

    switch (off) {
    case REG_LOAD_COUNT:
        s->load = value;
        /* Restart the count from the new load if running. */
        if (s->enabled) {
            s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        break;
    case REG_CURRENT_VALUE:
        /* Read-only on hardware. */
        break;
    case REG_CONTROL:
        if (value & CTRL_ENABLE) {
            if (!s->enabled) {
                s->enabled = true;
                s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                if (s->load == 0) {
                    s->load = 0xffffffff;
                }
            }
        } else if (s->enabled) {
            s->cur_frozen = rk3588_timer_current(s);
            s->enabled = false;
        }
        s->ctrl = value & (CTRL_ENABLE | CTRL_MODE_USER_DEFINED |
                           CTRL_INT_UNMASK);
        rk3588_timer_update_irq(s);
        break;
    case REG_INT_STATUS: /* write-1-to-clear */
        s->int_status &= ~value;
        rk3588_timer_update_irq(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps rk3588_timer_ops = {
    .read = rk3588_timer_read,
    .write = rk3588_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rk3588_timer_init(Object *obj)
{
    RK3588TimerState *s = RK3588_TIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rk3588_timer_ops, s,
                          TYPE_RK3588_TIMER, RK3588_TIMER_REGS);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const Property rk3588_timer_properties[] = {
    DEFINE_PROP_UINT32("clock-freq", RK3588TimerState, freq_hz, 24000000),
};

static void rk3588_timer_reset_hold(Object *obj, ResetType type)
{
    RK3588TimerState *s = RK3588_TIMER(obj);

    s->load = 0;
    s->ctrl = 0;
    s->int_status = 0;
    s->cur_frozen = 0;
    s->enabled = false;
    s->start_ns = 0;
}

static void rk3588_timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    rc->phases.hold = rk3588_timer_reset_hold;
    device_class_set_props(dc, rk3588_timer_properties);
    dc->desc = "Rockchip rk3288-style down-counting timer";
}

static const TypeInfo rk3588_timer_types[] = {
    {
        .name = TYPE_RK3588_TIMER,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RK3588TimerState),
        .instance_init = rk3588_timer_init,
        .class_init = rk3588_timer_class_init,
    },
};

DEFINE_TYPES(rk3588_timer_types)