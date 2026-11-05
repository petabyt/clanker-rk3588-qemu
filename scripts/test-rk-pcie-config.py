#!/usr/bin/env python3
"""Exercise the experimental RK3588 PCIe configuration path with qtest."""

import argparse
import os
import signal
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="build/qemu/qemu-system-aarch64")
    args = parser.parse_args()
    proc = subprocess.Popen([
        args.qemu, "-M", "rk3588,pcie-experimental=on",
        "-device", "virtio-gpu-pci,bus=dw-pcie,addr=0",
        "-display", "none", "-serial", "none", "-monitor", "none", "-S",
        "-qtest", "stdio", "-qtest-log", "/dev/null",
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
        start_new_session=True)

    def command(text):
        proc.stdin.write(text + "\n")
        proc.stdin.flush()
        reply = proc.stdout.readline().split()
        if not reply or reply[0] != "OK":
            raise RuntimeError(f"qtest {text}: {reply}")
        return int(reply[1], 16) if len(reply) > 1 else None

    def write(offset, value):
        command(f"writel {0xa41000000 + offset:#x} {value:#x}")

    try:
        assert command("readl 0xa41000000") == 0xabcd16c3
        # Root bus 0x40, secondary bus 0x41, subordinate bus 0x41.
        write(0x18, 0x00414140)
        # Outbound region zero: config type 0, CPU window 0xf4000000,
        # downstream bus 0x41, device 0, function 0.
        for offset, value in ((0x900, 0), (0x90c, 0xf4000000),
                              (0x910, 0), (0x914, 0xf40fffff),
                              (0x918, 0x41000000), (0x91c, 0),
                              (0x904, 4), (0x908, 0x80000000)):
            write(offset, value)
        assert command("readl 0xf4000000") == 0x10501af4
        write(0x918, 0x41010000)  # absent device
        assert command("readl 0xf4000000") == 0xffffffff
        write(0x918, 0x41000000)
        # Map the endpoint's 64-bit BAR4 at 0xa00000000 and open the
        # root bridge's matching prefetchable window.
        command("writel 0xf4000020 0")
        command("writel 0xf4000024 0xa")
        command("writew 0xf4000004 2")
        write(0x24, 0x00010001)
        write(0x28, 0xa)
        write(0x2c, 0xa)
        command("writew 0xa41000004 2")
        # The stock DT describes a 64-bit memory window in region 1.
        for offset, value in ((0x900, 1), (0x90c, 0), (0x910, 0xa),
                              (0x914, 0x3fffffff), (0x918, 0),
                              (0x91c, 0xa), (0x904, 0x100),
                              (0x908, 0x80000000)):
            write(offset, value)
        assert command("readl 0xa41000908") & 0x80000000
        # Virtio common configuration's device_feature_select register.
        command("writel 0xa00000000 1")
        assert command("readl 0xa00000000") == 1
        # SMMUv3 IDR0 must be implemented, not the zero-valued stub.
        assert command("readl 0xfc900000") != 0
        print("PASS: PCI configuration, 64-bit BAR access, SMMU ID")
    finally:
        os.killpg(proc.pid, signal.SIGTERM)
        proc.communicate(timeout=10)


if __name__ == "__main__":
    main()
