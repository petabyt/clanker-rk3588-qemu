#ifndef RK3588_DISPLAY_H
#define RK3588_DISPLAY_H

#include "hw/core/boards.h"

/*
 * VOP2 -> ramfb-style display: binds a guest-RAM display surface to a
 * graphic console once the firmware programs the ESmart2 scanout layer
 * (P9).  Refresh happens on demand via the GraphicHwOps.gfx_update
 * callback, which QEMU invokes before every UI redraw and screendump.
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
