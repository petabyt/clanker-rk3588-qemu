#!/usr/bin/env python3
"""Check that the opt-in RK3588 ITS blocks appear at the board DT addresses."""

import argparse
import subprocess


def read_registers(qemu, enabled):
    machine = "rk3588,pcie-its=" + ("on" if enabled else "off")
    proc = subprocess.Popen(
        [qemu, "-M", machine, "-display", "none", "-serial", "none",
         "-monitor", "none", "-S", "-qtest", "stdio", "-qtest-log", "/dev/null"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
    )
    try:
        values = []
        for address in (0xfe640008, 0xfe660008):
            proc.stdin.write(f"readl {address:#x}\n")
            proc.stdin.flush()
            reply = proc.stdout.readline().split()
            if len(reply) != 2 or reply[0] != "OK":
                raise RuntimeError(f"qtest read failed: {reply}")
            values.append(int(reply[1], 16))
        return values
    finally:
        proc.terminate()
        proc.communicate(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="build/qemu/qemu-system-aarch64")
    args = parser.parse_args()
    off = read_registers(args.qemu, False)
    on = read_registers(args.qemu, True)
    if off != [0, 0] or not all(on):
        raise RuntimeError(f"unexpected ITS TYPER values: off={off}, on={on}")
    print(f"ITS TYPER: off={off}, on={[hex(value) for value in on]}")


if __name__ == "__main__":
    main()
