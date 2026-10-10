#!/usr/bin/env python3
"""Write our BL31 into a scratch copy of the stock SD image's FIT atf-1 slot.

usage: own-bl31-sd.py <stock.img> <bl31.bin> <out.img>

The FIT (GPT partition 'uboot', at 0x100000) stores atf-1 as external data at
align256(fit_end) + data-offset(0x500000). The SPL verifies atf-1 against a
sha256 in the FIT, so the new image is padded to the stock slot size and the
hash is rewritten in place (same property length, so no offsets move).
The stock file is never modified.
"""
import hashlib
import struct
import sys

FIT_BASE = 0x100000
ATF1_DATA_OFFSET = 0x500000
ATF1_SIZE = 0x22176  # data-size of images/atf-1 in the stock FIT


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    stock = bytearray(open(sys.argv[1], 'rb').read())
    bl31 = open(sys.argv[2], 'rb').read()

    magic, fit_tot = struct.unpack_from('>II', stock, FIT_BASE)
    if magic != 0xd00dfeed:
        print('no FIT at 0x%x' % FIT_BASE, file=sys.stderr)
        return 1
    fit_end = FIT_BASE + fit_tot
    slot = ((fit_end + 0xff) & ~0xff) + ATF1_DATA_OFFSET

    old = bytes(stock[slot:slot + ATF1_SIZE])
    old_hash = hashlib.sha256(old).digest()
    fit_region = bytes(stock[FIT_BASE:fit_end])
    if fit_region.count(old_hash) != 1:
        print('stock atf-1 hash not found in FIT at slot 0x%x' % slot,
              file=sys.stderr)
        return 1

    if len(bl31) > ATF1_SIZE:
        print('bl31 %d bytes exceeds slot %d' % (len(bl31), ATF1_SIZE),
              file=sys.stderr)
        return 1
    new = bl31 + b'\0' * (ATF1_SIZE - len(bl31))
    stock[slot:slot + ATF1_SIZE] = new

    new_hash = hashlib.sha256(new).digest()
    idx = stock.index(old_hash, FIT_BASE, fit_end)
    stock[idx:idx + 32] = new_hash

    open(sys.argv[3], 'wb').write(stock)
    print('slot 0x%x: %d bytes (padded to %d), hash %s' %
          (slot, len(bl31), ATF1_SIZE, new_hash.hex()[:16]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
