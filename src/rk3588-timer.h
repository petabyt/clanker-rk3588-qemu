/*
 * Rockchip rk3288-style timer (timer@feae0000 on the OPi5 DTB, compat
 * "rockchip,rk3588-timer","rockchip,rk3288-timer"; Linux
 * drivers/clocksource/timer-rockchip.c).
 *
 * Registers (32-bit LE, window 0x20):
 *   +0x00 LOAD_COUNT      (driver also stores 0 at +0x04: 64-bit load)
 *   +0x08 CURRENT_VALUE   (live down-counter; +0x0c is the high half)
 *   +0x10 CONTROL (rk3288): bit0 enable, bit1 mode (1 = user-defined
 *          count = one-shot, 0 = free-running/wrap), bit2 int unmask
 *   +0x18 INT_STATUS: bit0 = counter-expired pending, write-1-to-clear
 *
 * The kernel uses it for BOTH a clocksource + sched_clock (reads
 * CURRENT_VALUE as a free-running 32-bit down-counter at the "timer"
 * clock rate) and a broadcast clock-event while CPU idle stops the
 * local architectural timers.  CURRENT_VALUE is computed from virtual
 * time; a QEMUTimer callback at each expiry delivers interrupts without
 * requiring any guest register reads or ticking at the input clock rate.
 */
#ifndef RK3588_TIMER_H
#define RK3588_TIMER_H

#define TYPE_RK3588_TIMER "rk3588-timer"

#endif
