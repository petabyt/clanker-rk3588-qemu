/*
 * RK3588 crypto engine, hash path (see rk3588-crypto.h).
 *
 * Register map (as used by the SPL's rk_crypto driver):
 *   +0x04  CRYPTO_RST_CTL
 *   +0x08  DMA_INT_EN
 *   +0x0c  DMA_INT_ST (SPL reads 0x40 as "done"; the TRM names bit6
 *          zero_len and bit0 list_done, so the SPL's expectation needs checking)
 *   +0x10  CRYPTO_DMA_CTL: bit0 starts the list (driver writes 0x10001)
 *   +0x14  CRYPTO_DMA_LLI_ADDR (guest physical)
 *   +0x48  CRYPTO_HASH_CTL (driver writes 0xffff0024 for SHA-256)
 *   +0x3a0 + 4n  CRYPTO_HASH_DOUT_n, digest words H0..H7
 *   +0x3e4 CRYPTO_HASH_VALID: 1 once the digest is valid; driver polls it
 *
 * Descriptor (32-bit words, little-endian in guest RAM):
 *   +0x00 src_addr   +0x04 src_len   ...   +0x20 next (0 = end)
 * The list is walked while src_len != 0 and the next pointer is set.
 *
 * Only SHA-256 is modelled (the SPL's digest length is 32 bytes). The
 * digest is computed with QEMU's host crypto backend, so the FIT's
 * stored hash is the real check on correctness.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "crypto/hash.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "rk3588-crypto.h"

#define RK_CRYPTO_STATUS_IDLE   0x40
#define RK_CRYPTO_DIGEST_OFF    0x3a0
#define RK_CRYPTO_DIGEST_WORDS  8
#define RK_CRYPTO_READY_OFF     0x3e4
#define RK_CRYPTO_MAX_DESC      16

static uint32_t rk_crypto_lli;                 /* +0x14 */
static uint32_t rk_crypto_digest[RK_CRYPTO_DIGEST_WORDS];
static bool rk_crypto_ready;

static void rk_crypto_run_sha256(uint32_t lli)
{
    GByteArray *data = g_byte_array_new();
    uint8_t *digest = NULL;
    size_t digest_len = 0;
    Error *err = NULL;
    uint32_t desc = lli;

    for (int n = 0; n < RK_CRYPTO_MAX_DESC && desc; n++) {
        uint32_t d[9];
        uint32_t src, len, next;

        address_space_read(&address_space_memory, desc,
                           MEMTXATTRS_UNSPECIFIED, d, sizeof(d));
        src = le32_to_cpu(d[0]);
        len = le32_to_cpu(d[1]);
        next = le32_to_cpu(d[8]);
        if (!len) {
            break;
        }
        {
            uint8_t *buf = g_malloc(len);
            address_space_read(&address_space_memory, src,
                               MEMTXATTRS_UNSPECIFIED, buf, len);
            g_byte_array_append(data, buf, len);
            g_free(buf);
        }
        desc = next;
    }

    if (qcrypto_hash_bytes(QCRYPTO_HASH_ALGO_SHA256, data->data, data->len,
                           &digest, &digest_len, &err) < 0 ||
        digest_len != RK_CRYPTO_DIGEST_WORDS * 4) {
        error_report_err(err);
        g_byte_array_free(data, true);
        g_free(digest);
        rk_crypto_ready = false;
        return;
    }
    for (int i = 0; i < RK_CRYPTO_DIGEST_WORDS; i++) {
        /* the driver reads each word big-endian, so store the value
         * with the byte order of the digest bytes */
        rk_crypto_digest[i] = ((uint32_t)digest[4 * i] << 24) |
                              ((uint32_t)digest[4 * i + 1] << 16) |
                              ((uint32_t)digest[4 * i + 2] << 8) |
                              digest[4 * i + 3];
    }
    rk_crypto_ready = true;
    g_byte_array_free(data, true);
    g_free(digest);
}

uint64_t rk3588_crypto_read(uint32_t stored, hwaddr off)
{
    if (off == 0x4) {
        return 0;                       /* reset/enable self-clears */
    }
    if (off == 0xc) {
        return RK_CRYPTO_STATUS_IDLE;
    }
    if (off >= RK_CRYPTO_DIGEST_OFF &&
        off < RK_CRYPTO_DIGEST_OFF + 4 * RK_CRYPTO_DIGEST_WORDS) {
        return rk_crypto_digest[(off - RK_CRYPTO_DIGEST_OFF) / 4];
    }
    if (off == RK_CRYPTO_READY_OFF) {
        return rk_crypto_ready ? 1 : 0;
    }
    return stored;
}

void rk3588_crypto_write(hwaddr off, uint64_t value)
{
    switch (off) {
    case 0x14:
        rk_crypto_lli = value;
        break;
    case 0x10:
        /* start: the driver writes 0x10001 after the descriptor address */
        if ((value & 0x1) && rk_crypto_lli) {
            rk_crypto_run_sha256(rk_crypto_lli);
        }
        break;
    case RK_CRYPTO_READY_OFF:
        if (value & 0x1) {
            rk_crypto_ready = false;    /* ack */
        }
        break;
    default:
        break;
    }
}
