#!/usr/bin/env python3
"""Drive an emulated xHCI host against RK3588 DWC3 EP0 through qtest."""
import subprocess
import sys
import time

qemu = sys.argv[1] if len(sys.argv) > 1 else "build/qemu/qemu-system-aarch64"
proc = subprocess.Popen(
    [qemu, "-M", "rk3588,sram-boot=on", "-bios", "futo-bootloader/rk/usb-poc/build/menu.bin",
     "-S", "-display", "none", "-serial", "none", "-monitor", "none",
     "-qtest", "stdio", "-qtest-log", "/dev/null",
     "-device", "rk3588-otg-gadget,bus=usb-a.0,port=1"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True)

def qtest(command):
    proc.stdin.write(command + "\n")
    proc.stdin.flush()
    response = proc.stdout.readline().strip()
    if not response.startswith("OK"):
        raise RuntimeError(f"{command}: {response}")
    return response[2:].strip()

def write(addr, value):
    qtest(f"writel {addr:#x} {value:#x}")

def read(addr):
    return int(qtest(f"readl {addr:#x}"), 16)

def words(base, values):
    for i, value in enumerate(values):
        write(base + i * 4, value)

def wait_value(addr, expected):
    deadline = time.monotonic() + 5
    while read(addr) != expected:
        if proc.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError(f"timeout waiting for {addr:#x} = {expected:#x}")
        time.sleep(0.001)

def wait_xhci_event(base, pointer, code=1):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        for i in range(32):
            addr = base + i * 16
            if read(addr) == pointer and read(addr + 12) & 1:
                assert read(addr + 8) >> 24 == code
                return
        time.sleep(0.001)
    entries = [[read(base + i * 16 + j * 4) for j in range(4)] for i in range(32)]
    raise RuntimeError(f"timeout waiting for xHCI event {pointer:#x}: {entries}")

try:
    op, rt, db = 0xfc400020, 0xfc401000, 0xfc402000
    cmd, event, erst, dcbaa = 0x1026000, 0x1024000, 0x1025000, 0x1027000
    inp, output, ep_ring = 0x1028000, 0x1029000, 0x102a000
    trb, setup_buf, status_trb = 0x1030000, 0x1031000, 0x1030020
    host_buf, data_trb, payload = 0x1032000, 0x1030040, 0x1033000

    # Configure the gadget event ring, then initialize the xHCI host.
    write(0xfc00c400, 0x1000000)
    write(0xfc00c408, 256)
    write(0xfc00c708, 6)
    write(0xfc00c704, 0x80000000)
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
    write(cmd + 12, (9 << 10) | 1)  # Enable Slot
    write(db, 0)
    wait_xhci_event(event, cmd)

    # EP0 OUT configuration and setup buffer; xHCI Address Device sends
    # SET_ADDRESS, which must reach the gadget firmware's setup TRB.
    write(0xfc00c80c, 0x409)
    write(0xfc00c808, 0x200)
    write(0xfc00c804, 0x500)
    write(0xfc00c80c, 0x401)
    write(0xfc00c808, 1)
    write(0xfc00c80c, 0x402)
    words(trb, [setup_buf, 0, 8, 0xc23])
    write(0xfc00c804, trb)
    write(0xfc00c808, 0)
    write(0xfc00c80c, 0x406)
    write(dcbaa + 8, output)
    write(inp + 4, 3)
    write(inp + 32, (1 << 27) | (3 << 20))
    write(inp + 36, 1 << 16)
    write(inp + 17 * 4, (64 << 16) | (4 << 3) | (3 << 1))
    write(inp + 18 * 4, ep_ring | 1)
    write(inp + 20 * 4, 8)
    words(cmd + 16, [inp, 0, 0, (11 << 10) | (1 << 24) | 1])
    write(db, 0)
    wait_xhci_event(event, cmd + 16)
    assert read(setup_buf) == 0x10500  # SET_ADDRESS, address 1
    assert not read(trb + 12) & 1

    # Give the guest a status TRB and then another setup TRB.
    write(0xfc00c40c, read(0xfc00c40c))
    write(0xfc00c818, 0x200)
    write(0xfc00c814, 0x2000500)
    write(0xfc00c81c, 0x401)
    write(0xfc00c818, 1)
    write(0xfc00c81c, 0x402)
    words(status_trb, [0, 0, 0, 0x833])
    write(0xfc00c814, status_trb)
    write(0xfc00c818, 0)
    write(0xfc00c81c, 0x406)
    assert not read(status_trb + 12) & 1
    write(0xfc00c40c, read(0xfc00c40c))
    words(trb, [setup_buf, 0, 8, 0xc23])
    write(0xfc00c804, trb)
    write(0xfc00c808, 0)
    write(0xfc00c80c, 0x406)

    # Host issues GET_DESCRIPTOR through the xHCI EP0 transfer ring.
    words(ep_ring, [0x01000680, 0x00120000, 8, 0x30841,
                    host_buf, 0, 18, 0x10c01, 0, 0, 0, 0x1021])
    write(db + 4, 1)
    wait_value(setup_buf, 0x01000680)
    assert read(setup_buf + 4) == 0x00120000
    assert not read(trb + 12) & 1

    # Respond via the DWC3 IN data TRB and check host completion/data.
    descriptor = [0x02000112, 0x40000000, 0x12345678, 0x01000100, 1]
    words(payload, descriptor)
    words(data_trb, [payload, 0, 18, 0x853])
    write(0xfc00c814, data_trb)
    write(0xfc00c818, 0)
    write(0xfc00c81c, 0x406)
    wait_xhci_event(event, ep_ring + 32)
    assert [read(host_buf + i * 4) for i in range(4)] == descriptor[:4]
    assert read(host_buf + 16) & 0xffff == descriptor[4]
    assert not read(data_trb + 12) & 1
    print("PASS: xHCI SET_ADDRESS and GET_DESCRIPTOR through DWC3 EP0 TRBs")

    # Complete the gadget-side OUT status and issue an OUT no-data request.
    words(status_trb, [0, 0, 0, 0x843])
    write(0xfc00c804, status_trb)
    write(0xfc00c808, 0)
    write(0xfc00c80c, 0x406)
    assert not read(status_trb + 12) & 1
    words(trb, [setup_buf, 0, 8, 0xc23])
    write(0xfc00c804, trb)
    write(0xfc00c808, 0)
    write(0xfc00c80c, 0x406)
    words(ep_ring + 48, [0x00010900, 0, 8, 0x841,
                         0, 0, 0, 0x11021])
    write(db + 4, 1)
    wait_value(setup_buf, 0x00010900)
    words(status_trb, [0, 0, 0, 0x833])
    write(0xfc00c814, status_trb)
    write(0xfc00c818, 0)
    write(0xfc00c81c, 0x406)
    wait_xhci_event(event, ep_ring + 64)
    assert not read(status_trb + 12) & 1
    print("PASS: xHCI SET_CONFIGURATION through DWC3 EP0 status TRB")

    # CDC SET_LINE_CODING: host OUT data, guest OUT data TRB and IN status.
    words(trb, [setup_buf, 0, 8, 0xc23])
    write(0xfc00c804, trb)
    write(0xfc00c808, 0)
    write(0xfc00c80c, 0x406)
    line_src, line_dst = 0x1038000, 0x1039000
    words(line_src, [0x0016e360, 0x00080000])
    words(ep_ring + 80, [0x00002021, 0x00070000, 8, 0x20841,
                         line_src, 0, 7, 0x0c01,
                         0, 0, 0, 0x11021])
    write(db + 4, 1)
    wait_value(setup_buf, 0x00002021)
    assert read(setup_buf + 4) == 0x00070000
    # Split the OUT payload over two chained DWC3 TRBs.
    words(data_trb, [line_dst, 0, 4, 0x855,
                     line_dst + 4, 0, 3, 0x853])
    write(0xfc00c804, data_trb)
    write(0xfc00c808, 0)
    write(0xfc00c80c, 0x406)
    assert read(line_dst) == 0x0016e360
    assert read(line_dst + 4) & 0xffffff == 0x080000
    assert not read(data_trb + 12) & 1
    assert not read(data_trb + 28) & 1
    words(status_trb, [0, 0, 0, 0x843])
    write(0xfc00c814, status_trb)
    write(0xfc00c818, 0)
    write(0xfc00c81c, 0x406)
    wait_xhci_event(event, ep_ring + 112)
    print("PASS: xHCI CDC SET_LINE_CODING through DWC3 EP0 OUT data TRB")

    # Configure physical endpoint 3 (USB endpoint 1 IN) in both controllers.
    bulk_ring, bulk_trb, bulk_src, bulk_dst = 0x102b000, 0x1030060, 0x1034000, 0x1035000
    write(0xfc00c838, (512 << 3) | (2 << 1))
    write(0xfc00c834, (3 << 25) | 0x500)
    write(0xfc00c83c, 0x401)
    write(0xfc00c838, 1)
    write(0xfc00c83c, 0x402)
    words(inp, [0] * (33 * 8))
    write(inp + 4, 1 | (1 << 3))
    words(inp + 32, [read(output + i * 4) for i in range(8)])
    write(inp + 32, (read(inp + 32) & ~(31 << 27)) | (3 << 27))
    write(inp + 33 * 4, (512 << 16) | (6 << 3) | (3 << 1))
    write(inp + 34 * 4, bulk_ring | 1)
    write(inp + 36 * 4, 512)
    words(cmd + 32, [inp, 0, 0, (12 << 10) | (1 << 24) | 1])
    write(db, 0)
    wait_xhci_event(event, cmd + 32)
    words(bulk_src, [0x54524155, 0x52564f20, 0x53552052, 0x00002142])
    # The host can request data before firmware supplies a transfer; arming
    # the endpoint must wake xHCI's NAKed request without another doorbell.
    words(bulk_ring, [bulk_dst, 0, 16, (1 << 10) | (1 << 5) | 1])
    write(db + 4, 3)
    assert all(read(event + i * 16) != bulk_ring for i in range(32))
    words(bulk_trb, [bulk_src, 0, 16, 0x813])
    write(0xfc00c834, bulk_trb)
    write(0xfc00c838, 0)
    write(0xfc00c83c, 0x406)
    wait_xhci_event(event, bulk_ring)
    assert [read(bulk_dst + i * 4) for i in range(4)] == [
        0x54524155, 0x52564f20, 0x53552052, 0x00002142]
    assert not read(bulk_trb + 12) & 1
    print("PASS: xHCI bulk IN through a DWC3 normal TRB")

    # A shorter gadget response than the host buffer is a USB short packet.
    words(bulk_trb, [bulk_src, 0, 16, 0x813])
    write(0xfc00c834, bulk_trb)
    write(0xfc00c838, 0)
    write(0xfc00c83c, 0x406)
    words(bulk_ring + 16, [bulk_dst, 0, 64, (1 << 10) | (1 << 5) | (1 << 2) | 1])
    write(db + 4, 3)
    wait_xhci_event(event, bulk_ring + 16, 13)
    assert [read(bulk_dst + i * 4) for i in range(4)] == [
        0x54524155, 0x52564f20, 0x53552052, 0x00002142]
    print("PASS: xHCI bulk IN short packet through a DWC3 TRB")

    # Endpoint 1 OUT, with a short packet leaving a 16-byte residual.
    out_ring, out_trb, out_src, out_dst = 0x102c000, 0x1030080, 0x1036000, 0x1037000
    write(0xfc00c828, (512 << 3) | (2 << 1))
    write(0xfc00c824, (2 << 25) | 0x500)
    write(0xfc00c82c, 0x401)
    write(0xfc00c828, 1)
    write(0xfc00c82c, 0x402)
    words(inp, [0] * (33 * 8))
    write(inp + 4, 1 | (1 << 2))
    words(inp + 32, [read(output + i * 4) for i in range(8)])
    write(inp + 32, (read(inp + 32) & ~(31 << 27)) | (3 << 27))
    write(inp + 25 * 4, (512 << 16) | (2 << 3) | (3 << 1))
    write(inp + 26 * 4, out_ring | 1)
    write(inp + 28 * 4, 512)
    words(cmd + 48, [inp, 0, 0, (12 << 10) | (1 << 24) | 1])
    write(db, 0)
    wait_xhci_event(event, cmd + 48)
    words(out_src, [0x72736f68, 0x6f742074, 0x52415520, 0x00000054])
    words(out_trb, [out_dst, 0, 32, 0x813])
    write(0xfc00c824, out_trb)
    write(0xfc00c828, 0)
    write(0xfc00c82c, 0x406)
    words(out_ring, [out_src, 0, 16, (1 << 10) | (1 << 5) | 1])
    write(db + 4, 2)
    wait_xhci_event(event, out_ring)
    assert [read(out_dst + i * 4) for i in range(4)] == [
        0x72736f68, 0x6f742074, 0x52415520, 0x00000054]
    assert read(out_trb + 8) & 0xffffff == 16
    assert not read(out_trb + 12) & 1
    print("PASS: xHCI bulk OUT through a DWC3 normal TRB")

    # An unsupported control request is stalled by the gadget firmware.
    words(trb, [setup_buf, 0, 8, 0xc23])
    write(0xfc00c804, trb)
    write(0xfc00c808, 0)
    write(0xfc00c80c, 0x406)
    words(ep_ring + 128, [0x0000ff80, 0x00010000, 8, 0x30841,
                          host_buf, 0, 1, 0x10c01,
                          0, 0, 0, 0x1021])
    write(db + 4, 1)
    wait_value(setup_buf, 0x0000ff80)
    write(0xfc00c80c, 0x404)
    wait_xhci_event(event, ep_ring + 128, 6)
    write(0xfc00c80c, 0x405)
    assert not read(0xfc00c80c) & 0x400
    print("PASS: DWC3 control endpoint STALL reaches the xHCI host")
finally:
    proc.terminate()
    proc.wait(timeout=10)
