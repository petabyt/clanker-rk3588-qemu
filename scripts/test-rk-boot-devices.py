#!/usr/bin/env python3
"""Regress RK3588 broadcast expiry and SD controller software reset."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="build/qemu/qemu-system-aarch64")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="rk-timer-", dir="/tmp/opencode") as tmp:
        monitor = str(Path(tmp) / "qmp.sock")
        proc = subprocess.Popen(
            [str((root / args.qemu).resolve()), "-M", "rk3588", "-S",
             "-display", "none", "-serial", "none", "-monitor", "none",
             "-qtest", "stdio", "-qtest-log", "/dev/null",
             "-qmp", f"unix:{monitor},server=on,wait=off"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        sock = socket.socket(socket.AF_UNIX)
        sock.settimeout(10)
        try:
            for _ in range(100):
                if proc.poll() is not None:
                    raise RuntimeError(proc.stderr.read())
                try:
                    sock.connect(monitor)
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    time.sleep(0.05)
            stream = sock.makefile("rwb", buffering=0)
            stream.readline()

            def qmp(command, arguments=None):
                stream.write((json.dumps({"execute": command,
                                          "arguments": arguments or {}}) + "\n").encode())
                while True:
                    reply = json.loads(stream.readline())
                    if "error" in reply:
                        raise RuntimeError(reply)
                    if "return" in reply:
                        return reply["return"]

            irqs = {}

            def qtest(command):
                proc.stdin.write(command + "\n")
                proc.stdin.flush()
                reply = proc.stdout.readline().strip()
                while reply.startswith("IRQ "):
                    _, level, number = reply.split()
                    irqs[int(number)] = level == "raise"
                    reply = proc.stdout.readline().strip()
                assert reply.startswith("OK"), (command, reply)
                return reply[2:].strip()

            def write(offset, value):
                qtest(f"writel {0xfeae0000 + offset:#x} {value:#x}")

            def pending():
                return int(qtest("readl 0xfeae0018"), 16)

            def run():
                qmp("cont")
                time.sleep(0.1)
                qmp("stop")

            qmp("qmp_capabilities")
            # SYSMEM firewall uses full-width RW, not GRF hiword masks.
            for address, value in ((0xfe038000, 0xffff0000),
                                   (0xfe038044, 0x00ff00ff),
                                   (0xfe0380f0, 0x00000081)):
                qtest(f"writel {address:#x} {value:#x}")
                assert int(qtest(f"readl {address:#x}"), 16) == value
            qtest("writel 0xfe038000 0")
            assert int(qtest("readl 0xfe038000"), 16) == 0
            print("PASS: SYSMEM firewall register storage (not DMA enforcement)")
            # VDEC0's first Rockchip IOMMU window: paging, stall and reset.
            iommu = 0xfdc38700
            qtest(f"writel {iommu:#x} 0x12345000")
            assert int(qtest(f"readl {iommu:#x}"), 16) == 0x12345000
            qtest(f"writel {iommu + 8:#x} 0")
            assert int(qtest(f"readl {iommu + 4:#x}"), 16) & 1
            qtest(f"writel {iommu + 8:#x} 2")
            assert int(qtest(f"readl {iommu + 4:#x}"), 16) & 4
            qtest(f"writel {iommu + 8:#x} 6")
            assert int(qtest(f"readl {iommu:#x}"), 16) == 0
            assert not int(qtest(f"readl {iommu + 4:#x}"), 16) & 5
            print("PASS: Rockchip IOMMU register paging, stall and reset")
            # xHCI DCBAAP is a 64-bit register, accessed by vendor U-Boot.
            qtest("writeq 0xfc000050 0x1122334455667788")
            assert int(qtest("readl 0xfc000050"), 16) == 0x55667788
            assert int(qtest("readl 0xfc000054"), 16) == 0x11223344
            # SDMMC domain power and memory reset are independent controls.
            def pm_read(offset):
                return int(qtest(f"readl {0xfd8d0000 + offset:#x}"), 16)

            def pm_write(offset, value):
                qtest(f"writel {0xfd8d0000 + offset:#x} {value:#x}")

            assert pm_read(0x8290) & (1 << 26)
            assert pm_read(0x81f4) & (1 << 5)
            pm_write(0x8150, 0x20002000)
            assert not pm_read(0x8290) & (1 << 26)
            pm_write(0x8150, 0x00010000)  # unrelated write-enable
            assert not pm_read(0x8290) & (1 << 26)
            pm_write(0x8150, 0x20000000)
            assert pm_read(0x8290) & (1 << 26)
            pm_write(0x81a4, 0x20002000)
            assert pm_read(0x81fc) & (1 << 5)
            pm_write(0x81a4, 0x20000000)
            assert not pm_read(0x81fc) & (1 << 5)
            devices = qmp("qom-list", {"path": "/machine/unattached"})
            sd = next(d["name"] for d in devices
                      if d["type"] == "child<rk3588-dwmshc>")
            qtest(f"irq_intercept_out /machine/unattached/{sd} sysbus-irq")
            assert (int(qtest("readl 0xfe2c0070"), 16) >> 7) & 7 == 1
            qtest("writel 0xfe2c0100 0x02000001")
            assert int(qtest("readl 0xfe2c0100"), 16) == 0x02000001
            # SD engine resets must not erase IRQ/configuration registers.
            qtest("writel 0xfe2c0024 4")
            qtest("writel 0xfe2c004c 0x207f0080")
            qtest("writel 0xfe2c0000 0x17")
            assert int(qtest("readl 0xfe2c0024"), 16) == 4
            assert int(qtest("readl 0xfe2c004c"), 16) == 0x207f0080
            assert int(qtest("readl 0xfe2c0000"), 16) == 0x10
            qtest("writel 0xfe2c002c 0x80000000")  # CMD0, no response
            assert int(qtest("readl 0xfe2c0040"), 16) == 4
            assert irqs.get(0), "CMD_DONE did not assert IRQ"
            qtest("writel 0xfe2c0044 4")
            assert int(qtest("readl 0xfe2c0040"), 16) == 0
            assert not irqs.get(0), "W1C did not lower IRQ"
            # Park CPU0 in WFI, so no guest driver can poll the timer.
            qtest("writel 0 0xd503207f")
            qtest("writel 4 0x17ffffff")
            write(0, 239999)  # 10 ms at 24 MHz
            write(0x10, 7)   # one-shot, interrupt enabled
            assert pending() == 0
            run()
            assert pending() == 1, "one-shot expiry needs a counter read"
            write(0x18, 1)
            assert pending() == 0, "W1C did not clear status"
            run()
            assert pending() == 0, "one-shot timer fired twice"
            write(0x10, 0)
            write(0x10, 5)   # periodic, interrupt enabled
            run()
            assert pending() == 1, "periodic expiry missing"
            write(0x18, 1)
            run()
            assert pending() == 1, "periodic timer did not reload"
            write(0x10, 0)
            write(0x18, 1)
            run()
            assert pending() == 0, "disabled timer fired"
            qmp("quit")
            proc.wait(10)
            assert proc.returncode == 0
            print("PASS: asynchronous one-shot, W1C, periodic reload, disable")
            print("PASS: SD software reset preserves config; CMD_DONE IRQ and W1C")
        finally:
            sock.close()
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    main()
