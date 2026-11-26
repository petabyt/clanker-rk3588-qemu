#!/usr/bin/env python3
"""Boot a disposable SPI NOR image through the real ROM to U-Boot."""

import argparse
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--qemu", default="build/qemu/qemu-system-aarch64")
parser.add_argument("--rom", default="private/rk3588_bootrom.bin")
parser.add_argument("--idb", default="experiments/u-boot-opi5/idbloader-spi.img")
parser.add_argument("--fit", default="experiments/u-boot-opi5/u-boot.itb")
parser.add_argument("--timeout", type=int, default=90)
args = parser.parse_args()

idb = Path(args.idb).read_bytes()
fit = Path(args.fit).read_bytes()
if len(idb) > 0x60000 or len(fit) > 8 * 1024 * 1024 - 0x60000:
    raise SystemExit("SPI image components do not fit their flash regions")

with tempfile.TemporaryDirectory(prefix="rk3588-spi-", dir="/tmp/opencode") as tmp:
    flash = Path(tmp) / "flash.bin"
    image = bytearray(b"\xff" * (8 * 1024 * 1024))
    image[:len(idb)] = idb
    image[0x60000:0x60000 + len(fit)] = fit
    flash.write_bytes(image)
    cmd = [args.qemu, "-M", f"rk3588,bootrom={args.rom}", "-smp", "8",
           "-display", "none", "-serial", "stdio", "-monitor", "none",
           "-drive", f"if=mtd,format=raw,file={flash}",
           "-d", "unimp,guest_errors", "-D", str(Path(tmp) / "qemu.log")]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
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
                if b"\nU-Boot 20" in output:
                    break
        required = (b"DDR ", b"U-Boot SPL ", b"Trying to boot from SPI",
                    b"sha256+ OK", b"\nU-Boot 20")
        if not all(marker in output for marker in required):
            raise SystemExit("FAIL: SPI boot did not reach U-Boot:\n" +
                             output.decode(errors="replace"))
        if (Path(tmp) / "qemu.log").stat().st_size:
            raise SystemExit("FAIL: QEMU diagnostic log is not empty:\n" +
                             (Path(tmp) / "qemu.log").read_text())
        print("PASS: ROM -> SPI DDR -> SPL -> verified FIT -> U-Boot")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
