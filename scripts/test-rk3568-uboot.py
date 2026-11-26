#!/usr/bin/env python3
"""Direct-load test; does not test BootROM, DDR, storage or TF-A."""
import pathlib
import os
import selectors
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parent.parent
image = root / "build/rk3568/u-boot.bin"
command = [str(root / "build/qemu/qemu-system-aarch64"), "-M", "rk3568",
           "-nographic", "-monitor", "none", "-device",
           f"loader,file={image},addr=0x800000,cpu-num=0"]
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
                proc.stdin.write(b"echo RK3568_HELLO\n")
                proc.stdin.flush()
                command_sent = True
            if b"\nRK3568_HELLO\r\n" in data:
                sys.stdout.buffer.write(data)
                print("RK3568 U-Boot command test: PASS")
                sys.exit(0)
    sys.stdout.buffer.write(data)
    print("RK3568 U-Boot command test: FAIL", file=sys.stderr)
    sys.exit(1)
finally:
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
