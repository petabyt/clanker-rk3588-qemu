#!/usr/bin/env python3
"""Run the unmodified RK3588 ROM's DWC3 routines against a QEMU xHCI host.

This invokes the ROM's USB init and poll functions, not its full boot path.
"""
import json
import binascii
import os
import socket
import subprocess
import sys
import tempfile
import time

qemu = sys.argv[1] if len(sys.argv) > 1 else "build/qemu/qemu-system-aarch64"
firmware = sys.argv[2] if len(sys.argv) > 2 else "build/dwc3-maskrom-harness.bin"
tmp = tempfile.TemporaryDirectory(prefix="dwc3-guest-", dir="/tmp/opencode")
sock_path = tmp.name + "/qmp"
proc = subprocess.Popen(
    [qemu, "-M", "rk3588,sram-boot=on,maskrom-test=on", "-bios", firmware, "-S",
     "-device", "loader,file=private/rk3588_bootrom.bin,addr=0xffff0000,force-raw=on", "-display", "none", "-serial", "none", "-monitor", "none",
     "-qtest", "stdio", "-qtest-log", "/dev/null",
     "-qmp", f"unix:{sock_path},server=on,wait=off",
     "-device", "rk3588-otg-gadget,bus=usb-a.0,port=1"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True)

def qtest(command):
    proc.stdin.write(command + "\n")
    proc.stdin.flush()
    result = proc.stdout.readline().strip()
    if not result.startswith("OK"):
        raise RuntimeError(f"{command}: {result}")
    return result[2:].strip()

def write(addr, value):
    qtest(f"writel {addr:#x} {value:#x}")

def read(addr):
    return int(qtest(f"readl {addr:#x}"), 16)

def words(addr, values):
    for i, value in enumerate(values):
        write(addr + 4 * i, value)

def wait_for(test, what):
    deadline = time.monotonic() + 10
    while not test():
        if proc.poll() is not None or time.monotonic() >= deadline:
            regs = {hex(a): hex(read(a)) for a in
                    (0xfc00c40c, 0xfc00c80c, 0xfc00c81c, 0xff0c0000,
                     0xff0c0008, 0xff00049c, 0xff0004a0, 0xff0004a4,
                     0xff000408, 0xff0004cc, 0xff0004ec, 0xff0004f0)}
            regs["events"] = [hex(read(0xff0002e0 + i * 4)) for i in range(32)]
            raise RuntimeError(f"timeout waiting for {what}: {regs}")
        time.sleep(0.001)

def event_done(base, pointer, code=1):
    for i in range(32):
        addr = base + i * 16
        if read(addr) == pointer and read(addr + 12) & 1:
            actual = read(addr + 8) >> 24
            if actual != code:
                raise RuntimeError(f"xHCI completion {pointer:#x}: {actual}, wanted {code}")
            return True
    return False

def setup_ready():
    return (read(0xff00049c) == 1 and read(0xff000408) & 0x10 and
            read(0xff0004cc) & 1)

try:
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(5)
    wait_for(lambda: os.path.exists(sock_path), "QMP socket")
    sock.connect(sock_path)
    stream = sock.makefile("rwb", buffering=0)
    stream.readline()
    for command in ("qmp_capabilities", "cont"):
        stream.write((json.dumps({"execute": command}) + "\n").encode())
        while True:
            reply = json.loads(stream.readline())
            if "error" in reply:
                raise RuntimeError(reply)
            if "return" in reply:
                break

    op, rt, db = 0xfc400020, 0xfc401000, 0xfc402000
    cmd, event, erst, dcbaa = 0x1026000, 0x1024000, 0x1025000, 0x1027000
    inp, output, ep_ring = 0x1028000, 0x1029000, 0x102a000
    host_buf, bulk_ring, bulk_buf = 0x1032000, 0x102b000, 0x1035000
    wait_for(lambda: read(op + 0x400) & 1, "gadget pull-up")
    assert read(0xff0c0000) == 0x87654321 and read(0xff0c0004) == 0
    write(op, 2)
    words(erst, [event, 0, 64, 0])
    write(op + 0x30, dcbaa)
    write(op + 0x34, 0)
    write(op + 0x18, cmd | 1)
    write(op + 0x1c, 0)
    write(op + 0x38, 1)
    write(rt + 0x28, 1)
    write(rt + 0x30, erst)
    write(rt + 0x34, 0)
    write(rt + 0x38, event)
    write(rt + 0x3c, 0)
    write(op, 1)
    write(op + 0x400, 0x210)
    words(cmd, [0, 0, 0, (9 << 10) | 1])
    write(db, 0)
    wait_for(lambda: event_done(event, cmd), "enable slot")

    write(dcbaa + 8, output)
    write(inp + 4, 3)
    write(inp + 32, (1 << 27) | (3 << 20))
    write(inp + 36, 1 << 16)
    write(inp + 17 * 4, (64 << 16) | (4 << 3) | (3 << 1))
    write(inp + 18 * 4, ep_ring | 1)
    write(inp + 20 * 4, 8)
    words(cmd + 16, [inp, 0, 0, (11 << 10) | (1 << 24) | 1])
    write(db, 0)
    wait_for(lambda: event_done(event, cmd + 16), "SET_ADDRESS")
    wait_for(lambda: read(0xff000298) == 0x10500 and setup_ready(),
             "MaskROM rearm after address")

    words(ep_ring, [0x01000680, 0x00120000, 8, 0x30841,
                    host_buf, 0, 18, 0x10c01, 0, 0, 0, 0x1021])
    write(db + 4, 1)
    wait_for(lambda: event_done(event, ep_ring + 32), "GET_DESCRIPTOR")
    assert [read(host_buf + i * 4) for i in range(4)] == [
        0x02000112, 0x40000000, 0x350b2207, 0x00000100]
    print("PASS: actual MaskROM device descriptor")
    wait_for(setup_ready, "MaskROM descriptor status")

    # The ROM has its own configuration descriptor (32 bytes).
    words(ep_ring + 48, [0x02000680, 0x00200000, 8, 0x30841,
                         host_buf, 0, 32, 0x10c01, 0, 0, 0, 0x1021])
    write(db + 4, 1)
    wait_for(lambda: event_done(event, ep_ring + 80), "MaskROM configuration descriptor")
    assert read(host_buf) == 0x00200209
    wait_for(setup_ready, "MaskROM configuration status")
    words(ep_ring + 96, [0x00010900, 0, 8, 0x841, 0, 0, 0, 0x11021])
    write(db + 4, 1)
    wait_for(lambda: event_done(event, ep_ring + 112), "MaskROM SET_CONFIGURATION")
    wait_for(setup_ready, "MaskROM setup after configuration")
    print("PASS: actual MaskROM configuration and SET_CONFIGURATION")

    # Vendor download to index 0x472 targets RAM at zero, avoiding the
    # harness at ff001000. A deliberately invalid CRC prevents execution.
    # Length 100 forces the ROM's chained 64-byte and bounce-buffer TRBs.
    src = 0x1038000
    for i in range(25):
        write(src + 4 * i, 0x11220000 + i)
    words(ep_ring + 128, [0x00000c40, 0x00640472, 8, 0x20841,
                          src, 0, 100, 0x0c01, 0, 0, 0, 0x11021])
    write(db + 4, 1)
    wait_for(lambda: event_done(event, ep_ring + 160), "MaskROM vendor download")
    assert [read(i * 4) for i in range(25)] == [0x11220000 + i for i in range(25)]
    print("PASS: actual MaskROM chained vendor download into RAM")

    # A CRC-valid download executes at zero, writes a marker and returns to
    # the ROM.  This proves the control transaction reaches its real handoff.
    wait_for(setup_ready, "MaskROM download status")
    insns = [0xd2800202, 0xf2bfe182, 0x52824683, 0xb9000043, 0xd65f03c0]
    payload = b"".join(i.to_bytes(4, "little") for i in insns)
    payload += binascii.crc_hqx(payload, 0xffff).to_bytes(2, "big")
    for i in range(0, len(payload), 4):
        write(src + i, int.from_bytes(payload[i:i + 4].ljust(4, b"\0"), "little"))
    words(ep_ring + 176, [0x00000c40, (len(payload) << 16) | 0x472, 8, 0x20841,
                          src, 0, len(payload), 0x0c01, 0, 0, 0, 0x11021])
    write(db + 4, 1)
    wait_for(lambda: event_done(event, ep_ring + 208), "CRC-valid MaskROM download")
    wait_for(lambda: read(0xff0c0010) == 0x1234, "MaskROM payload handoff")
    print("PASS: actual MaskROM validates CRC and executes downloaded code")

    for case, length in enumerate((7, 64, 129), start=1):
        wait_for(setup_ready, "MaskROM download status")
        payload = bytes((i * 13 + case * 17) & 0xff for i in range(length))
        for i in range(0, length, 4):
            write(src + i, int.from_bytes(payload[i:i + 4].ljust(4, b"\0"), "little"))
        off = 224 + (case - 1) * 48
        words(ep_ring + off, [0x00000c40, (length << 16) | 0x472, 8, 0x20841,
                              src, 0, length, 0x0c01, 0, 0, 0, 0x11021])
        write(db + 4, 1)
        wait_for(lambda: event_done(event, ep_ring + off + 32),
                 f"MaskROM {length}-byte vendor download")
        data = b"".join(read(i).to_bytes(4, "little")
                        for i in range(0, length, 4))[:length]
        assert data == payload, f"MaskROM {length}-byte download mismatch"
        print(f"PASS: actual MaskROM {length}-byte vendor download")

    # A full 4 KiB block is not the end of a Rockchip download. Follow it
    # with a short block and a CRC over both, then verify the ROM executes it.
    image = b"".join(i.to_bytes(4, "little") for i in insns)
    image += bytes((i * 7 + 3) & 0xff for i in range(4096 + 20 - len(image)))
    image += binascii.crc_hqx(image, 0xffff).to_bytes(2, "big")
    write(0xff0c0010, 0)
    for case, chunk in enumerate((image[:4096], image[4096:])):
        wait_for(setup_ready, "MaskROM multi-block status")
        for i in range(0, len(chunk), 4):
            write(src + i, int.from_bytes(chunk[i:i + 4].ljust(4, b"\0"), "little"))
        off = 368 + case * 48
        words(ep_ring + off, [0x00000c40, (len(chunk) << 16) | 0x472, 8, 0x20841,
                              src, 0, len(chunk), 0x0c01, 0, 0, 0, 0x11021])
        write(db + 4, 1)
        wait_for(lambda: event_done(event, ep_ring + off + 32),
                 "MaskROM multi-block download")
        if case == 0:
            assert read(0xff0c0010) == 0, "ROM executed before final block"
    wait_for(lambda: read(0xff0c0010) == 0x1234, "multi-block payload handoff")
    assert read(4096) == int.from_bytes(image[4096:4100], "little")
    print("PASS: actual MaskROM multi-block download, CRC and payload handoff")
finally:
    proc.terminate()
    proc.wait(timeout=10)
    tmp.cleanup()
