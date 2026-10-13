/*
 * VOP2 scanout -> ramfb-style display surface (plan.md P9).
 *
 * The firmware (rk/src/rk3588/vop2.c config_smart_layer()) programs the
 * ESmart2 layer at VOP2 base + 0x1c00: master enable in region0_mst_ctl,
 * framebuffer address in region0_mst_yrgb, stride (pixels) in
 * region0_vir, geometry in region0_act_info.  This module watches those
 * registers on each gfx_update and maps the guest framebuffer into a
 * DisplaySurface exactly like hw/display/ramfb.c does, so the window
 * (and monitor screendumps) show guest RAM directly.
 */
#include "qemu/osdep.h"
#include "hw/core/boards.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "system/physmem.h"
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

typedef struct RkDisplay {
    QemuConsole *con;
    bool surface_ready;
    hwaddr fb_addr;
    uint64_t fb_stride;
    uint32_t width, height;
} RkDisplay;

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

static bool rk_display_gfx_update(void *opaque)
{
    RkDisplay *s = opaque;
    uint32_t ctl, vir, act, addr;
    uint32_t width, height;
    hwaddr stride, linesize, mapsize, size;
    DisplaySurface *ds;
    void *data;

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
    width = (act & 0xffff) + 1;
    height = (act >> 16) + 1;
    if (width < 16 || width > 4096 || height < 16 || height > 4096 ||
        vir < width) {
        return true;
    }
    stride = (hwaddr)vir * 4;

    /* same scanout config: guest RAM backs the surface, nothing to do */
    if (s->surface_ready && addr == s->fb_addr && width == s->width &&
        height == s->height && stride == s->fb_stride) {
        qemu_console_update_full(s->con);
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
    s->fb_addr = addr;
    s->fb_stride = stride;
    s->width = width;
    s->height = height;
    qemu_console_update_full(s->con);
    return true;
}

static const GraphicHwOps rk_display_ops = {
    .gfx_update = rk_display_gfx_update,
};

static RkDisplay *rk_display_state;

void rk3588_display_repaint(void)
{
    if (rk_display_state) {
        rk_display_gfx_update(rk_display_state);
    }
}

void rk3588_display_init(MachineState *ms)
{
    RkDisplay *s = g_new0(RkDisplay, 1);

    (void)ms;
    s->con = qemu_graphic_console_create(NULL, 0, &rk_display_ops, s);
    rk_display_state = s;
}
