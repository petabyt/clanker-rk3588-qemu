/* Genbook-faithful input for the tiny-core-linux demo:
 *
 *  touchpad = hid-over-i2c @0x2c on i2c5 (mainline rk3588-coolpi-cm5-genbook:
 *  compatible "hid-over-i2c", hid-descr-addr 0x0020, IRQ GPIO1_PD6 low).
 *
 *  The dw-i2c model routes address 0x2c here.  Register framing from the
 *  controller: writes arrive as reg16 = txbuf[1] | txbuf[2]<<8 with the
 *  payload after (hid over-i2c registers are 2-byte LE); reads arrive with
 *  only the LOW byte (the controller stores mrxraddr & 0xff) — all our
 *  registers are < 0x100 so that is sufficient.
 *
 *  Protocol consumed by this kernel's i2c-hid-core:
 *   - probe:      plain byte read -> 0x00 (ACK)
 *   - descriptor: read 30 bytes @0x20 (struct i2c_hid_desc, kernel layout)
 *   - reportdesc: read @0x22 (wReportDescRegister)
 *   - commands:   write [reg16=0x00][type/id][opcode] (SET_POWER=0x08,
 *                 RESET=0x01...); device considers them done instantly
 *   - input:      on IRQ (line low), host plain-reads wMaxInputLength(8)
 *                 bytes: [le16 length][report...]
 *
 *  Mouse reports are fed from the QEMU input system (same event flow the
 *  gtk/sdl console uses), so the HOST mouse drives the guest touchpad.
 */
#ifndef RK3588_I2CHID_H
#define RK3588_I2CHID_H

#include "hw/core/sysbus.h"

#define RK_I2CHID_ADDR 0x2c

/* Register the QEMU input handler (call once from the i2c5 realize when
 * the "has-touchpad" property is set). */
void rk_i2chid_setup(DeviceState *dev);

/* Slave accessors used by rk3588-i2c.c.  Both return false to NACK. */
bool rk_i2chid_write(unsigned int reg16, const uint8_t *payload,
                     unsigned int plen);
bool rk_i2chid_read(unsigned int reg, uint8_t *buf, unsigned int len);

#endif