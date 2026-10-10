#ifndef RK3588_STUBS_H
#define RK3588_STUBS_H

#include "system/memory.h"

/*
 * Register-file stubs for the RK3588 blocks the ./rk firmware pokes
 * (CRU/PMU/IOC/GRF/SGRF/GPIO/PWM/VOP2/eDP...).  See rk3588-stubs.c for
 * the read/write semantics each class needs.
 */
void rk3588_create_stub_regions(MemoryRegion *sysmem);

/*
 * Set by rk3588.c when -bios carries a TF-A (BL31) ELF.  Gates the
 * ATF-only shutdown/reset hooks so the ./rk firmware paths keep their
 * exact prior behaviour.
 */
extern bool rk3588_atf_mode;

#endif
