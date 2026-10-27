#ifndef RK3588_DISPLAY_H
#define RK3588_DISPLAY_H

#include "hw/core/boards.h"

#define TYPE_RK3588_DISPLAY "rk3588-display"

/*
 * VOP2 -> ramfb-style display: binds a guest-RAM display surface to a
 * graphic console once the firmware (or a guest driver) programs a
 * VOP2 scanout layer (P9).  The console is a machine-internal sysbus
 * device with id "hdmi" (screendump target name); it is created from
 * the machine-init-done notifier so a command-line virtio-gpu head
 * keeps console 0 (the window/VNC default view) - see the .c file's
 * header comment for the ordering rationale.  Refresh happens on
 * demand via GraphicHwOps.gfx_update, which QEMU invokes before every
 * UI redraw and inside screendump's qemu_console_co_wait_update.
 */
void rk3588_display_init(MachineState *ms);

/*
 * Scanout refresh on demand: re-reads the active window and pushes the
 * guest framebuffer to the console.  Called by the VOP2 frame timer at
 * display cadence (this tree's qemu_console_hw_update() ignores NULL,
 * so the console cannot be looked up from the device side).
 */
void rk3588_display_repaint(void);

#endif /* RK3588_DISPLAY_H */
