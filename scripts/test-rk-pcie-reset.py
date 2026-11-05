#!/usr/bin/env python3
"""Check that a machine reset disables programmed PCIe iATU windows."""

import argparse
import json
import os
import socket
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="build/qemu/qemu-system-aarch64")
    args = parser.parse_args()
    os.makedirs("/tmp/opencode", exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pcie-reset-", dir="/tmp/opencode") as tmp:
        path = os.path.join(tmp, "qmp.sock")
        proc = subprocess.Popen([
            args.qemu, "-M", "rk3588,pcie-experimental=on", "-display", "none",
            "-serial", "none", "-monitor", "none", "-S", "-qtest", "stdio",
            "-qtest-log", "/dev/null", "-qmp", f"unix:{path},server=on,wait=off",
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

        def qtest(command):
            proc.stdin.write(command + "\n")
            proc.stdin.flush()
            reply = proc.stdout.readline().split()
            if not reply or reply[0] != "OK":
                raise RuntimeError(f"qtest {command}: {reply}")
            return int(reply[1], 16) if len(reply) > 1 else None

        try:
            for command in ("writel 0xa41000900 0", "writel 0xa4100090c 0xf4000000",
                            "writel 0xa41000914 0xf40fffff", "writel 0xa41000904 4",
                            "writel 0xa41000908 0x80000000"):
                qtest(command)
            assert qtest("readl 0xa41000908") == 0x80000000
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.settimeout(10)
                sock.connect(path)
                stream = sock.makefile("rwb", buffering=0)
                json.loads(stream.readline())
                for name in ("qmp_capabilities", "system_reset"):
                    stream.write(json.dumps({"execute": name}).encode() + b"\n")
                    while True:
                        reply = json.loads(stream.readline())
                        if "event" not in reply:
                            break
                    if "error" in reply:
                        raise RuntimeError(f"QMP {name}: {reply['error']}")
            assert qtest("readl 0xa41000908") == 0
            print("PASS: machine reset disables outbound PCIe iATU")
        finally:
            proc.terminate()
            proc.communicate(timeout=10)


if __name__ == "__main__":
    main()
