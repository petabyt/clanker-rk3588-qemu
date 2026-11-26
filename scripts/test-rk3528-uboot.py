#!/usr/bin/env python3
"""Direct U-Boot test, optionally preceded by the experimental DDR shim.

Neither mode tests SPI, SPL, TF-A or the real BootROM.
"""
import hashlib
import pathlib
import os
import selectors
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parent.parent
image = root / "build/rk3528/u-boot.bin"
if len(sys.argv) > 2:
    sys.exit("usage: test-rk3528-uboot.py [DDR_BINARY]")
ddr = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else None
if ddr and hashlib.sha256(ddr.read_bytes()).hexdigest() != (
        "a4a6d866f789d0c063334e4863ac7b4ed11bcdc735ea48fe3c0200575592edf8"):
    sys.exit("Unexpected RK3528 DDR binary SHA-256")
command = [str(root / "build/qemu/qemu-system-aarch64"), "-M", "rk3528",
           "-nographic", "-monitor", "none"]
if ddr:
    command += ["-bios", str(ddr), "-device",
                f"loader,file={image},addr=0x800000,force-raw=on"]
else:
    command += ["-device", f"loader,file={image},addr=0x800000,cpu-num=0"]
proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT)
selector = selectors.DefaultSelector()
selector.register(proc.stdout, selectors.EVENT_READ)
data = b""
sent = False
command_sent = False
deadline = time.monotonic() + 45
try:
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            break
        for key, _ in selector.select(timeout=0.5):
            chunk = os.read(key.fd, 4096)
            if not chunk:
                break
            data += chunk
            if not sent and b"Hit any key to stop autoboot" in data:
                proc.stdin.write(b" ")
                proc.stdin.flush()
                sent = True
            if sent and not command_sent and b"=> " in data:
                proc.stdin.write(b"echo RK3528_HELLO\n")
                proc.stdin.flush()
                command_sent = True
            if b"\nRK3528_HELLO\r\n" in data:
                if ddr and not (0 <= data.find(b"fwver: v1.14") <
                                data.find(b"\nout\r\n") < data.find(b"U-Boot")):
                    sys.exit("DDR banner/return missing or out of order")
                sys.stdout.buffer.write(data)
                print("RK3528 DDR shim + U-Boot: PASS" if ddr else
                      "RK3528 U-Boot command test: PASS")
                sys.exit(0)
    sys.stdout.buffer.write(data)
    print("RK3528 U-Boot command test: FAIL", file=sys.stderr)
    sys.exit(1)
finally:
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
