#!/usr/bin/env python3
"""Check ROM -> authenticated DDR -> SPL using a disposable SD snapshot."""

import argparse
import os
import selectors
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--qemu", default="build/qemu/qemu-system-aarch64")
parser.add_argument("--rom", default="private/rk3588_bootrom.bin")
parser.add_argument("--image", required=True)
parser.add_argument("--timeout", type=int, default=90)
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix="rk3588-rom-") as tmp:
    env = os.environ.copy()
    env["TMPDIR"] = tmp
    cmd = [args.qemu, "-M", f"rk3588,bootrom={args.rom}", "-smp", "8",
           "-display", "none", "-serial", "stdio", "-monitor", "none",
           "-snapshot", "-drive", f"if=sd,format=raw,file={args.image}"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env)
    output = bytearray()
    deadline = time.monotonic() + args.timeout
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(proc.stdout, selectors.EVENT_READ)
            while time.monotonic() < deadline:
                events = selector.select(max(0, deadline - time.monotonic()))
                if not events:
                    break
                data = os.read(proc.stdout.fileno(), 4096)
                if not data:
                    break
                output.extend(data)
                if b"U-Boot SPL " in output:
                    break
        if b"DDR " not in output or b"U-Boot SPL " not in output:
            raise SystemExit("FAIL: ROM did not reach DDR and SPL:\n" +
                             output.decode(errors="replace"))
        print("PASS: real BootROM loaded DDR and reached SPL from SD")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
