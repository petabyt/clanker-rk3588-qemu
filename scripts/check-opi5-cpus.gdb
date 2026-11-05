set pagination off
python
import gdb
import re
import struct

# Read-only proof for the SHA256-pinned 6.18.43 Armbian image.  Offsets
# below are from its own /boot/System.map-6.18.43-current-rockchip64.
kernel_cpu = None
for thread in gdb.selected_inferior().threads():
    thread.switch()
    vbar = int(gdb.parse_and_eval('$VBAR_EL2')) & ((1 << 64) - 1)
    ttbr = int(gdb.parse_and_eval('$TTBR1_EL2')) & ((1 << 64) - 1)
    if vbar >> 48 == 0xffff and ttbr:
        kernel_cpu = thread
        break
assert kernel_cpu is not None, 'no Linux EL2 page-table context found'
tcr = int(gdb.parse_and_eval('$TCR_EL2')) & ((1 << 64) - 1)
assert (tcr >> 30) & 3 == 2, 'expected 4 KiB TTBR1 granule'
assert (tcr >> 16) & 63 == 16, 'expected 48-bit TTBR1 virtual addresses'
kernel_base = vbar & ~0x1fffff
pa_mask = (1 << 48) - 1

def physical(address, length):
    result = gdb.execute('maintenance packet m%x,%x' % (address, length),
                         to_string=True)
    match = re.search(r'received: "([0-9a-fA-F]+)"', result)
    assert match and len(match[1]) == length * 2, result
    return bytes.fromhex(match[1])

def translate(address):
    table = ttbr & pa_mask & ~4095
    for level, shift in enumerate((39, 30, 21, 12)):
        entry = table + ((address >> shift) & 511) * 8
        descriptor = struct.unpack('<Q', physical(entry, 8))[0]
        assert descriptor & 1, 'unmapped Linux virtual address %#x' % address
        if level == 3 or not descriptor & 2:
            mask = (1 << shift) - 1
            return (descriptor & pa_mask & ~mask) | (address & mask)
        table = descriptor & pa_mask & ~4095
    raise AssertionError('invalid page table')

gdb.execute('maintenance packet Qqemu.PhyMemMode:1', to_string=True)
try:
    magic = physical(translate(kernel_base + 0x38), 4)
    assert magic == b'ARM\x64', 'not the expected arm64 Image mapping'
    online = struct.unpack('<Q', physical(translate(kernel_base + 0x1f8ac08), 8))[0]
    assert online == 0xff, 'Linux online mask is %#x, not 8 CPUs' % online
    print('P20_CPU_PROOF: Linux __cpu_online_mask=0xff (8 CPUs)')
finally:
    gdb.execute('maintenance packet Qqemu.PhyMemMode:0', to_string=True)
end
