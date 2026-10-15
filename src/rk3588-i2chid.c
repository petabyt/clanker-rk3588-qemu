/* hid-over-i2c touchpad slave + QEMU input bridge (see rk3588-i2chid.h). */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "ui/input.h"
#include "rk3588-i2chid.h"
#include "rk3588-stubs.h"

/* ---------------------------------------------------------------------
 * HID descriptor — EXACT layout of this kernel's struct i2c_hid_desc
 * (drivers/hid/i2c-hid/i2c-hid-core.c:75, 30 bytes):
 *   +0  wHIDDescLength   +2  bcdVersion      +4  wReportDescLength
 *   +6  wReportDescReg   +8  wInputRegister  +10 wMaxInputLength
 *   +12 wOutputRegister  +14 wMaxOutputLength +16 wCommandRegister
 *   +18 wDataRegister    +20 wVendorID       +22 wProductID
 *   +24 wVersionID       +26 reserved[4]
 * ------------------------------------------------------------------- */
static uint8_t tp_hid_desc[30] = {
    30, 0,                   /* wHIDDescLength      */
    0x00, 0x01,              /* bcdVersion 1.00     */
    0, 0,                    /* wReportDescLength (patched in setup) */
    0x22, 0x00,              /* wReportDescRegister */
    0x03, 0x00,              /* wInputRegister      */
    0x08, 0x00,              /* wMaxInputLength = 8 */
    0x02, 0x00,              /* wOutputRegister     */
    0x00, 0x00,              /* wMaxOutputLength    */
    0x00, 0x00,              /* wCommandRegister    */
    0x06, 0x00,              /* wDataRegister       */
    0x34, 0x12,              /* wVendorID 0x1234    */
    0x77, 0x27,              /* wProductID 0x2777   */
    0x01, 0x00,              /* wVersionID          */
    0, 0, 0, 0,              /* reserved            */
};

/* Standard boot mouse, no report IDs: buttons(3)+pad(5), X, Y, wheel. */
static const uint8_t tp_report_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,
    0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08,
    0x95, 0x01, 0x81, 0x06,
    0xc0, 0xc0,
};

/* ---------------------------------------------------------------------
 * Pending input: one frame [le16=6][buttons][dx][dy][wheel][pad][pad]
 * ------------------------------------------------------------------- */
#define TP_FRAME_LEN 8
static uint8_t tp_frame[TP_FRAME_LEN];
static bool tp_frame_pending;
static bool tp_reset_pending;

/* i2c-hid opcodes (this kernel's i2c-hid-core.c) */
#define I2C_HID_OPCODE_RESET      0x01
#define I2C_HID_OPCODE_SET_POWER  0x08

static void tp_line_update(void)
{
    /* Level-low line: asserted while a report OR a reset-completion is
     * pending.  i2c-hid's finish_hwreset waits for the reset-done IRQ
     * (get_input returning length 0), so the RESET command must also
     * raise the line. */
    rk3588_touchpad_set_line((tp_frame_pending || tp_reset_pending) ? 1 : 0);
}

/* Sequential-read cursor: the rk3x model caps a receive burst at 32
 * bytes, so a 64-byte report descriptor arrives as two 0x22 reads (the
 * register is only re-selected on a WRITE).  Serve bytes from the
 * cursor and advance it; any write (register select) resets it. */
static unsigned tp_cur_off;

bool rk_i2chid_write(unsigned int reg16, const uint8_t *payload,
                     unsigned int plen)
{
    tp_cur_off = 0;   /* register select resets the read cursor */
    if (reg16 == 0x00 && plen >= 2) {
        uint8_t opcode = payload[1];
        if (opcode == I2C_HID_OPCODE_RESET) {
            tp_reset_pending = true;
            tp_line_update();       /* reset-done IRQ: host reads len 0 */
        }
        /* SET_POWER / SET_IDLE / SET_PROTOCOL: accepted, done at once
           (the driver only ever reads the command register back as 0). */
        return true;
    }
    /* Output report (boot mouse has none) / register select: accept. */
    return true;
}

bool rk_i2chid_read(unsigned int reg, uint8_t *buf, unsigned int len)
{
    memset(buf, 0, len);

    if (tp_frame_pending) {
        unsigned int n = len < TP_FRAME_LEN ? len : TP_FRAME_LEN;
        memcpy(buf, tp_frame, n);
        tp_frame_pending = false;
        tp_line_update();               /* level-low: deassert when empty */
        return true;
    }
    if (tp_reset_pending) {
        /* length 0 == "reset completed" to i2c_hid_get_input */
        tp_reset_pending = false;
        return true;
    }

    switch (reg & 0xff) {
    case 0x20: {                        /* 30-byte HID descriptor */
        unsigned int n = len < sizeof(tp_hid_desc) ? len
                                                        : sizeof(tp_hid_desc);
        if (tp_cur_off < sizeof(tp_hid_desc)) {
            unsigned int avail = sizeof(tp_hid_desc) - tp_cur_off;
            n = len < avail ? len : avail;
            memcpy(buf, tp_hid_desc + tp_cur_off, n);
            tp_cur_off += n;
        }
        return true;
    }
    case 0x22: {                        /* report descriptor, chunked */
        if (tp_cur_off < sizeof(tp_report_desc)) {
            unsigned int avail = sizeof(tp_report_desc) - tp_cur_off;
            unsigned int n = len < avail ? len : avail;
            memcpy(buf, tp_report_desc + tp_cur_off, n);
            tp_cur_off += n;
        }
        return true;
    }
    default:
        /* probe byte-read, command status (0 = done), data register */
        return true;
    }
}

/* ---------------------------------------------------------------------
 * QEMU input side: accumulate console pointer events, push a report on
 * sync (mirrors hw/input/hid.c's pointer ops, self-contained because
 * HIDState requires a real DeviceState layout we do not have).
 * ------------------------------------------------------------------- */
static int pend_dx, pend_dy, pend_dz;
static int last_abs_x = -1, last_abs_y = -1;
static uint8_t pend_buttons, pushed_buttons;

static void tp_input_event(DeviceState *dev, QemuConsole *src,
                           QemuInputEvent *evt)
{
    static const int bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT]   = 0x01,
        [INPUT_BUTTON_RIGHT]  = 0x02,
        [INPUT_BUTTON_MIDDLE] = 0x04,
    };

    switch (evt->type) {
    case INPUT_EVENT_KIND_REL:
        if (evt->rel.axis == INPUT_AXIS_X) {
            pend_dx += evt->rel.value;
        } else if (evt->rel.axis == INPUT_AXIS_Y) {
            pend_dy += evt->rel.value;
        }
        break;
    case INPUT_EVENT_KIND_ABS:
        /* console pointer positions arrive as absolute; derive deltas */
        if (evt->abs.axis == INPUT_AXIS_X) {
            if (last_abs_x >= 0) {
                pend_dx += evt->abs.value - last_abs_x;
            }
            last_abs_x = evt->abs.value;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            if (last_abs_y >= 0) {
                pend_dy += evt->abs.value - last_abs_y;
            }
            last_abs_y = evt->abs.value;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button < INPUT_BUTTON__MAX &&
            bmap[evt->btn.button]) {
            if (evt->btn.down) {
                pend_buttons |= bmap[evt->btn.button];
            } else {
                pend_buttons &= ~bmap[evt->btn.button];
            }
        } else if (evt->btn.button == INPUT_BUTTON_WHEEL_UP && evt->btn.down) {
            pend_dz -= 1;
        } else if (evt->btn.button == INPUT_BUTTON_WHEEL_DOWN && evt->btn.down) {
            pend_dz += 1;
        }
        break;
    default:
        break;
    }
}

static int8_t clamp_s8(int v)
{
    if (v > 127) {
        return 127;
    }
    if (v < -127) {
        return -127;
    }
    return (int8_t)v;
}

static void tp_input_sync(DeviceState *dev)
{
    bool moved = pend_dx || pend_dy || pend_dz ||
                 pend_buttons != pushed_buttons;

    if (!moved) {
        return;
    }
    tp_frame[0] = 6;                    /* le16 report length */
    tp_frame[1] = 0;
    tp_frame[2] = pend_buttons;
    tp_frame[3] = (uint8_t)clamp_s8(pend_dx);
    tp_frame[4] = (uint8_t)clamp_s8(pend_dy);
    tp_frame[5] = (uint8_t)clamp_s8(pend_dz);
    tp_frame[6] = 0;
    tp_frame[7] = 0;
    tp_frame_pending = true;
    pushed_buttons = pend_buttons;
    pend_dx = pend_dy = pend_dz = 0;
    tp_line_update();
}

static const QemuInputHandler tp_input_ops = {
    .name  = "genbook-touchpad",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_REL |
             INPUT_EVENT_MASK_ABS,
    .event = tp_input_event,
    .sync  = tp_input_sync,
};

void rk_i2chid_setup(DeviceState *dev)
{
    QemuInputHandlerState *s;

    tp_hid_desc[4] = sizeof(tp_report_desc) & 0xff;
    tp_hid_desc[5] = sizeof(tp_report_desc) >> 8;

    /* No bind: unbound handlers are the fallback sink in
     * qemu_input_find_handler(), so we receive the VOP2 console's pointer
     * events; activate() moves us to the head of the search list. */
    s = qemu_input_handler_register(dev, &tp_input_ops);
    qemu_input_handler_activate(s);
}