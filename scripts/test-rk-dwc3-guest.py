#!/usr/bin/env python3
"""Exercise DWC3 with executing guest gadget firmware and a QEMU xHCI host."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

qemu = sys.argv[1] if len(sys.argv) > 1 else "build/qemu/qemu-system-aarch64"
firmware = sys.argv[2] if len(sys.argv) > 2 else "/tmp/opencode/dwc3-fixture.bin"
tmp = tempfile.TemporaryDirectory(prefix="dwc3-guest-", dir="/tmp/opencode")
sock_path = tmp.name + "/qmp"
serial_path = tmp.name + "/serial.log"
linux = "--linux" in sys.argv[3:]
loaders = []
if linux:
    root = Path(__file__).resolve().parents[1]
    inputs = root / "tiny-core-linux/out"
    dtb = Path(tmp.name) / "tiny.dtb"
    shutil.copyfile(inputs / "rk3588-tiny.dtb", dtb)
    def fdt(*args):
        subprocess.run(["fdtput", *map(str, args)], check=True)
    fdt("-t", "x", dtb, "/chosen", "linux,initrd-start", "0", "9000000")
    fdt("-t", "x", dtb, "/chosen", "linux,initrd-end", "0",
        f"{0x9000000 + (inputs / 'initramfs.bin').stat().st_size:x}")
    fdt("-t", "s", dtb, "/chosen", "bootargs", "earlycon console=ttyS0,1500000 nokaslr maxcpus=1")
    fdt("-r", dtb, "/psci")
    fdt("-d", dtb, "/cpus/cpu@0", "enable-method")
    for file, addr in ((inputs / "Image", 0x2000000), (dtb, 0x7000000),
                       (inputs / "initramfs.bin", 0x9000000)):
        loaders += ["-device", f"loader,file={file},addr={addr:#x},force-raw=on"]
proc = subprocess.Popen(
    [qemu, "-M", "rk3588,sram-boot=on", "-smp", "2", "-bios", firmware, "-S",
     "-display", "none", "-serial", f"file:{serial_path}", "-monitor", "none",
     "-qtest", "stdio", "-qtest-log", "/dev/null",
     "-qmp", f"unix:{sock_path},server=on,wait=off",
     "-device", "rk3588-otg-gadget,bus=usb-a.0,port=1", *loaders],
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
                     0xff0c0020, 0xff0c0040, 0xff0c0044, 0xff0c0048, 0xff0c004c,
                     0xff0c0050, 0xff0c0054, 0xff0c0058, 0xff0c005c,
                     0xff0c0060, 0xff0c0064, 0xff0c0068,
                     0xff0a8000, 0xff0a8004, 0xff0a4000, 0xff0a4004)}
            raise RuntimeError(f"timeout waiting for {what}: {regs}")
        time.sleep(0.001)

def event_done(base, pointer, code=1):
    for i in range(32):
        addr = base + i * 16
        if read(addr) == pointer and read(addr + 12) & 1:
            actual = read(addr + 8) >> 24
            if actual not in (code if isinstance(code, tuple) else (code,)):
                raise RuntimeError(f"xHCI completion {pointer:#x}: {actual}, wanted {code}")
            return True
    return False

def transfer_length(base, pointer, requested):
    for i in range(64):
        addr = base + i * 16
        if read(addr) == pointer and read(addr + 12) & 1:
            return requested - (read(addr + 8) & 0xffffff)
    raise RuntimeError(f"missing transfer event for {pointer:#x}")

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
    wait_for(lambda: read(0xff0c0000) >= 2, "guest EP0 rearm")

    words(ep_ring, [0x01000680, 0x00120000, 8, 0x30841,
                    host_buf, 0, 18, 0x10c01, 0, 0, 0, 0x1021])
    write(db + 4, 1)
    wait_for(lambda: event_done(event, ep_ring + 32), "GET_DESCRIPTOR")
    assert [read(host_buf + i * 4) for i in range(4)] == [
        0x02000112, 0x40000000, 0x12345678, 0x0100]
    wait_for(lambda: read(0xff0c0000) >= 3, "guest status and EP0 rearm")

    # Fetch the guest's full CDC ACM configuration descriptor.
    words(ep_ring + 48, [0x02000680, 0x00430000, 8, 0x30841,
                         host_buf, 0, 67, 0x10c01, 0, 0, 0, 0x1021])
    write(db + 4, 1)
    wait_for(lambda: event_done(event, ep_ring + 80), "CDC configuration descriptor")
    assert read(host_buf) == 0x00430209
    assert read(host_buf + 8) & 0xff == 50
    wait_for(lambda: read(0xff0c0000) >= 4, "guest configuration status")

    words(ep_ring + 96, [0x00010900, 0, 8, 0x841, 0, 0, 0, 0x11021])
    write(db + 4, 1)
    wait_for(lambda: event_done(event, ep_ring + 112), "SET_CONFIGURATION")
    wait_for(lambda: read(0xff0c0000) >= 5, "guest configuration rearm")

    # CDC SET_LINE_CODING carries seven bytes from host to gadget.
    line_src = 0x1038000
    words(line_src, [0x0016e360, 0x00080000])
    words(ep_ring + 128, [0x00002021, 0x00070000, 8, 0x20841,
                          line_src, 0, 7, 0x0c01, 0, 0, 0, 0x11021])
    write(db + 4, 1)
    wait_for(lambda: event_done(event, ep_ring + 160), "SET_LINE_CODING")
    assert read(0xff0c0008) == 0x0016e360

    words(inp, [0] * (33 * 8))
    write(inp + 4, 1 | (1 << 3))
    words(inp + 32, [read(output + i * 4) for i in range(8)])
    write(inp + 32, (read(inp + 32) & ~(31 << 27)) | (3 << 27))
    write(inp + 33 * 4, (512 << 16) | (6 << 3) | (3 << 1))
    write(inp + 34 * 4, bulk_ring | 1)
    write(inp + 36 * 4, 512)
    words(cmd + 32, [inp, 0, 0, (12 << 10) | (1 << 24) | 1])
    write(db, 0)
    wait_for(lambda: event_done(event, cmd + 32), "configure bulk endpoint")
    data = b""
    bulk_index = 0
    while len(data) < len(b"UART over OTG\n"):
        ptr = bulk_ring + bulk_index * 16
        words(ptr, [bulk_buf, 0, 64, (1 << 10) | (1 << 5) | (1 << 2) | 1])
        write(db + 4, 3)
        wait_for(lambda: event_done(event, ptr, 13), "guest bulk message")
        n = transfer_length(event, ptr, 64)
        data += b"".join(read(bulk_buf + i).to_bytes(4, "little")
                         for i in range(0, n, 4))[:n]
        bulk_index += 1
    assert data == b"UART over OTG\n", data

    # Configure the host's bulk OUT endpoint and send input to firmware.
    out_ring, out_src = 0x102c000, 0x1036000
    words(inp, [0] * (33 * 8))
    write(inp + 4, 1 | (1 << 2))
    words(inp + 32, [read(output + i * 4) for i in range(8)])
    write(inp + 32, (read(inp + 32) & ~(31 << 27)) | (3 << 27))
    write(inp + 25 * 4, (512 << 16) | (2 << 3) | (3 << 1))
    write(inp + 26 * 4, out_ring | 1)
    write(inp + 28 * 4, 512)
    words(cmd + 48, [inp, 0, 0, (12 << 10) | (1 << 24) | 1])
    write(db, 0)
    wait_for(lambda: event_done(event, cmd + 48), "configure bulk OUT")
    write(out_src, 0x474e4950)  # PING
    words(out_ring, [out_src, 0, 4, (1 << 10) | (1 << 5) | 1])
    write(db + 4, 2)
    wait_for(lambda: event_done(event, out_ring), "guest bulk OUT")
    wait_for(lambda: read(0xff0c0004) == 0x474e4950, "guest received PING")
    print("PASS: executing CDC ACM gadget enumerates and exchanges bulk data with xHCI")
    if "--stream" in sys.argv[3:]:
        ptr = bulk_ring + bulk_index * 16
        words(ptr, [bulk_buf, 0, 64,
                               (1 << 10) | (1 << 5) | (1 << 2) | 1])
        write(db + 4, 3)
        wait_for(lambda: event_done(event, ptr, 13), "queued PONG")
        data = b"".join(read(bulk_buf + i * 4).to_bytes(4, "little") for i in range(2))
        assert data.startswith(b"PONG\n"), data
        print("PASS: CDC queue sends a second message after the first completes")
        with open(serial_path, "rb") as serial:
            output = serial.read()
            assert output.startswith(b"UART over OTG\n"), "replayed UART output differs"
            if "--ddr" not in sys.argv[3:]:
                assert output == b"UART over OTG\n", output
        print("PASS: trapped UART output matches CDC data and reaches UART")
    if "--ddr" in sys.argv[3:]:
        wait_for(lambda: read(0xff0c0020) == 2, "DDR return under UART trap")
        with open(serial_path, "rb") as serial:
            actual = serial.read()[len(b"UART over OTG\n"):]
        assert actual, "DDR did not write UART output"
        captured = b""
        bulk_index += 1
        while len(captured) < len(actual):
            ptr = bulk_ring + bulk_index * 16
            words(ptr, [bulk_buf, 0, 2048, (1 << 10) | (1 << 5) | (1 << 2) | 1])
            write(db + 4, 3)
            wait_for(lambda: event_done(event, ptr, 13), "captured DDR UART data")
            n = transfer_length(event, ptr, 2048)
            captured += b"".join(read(bulk_buf + i).to_bytes(4, "little")
                                 for i in range(0, n, 4))[:n]
            bulk_index += 1
        assert captured == actual, "DDR UART and USB streams differ"
        assert read(0xff0c0024) == 0, "CDC queue overflow"
        print(f"PASS: DDR returned; all {len(captured)} UART bytes match USB, no drops")
        if linux:
            serial_start = Path(serial_path).stat().st_size
            write(0xff0c0030, 1)
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline and proc.poll() is None:
                output = Path(serial_path).read_bytes()
                if b"INIT-OK" in output:
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError(f"Tiny Core init missing; EL2 diagnostics: "
                                   f"{[hex(read(a)) for a in (0xff0c0060, 0xff0c0064, 0xff0c0068, 0xff0c006c)]}; "
                                   f"serial tail: {output[-500:]!r}")
            linux_serial = output[serial_start:]
            assert b"Linux version" in linux_serial
            captured = b""
            while len(captured) < len(linux_serial):
                ptr = bulk_ring + bulk_index * 16
                words(ptr, [bulk_buf, 0, 2048, (1 << 10) | (1 << 5) | (1 << 2) | 1])
                write(db + 4, 3)
                wait_for(lambda: event_done(event, ptr, (1, 13)), "captured Linux UART")
                n = transfer_length(event, ptr, 2048)
                captured += b"".join(read(bulk_buf + i).to_bytes(4, "little")
                                     for i in range(0, n, 4))[:n]
                bulk_index += 1
            if captured[:len(linux_serial)] != linux_serial:
                idx = next((i for i, (a, b) in enumerate(zip(captured, linux_serial)) if a != b), 0)
                raise RuntimeError(f"Linux USB/UART mismatch at {idx}: "
                                   f"USB={captured[max(0, idx-32):idx+64]!r}, "
                                   f"UART={linux_serial[max(0, idx-32):idx+64]!r}, "
                                   f"drops={read(0xff0c0024)}")
            drops = read(0xff0c0024)
            assert drops == 0, f"CDC queue overflow: {drops} bytes"
            print(f"PASS: Tiny Core reached INIT-OK; all {len(linux_serial)} Linux UART "
                  f"bytes through INIT-OK match USB; {read(0xff0c006c)} EL2 traps, no drops")
finally:
    proc.terminate()
    proc.wait(timeout=10)
    tmp.cleanup()
