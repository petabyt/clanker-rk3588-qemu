#!/usr/bin/env python3
"""Test the experimental RK3588 OTG cable against an emulated xHCI port."""
import subprocess
import sys
import json
import socket
import tempfile
import time

qemu = sys.argv[1] if len(sys.argv) > 1 else "build/qemu/qemu-system-aarch64"
tmp = tempfile.TemporaryDirectory(prefix="rk-dwc3-", dir="/tmp/opencode")
qmp_path = tmp.name + "/qmp"
proc = subprocess.Popen(
    [qemu, "-M", "rk3588,sram-boot=on", "-bios", "futo-bootloader/rk/usb-poc/build/menu.bin",
     "-S", "-display", "none", "-serial", "none", "-monitor", "none",
     "-qtest", "stdio", "-qtest-log", "/dev/null",
     "-qmp", f"unix:{qmp_path},server=on,wait=off",
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

def read(addr):
    return int(qtest(f"readl {addr:#x}"), 16)

def write(addr, value):
    qtest(f"writel {addr:#x} {value:#x}")

try:
    port, dctl, events = 0xfc400420, 0xfc00c704, 0xfc00c40c
    assert not read(port) & 1
    write(0xfc00c400, 0x1000000)
    write(0xfc00c408, 32)
    write(0xfc00c708, 6)  # USB reset and connect-done events
    write(dctl, 0x80000000)
    assert read(port) & 1
    write(port, 0x210)  # port power and USB reset
    assert read(events) == 8
    assert read(0x1000000) == 0x101
    assert read(0x1000004) == 0x201
    write(dctl, 0)
    assert not read(port) & 1
    assert not read(0xfc00c70c) & (1 << 22)
    write(events, 8)
    assert read(0xfc00c70c) & (1 << 22)
    # A system reset must also detach and discard the controller's state.
    write(dctl, 0x80000000)
    assert read(port) & 1
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(5)
    deadline = time.monotonic() + 5
    while True:
        try:
            sock.connect(qmp_path)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.01)
    stream = sock.makefile("rwb", buffering=0)
    stream.readline()
    for command in ("qmp_capabilities", "system_reset"):
        stream.write((json.dumps({"execute": command}) + "\n").encode())
        while True:
            reply = json.loads(stream.readline())
            if "error" in reply:
                raise RuntimeError(reply)
            if "return" in reply:
                break
    assert not read(port) & 1
    assert read(0xfc00c400) == 0
    assert read(0xfc00c408) == 0
    sock.close()
    print("PASS: DWC3 cable attach, host reset events, detach, and system reset")
finally:
    proc.terminate()
    proc.wait(timeout=10)
    tmp.cleanup()
