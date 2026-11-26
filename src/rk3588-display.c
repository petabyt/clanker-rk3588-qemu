/*
 * VOP2 scanout -> ramfb-style display surface (plan.md P9), plus the
 * console-order policy (2026-10-21):
 *
 * The firmware (rk/src/rk3588/vop2.c config_smart_layer()) programs the
 * ESmart2 layer at VOP2 base + 0x1c00: master enable in region0_mst_ctl,
 * framebuffer address in region0_mst_yrgb, stride (pixels) in
 * region0_vir, geometry in region0_act_info.  This module watches those
 * registers on each gfx_update and maps the guest framebuffer into a
 * DisplaySurface exactly like hw/display/ramfb.c does, so the window
 * (and monitor screendumps) show guest RAM directly.  Linux fbcon uses
 * the Cluster0 window regs instead - see rk_display_get_window().
 *
 * Console order (why this is a device): the window/VNC default view is
 * always *console 0* = the first console created, and GTK pages follow
 * creation order.  CLI -device realize (vl.c) happens after machine
 * init, so a console created eagerly at machine init would shadow the
 * virtio-gpu head forever.  Therefore this display is a tiny machine-
 * internal sysbus device ("rk3588-display", id "hdmi") that registers
 * its console from qemu_add_machine_init_done_notifier() ->
 * qdev_machine_creation_done() runs at vl.c *after* -device realize:
 *
 *   vgpu=on :  console 0 = virtio-gpu head (window/VNC default = vgpu),
 *              console 1 = VOP2/HDMI, addressable as
 *              `screendump <file> hdmi` (qdev_find_recursive matches
 *              the device id - same trick hw/arm/npcm8xx.c uses).
 *   vgpu off:  HDMI is the only console, i.e. index 0 - the plain
 *              `screendump` used by p9/p17 exits keeps working.
 *
 * What each console shows: QEMU seeds every new graphic console with a
 * "Guest has not initialized the display (yet)." placeholder, so until
 * something programs VOP2 you see that message - not garbage.  After a
 * guest DRM modeset the surface maps the guest framebuffer verbatim;
 * uninitialised RAM therefore *looks* like garbage until fbcon
 * actually draws (correct emulation, not a model bug).
 */
#include "qemu/osdep.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev.h"
#include "qom/object.h"
#include "qemu/notify.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "system/physmem.h"
#include "qapi/error.h"
#include "rk3588-display.h"

#define VOP2_BASE                   0xfdd90000ull
#define VOP2_ESMART2                (VOP2_BASE + 0x1c00)
#define ESMART2_REGION0_MST_CTL     0x10
#define ESMART2_REGION0_MST_YRGB    0x14
#define ESMART2_REGION0_VIR         0x1c
#define ESMART2_REGION0_ACT_INFO    0x24

/*
 * Linux fbcon on rk3588: vop2_create_crtcs() picks Cluster0-win0 as
 * the primary plane (rockchip_vop2_reg.c: first DRM_PLANE_TYPE_PRIMARY
 * window), so the kernel's framebuffer sits in the Cluster0 window
 * regs (VOP2_WIN_* fields of rockchip_drm_vop2.c vop2_plane_atomic_update):
 * WIN_CTRL0 @0x1000 (enable bit0, format [5:1] — 0 = XRGB8888,
 * rb_swap bit14), CLUSTER_CTRL @0x1100 (bit0 cluster enable),
 * YRGB_MST @0x1010, WIN_VIR @0x1018 (yrgb stride in 4-byte words),
 * ACT_INFO @0x1020, DSP_INFO @0x1024, DSP_ST @0x1028.
 */
#define VOP2_CLUSTER0               (VOP2_BASE + 0x1000)
#define CLUSTER0_WIN_CTRL0          0x00
#define CLUSTER0_YRGB_MST           0x10
#define CLUSTER0_WIN_VIR            0x18
#define CLUSTER0_ACT_INFO           0x20
#define CLUSTER0_CLUSTER_CTRL       0x100

OBJECT_DECLARE_SIMPLE_TYPE(RkDisplay, RK3588_DISPLAY)

struct RkDisplay {
    SysBusDevice parent_obj;
    Notifier done;              /* machine-init-done -> create console */
    QemuConsole *con;
    bool surface_ready;
    bool indexed_surface;
    hwaddr fb_addr;
    uint64_t fb_stride;
    uint32_t width, height;
    uint8_t *indexed_pixels;
    size_t indexed_size;
    uint32_t indexed_palette[256];
    uint32_t indexed_sw, indexed_sh, indexed_dw, indexed_dh, indexed_ox, indexed_oy;
    uint8_t *rgb_snapshot;
    size_t rgb_snapshot_size;
};

static void rk_display_unmap(pixman_image_t *image, void *unused)
{
    void *data = pixman_image_get_data(image);
    uint32_t size = pixman_image_get_stride(image) *
                    pixman_image_get_height(image);

    physical_memory_unmap(data, size, 0, 0);
}

/*
 * Pick the active scanout window: the kernel's Cluster0 primary plane
 * first, the rk firmware's ESmart2 layer (P9) as fallback.  Both are
 * XRGB8888 (VOP2_FMT_ARGB8888, no rb_swap) — any other format keeps the
 * placeholder surface.
 */
static bool rk_display_get_window(uint32_t *ctlp, uint32_t *addrp,
                                  uint32_t *virp, uint32_t *actp)
{
    uint32_t ctrl0, cluster;

    physical_memory_read(VOP2_CLUSTER0 + CLUSTER0_WIN_CTRL0, &ctrl0, 4);
    physical_memory_read(VOP2_CLUSTER0 + CLUSTER0_CLUSTER_CTRL, &cluster, 4);
    if ((ctrl0 & 1) && (cluster & 1) && ((ctrl0 >> 1) & 0x1f) == 0 &&
        !(ctrl0 & (1 << 14))) {
        *ctlp = ctrl0;
        physical_memory_read(VOP2_CLUSTER0 + CLUSTER0_YRGB_MST, addrp, 4);
        physical_memory_read(VOP2_CLUSTER0 + CLUSTER0_WIN_VIR, virp, 4);
        physical_memory_read(VOP2_CLUSTER0 + CLUSTER0_ACT_INFO, actp, 4);
        return true;
    }

    physical_memory_read(VOP2_ESMART2 + ESMART2_REGION0_MST_CTL, ctlp, 4);
    physical_memory_read(VOP2_ESMART2 + ESMART2_REGION0_MST_YRGB, addrp, 4);
    physical_memory_read(VOP2_ESMART2 + ESMART2_REGION0_VIR, virp, 4);
    physical_memory_read(VOP2_ESMART2 + ESMART2_REGION0_ACT_INFO, actp, 4);
    return !!(*ctlp & 1);
}

/* ESmart2 BPP08, VP2 and nearest-neighbor scaling as used by the SRAM demo.
 * The palette's high byte and writes while lookup is enabled remain unknown. */
static bool rk_display_indexed(RkDisplay *s, uint32_t addr, uint32_t vir)
{
    uint32_t act, dsp, pos, hact, vact, lutsel, ctrl, scale;
    uint32_t palette[256];
    uint32_t sw, sh, dw, dh, ox, oy, width, height;
    hwaddr stride, mapsize, requested;
    uint8_t *pixels;
    DisplaySurface *ds;

    physical_memory_read(VOP2_BASE + 0x58, &lutsel, 4);
    physical_memory_read(VOP2_ESMART2, &ctrl, 4);
    if (!(lutsel & (1 << 4)) || ((lutsel >> 5) & 7) != 2 ||
        !(ctrl & (1 << 4))) {
        return false;
    }
    physical_memory_read(VOP2_ESMART2 + 0x20, &act, 4);
    physical_memory_read(VOP2_ESMART2 + 0x24, &dsp, 4);
    physical_memory_read(VOP2_ESMART2 + 0x28, &pos, 4);
    physical_memory_read(VOP2_ESMART2 + 0x30, &scale, 4);
    physical_memory_read(VOP2_BASE + 0xe4c, &hact, 4);
    physical_memory_read(VOP2_BASE + 0xe54, &vact, 4);
    sw = (act & 0xffff) + 1;
    sh = (act >> 16) + 1;
    dw = (dsp & 0xffff) + 1;
    dh = (dsp >> 16) + 1;
    ox = pos & 0xffff;
    oy = pos >> 16;
    width = (hact & 0xffff) - (hact >> 16);
    height = (vact & 0xffff) - (vact >> 16);
    stride = (hwaddr)vir * 4;
    if (!width || width > 4096 || !height || height > 4096 ||
        sw > 4096 || sh > 4096 || dw > 4096 || dh > 4096 ||
        stride < sw || stride > 16384 || ox >= width || oy >= height ||
        (sw != dw && (scale & 3) != 1) ||
        (sh != dh && ((scale >> 4) & 3) != 1)) {
        return false;
    }
    requested = stride * (sh - 1) + sw;
    mapsize = requested;
    pixels = physical_memory_map(addr, &mapsize, false);
    if (!pixels || mapsize != requested) {
        if (pixels) {
            physical_memory_unmap(pixels, mapsize, false, 0);
        }
        return false;
    }
    physical_memory_read(VOP2_BASE + 0x6000, palette, sizeof(palette));
    /* The VOP timer calls us every virtual frame, even for a static menu.
     * Avoid rescaling millions of pixels unless scanout or the LUT changed. */
    if (s->surface_ready && s->indexed_surface && s->width == width &&
        s->height == height && s->fb_addr == addr && s->fb_stride == stride &&
        s->indexed_sw == sw && s->indexed_sh == sh && s->indexed_dw == dw &&
        s->indexed_dh == dh && s->indexed_ox == ox && s->indexed_oy == oy &&
        s->indexed_size == requested && s->indexed_pixels &&
        !memcmp(s->indexed_palette, palette, sizeof(palette)) &&
        !memcmp(s->indexed_pixels, pixels, requested)) {
        physical_memory_unmap(pixels, mapsize, false, 0);
        return true;
    }
    if (!s->surface_ready || !s->indexed_surface || s->width != width ||
        s->height != height) {
        ds = qemu_create_displaysurface(width, height);
        qemu_console_set_surface(s->con, ds);
        s->surface_ready = true;
        s->indexed_surface = true;
        s->width = width;
        s->height = height;
    }
    if (s->indexed_size != requested) {
        s->indexed_pixels = g_realloc(s->indexed_pixels, requested);
        s->indexed_size = requested;
    }
    /* Render the snapshot we compare on the next frame, not live guest RAM. */
    memcpy(s->indexed_pixels, pixels, requested);
    ds = qemu_console_surface(s->con);
    memset(surface_data(ds), 0, surface_stride(ds) * height);
    for (uint32_t y = 0; y < MIN(dh, height - oy); y++) {
        uint32_t *dst = (uint32_t *)(surface_data(ds) +
                                     (oy + y) * surface_stride(ds));
        const uint8_t *row = s->indexed_pixels + (uint64_t)y * sh / dh * stride;

        for (uint32_t x = 0; x < MIN(dw, width - ox); x++) {
            dst[ox + x] = le32_to_cpu(palette[row[(uint64_t)x * sw / dw]]) &
                          0xffffff;
        }
    }
    memcpy(s->indexed_palette, palette, sizeof(palette));
    s->fb_addr = addr;
    s->fb_stride = stride;
    s->indexed_sw = sw;
    s->indexed_sh = sh;
    s->indexed_dw = dw;
    s->indexed_dh = dh;
    s->indexed_ox = ox;
    s->indexed_oy = oy;
    physical_memory_unmap(pixels, mapsize, false, 0);
    qemu_console_update_full(s->con);
    return true;
}

static bool rk_display_gfx_update(void *opaque)
{
    RkDisplay *s = opaque;
    uint32_t ctl, vir, act, addr;
    uint32_t width, height;
    hwaddr stride, linesize, mapsize, size;
    DisplaySurface *ds;
    void *data;

    if (!s->con) {
        return true;            /* console not created yet (pre-notifier) */
    }
    if (!rk_display_get_window(&ctl, &addr, &vir, &act)) {
        return true;
    }

    /* layer not programmed yet: keep the placeholder surface.  A
     * zero fb address is LEGAL on rk3588: DRAM is based at 0x0 and
     * the kernel's CMA places the fbcon framebuffer at the very
     * bottom (dri state: obj start=00000000, YRGB_MST=0). */
    if (!(ctl & 1) || vir == 0) {
        return true;
    }
    if (((ctl >> 1) & 0x1f) == 0x10) {
        rk_display_indexed(s, addr, vir);
        return true;
    }
    if (((ctl >> 1) & 0x1f) != 0) {
        return true;
    }
    width = (act & 0xffff) + 1;
    height = (act >> 16) + 1;
    if (width < 16 || width > 4096 || height < 16 || height > 4096 ||
        vir < width) {
        return true;
    }
    stride = (hwaddr)vir * 4;

    /* same scanout config: guest RAM backs the surface, nothing to do */
    if (s->surface_ready && !s->indexed_surface && addr == s->fb_addr && width == s->width &&
        height == s->height && stride == s->fb_stride) {
        uint8_t *pixels = surface_data(qemu_console_surface(s->con));
        size_t row_size = (size_t)width * 4;
        uint32_t first = height, last = 0;

        /* The mapped surface follows guest RAM, but notifying the frontend of
         * a full 1080p frame every tick is expensive for a mostly static UI. */
        for (uint32_t y = 0; y < height; y++) {
            uint8_t *src = pixels + y * stride;
            uint8_t *old = s->rgb_snapshot + y * row_size;

            if (memcmp(old, src, row_size)) {
                memcpy(old, src, row_size);
                first = MIN(first, y);
                last = y;
            }
        }
        if (first < height) {
            qemu_console_update(s->con, 0, first, width, last - first + 1);
        }
        return true;
    }

    linesize = (hwaddr)width * 4;
    mapsize = stride * (height - 1) + linesize;
    size = mapsize;
    data = physical_memory_map(addr, &mapsize, false);
    if (!data) {
        return true;
    }
    if (mapsize != size) {
        physical_memory_unmap(data, mapsize, 0, 0);
        return true;
    }
    ds = qemu_create_displaysurface_from(width, height, PIXMAN_x8r8g8b8,
                                          stride, data);
    if (!ds) {
        physical_memory_unmap(data, size, 0, 0);
        return true;
    }
    pixman_image_set_destroy_function(ds->image, rk_display_unmap, NULL);
    qemu_console_set_surface(s->con, ds);   /* frees the old surface */

    s->surface_ready = true;
    s->indexed_surface = false;
    s->fb_addr = addr;
    s->fb_stride = stride;
    s->width = width;
    s->height = height;
    s->rgb_snapshot_size = (size_t)width * height * 4;
    s->rgb_snapshot = g_realloc(s->rgb_snapshot, s->rgb_snapshot_size);
    for (uint32_t y = 0; y < height; y++) {
        memcpy(s->rgb_snapshot + (size_t)y * width * 4,
               (uint8_t *)data + y * stride, (size_t)width * 4);
    }
    qemu_console_update_full(s->con);
    return true;
}

static const GraphicHwOps rk_display_ops = {
    .gfx_update = rk_display_gfx_update,
};

static RkDisplay *rk_display_state;

/*
 * Machine-init-done: runs at qdev_machine_creation_done() (vl.c), i.e.
 * AFTER -device realize, so a virtio-gpu head realized from the
 * command line already owns console 0 and this one is next - see the
 * header comment for the full ordering rationale.
 */
static void rk_display_console_add(Notifier *notifier, void *unused)
{
    RkDisplay *s = container_of(notifier, RkDisplay, done);

    s->con = qemu_graphic_console_create(DEVICE(s), 0, &rk_display_ops, s);
}

void rk3588_display_repaint(void)
{
    if (rk_display_state && rk_display_state->con) {
        rk_display_gfx_update(rk_display_state);
    }
}

static void rk3588_display_realize(DeviceState *dev, Error **errp)
{
    RkDisplay *s = RK3588_DISPLAY(dev);

    s->done.notify = rk_display_console_add;
    qemu_add_machine_init_done_notifier(&s->done);
}

static void rk3588_display_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rk3588_display_realize;
    dc->desc = "RK3588 VOP2 -> graphic console (machine internal)";
    dc->user_creatable = false;
}

static void rk3588_display_finalize(Object *obj)
{
    RkDisplay *s = RK3588_DISPLAY(obj);

    g_free(s->indexed_pixels);
    g_free(s->rgb_snapshot);
}

static const TypeInfo rk3588_display_types[] = {
    {
        .name = TYPE_RK3588_DISPLAY,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RkDisplay),
        .instance_finalize = rk3588_display_finalize,
        .class_init = rk3588_display_class_init,
    },
};

DEFINE_TYPES(rk3588_display_types)

void rk3588_display_init(MachineState *ms)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_DISPLAY);

    (void)ms;
    /*
     * The id is what `screendump <file> hdmi` resolves the console by
     * (qdev_find_recursive walks the default sysbus); assign it before
     * realize, the way hw/arm/npcm8xx.c and hw/arm/virt.c do.
     */
    DEVICE(dev)->id = g_strdup("hdmi");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    rk_display_state = RK3588_DISPLAY(dev);
}
