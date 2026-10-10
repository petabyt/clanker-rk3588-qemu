#ifndef RK3588_CRYPTO_H
#define RK3588_CRYPTO_H

#include "qemu/osdep.h"

/*
 * RK3588 crypto engine (rockchip,rk3588-crypto, base 0xfe370000).
 * Hash path only, as used by the SPL's rk_crypto driver: the driver
 * builds a linked list of {src_addr, src_len, ..., next} descriptors,
 * writes its address to +0x14, starts the hash with +0x40, and reads
 * the digest words from 0x3a0.. once +0x3e4 reads nonzero.
 *
 * Hooks have the RkStubReadHook/RkStubWriteHook signatures used by
 * rk3588-stubs.c. Single instance, so state is file-static.
 */
uint64_t rk3588_crypto_read(uint32_t stored, hwaddr off);
void rk3588_crypto_write(hwaddr off, uint64_t value);

#endif
