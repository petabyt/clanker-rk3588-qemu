#!/usr/bin/env python3
"""Exercise the external rk sram-experiments USB2 ring probe.

The probe assumes the no-op completion is event 1.  Adapt that one check in
an ephemeral copy: QEMU can deliver the completion as event 0.  This is not
a modification of the hardware firmware or a claim of identical event order.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rk", type=Path, required=True,
                        help="rk checkout containing demo/sram-fb")
    parser.add_argument("--qemu", type=Path,
                        default=Path("build/qemu/qemu-system-aarch64"))
    args = parser.parse_args()
    qemu = args.qemu.resolve()
    demo = args.rk.resolve() / "demo/sram-fb"
    source = (demo / "usb-probe.c").read_text()
    old = ("if (REG(DMA_BASE + 0x2010) == DMA_BASE + 0x1000 &&\n"
           "\t    (REG(DMA_BASE + 0x2018) >> 24) == 1 &&\n"
           "\t    ((REG(DMA_BASE + 0x201c) >> 10) & 63) == 33)")
    new = ("if (REG(DMA_BASE + 0x2000) == DMA_BASE + 0x1000 &&\n"
           "\t    (REG(DMA_BASE + 0x2008) >> 24) == 1 &&\n"
           "\t    ((REG(DMA_BASE + 0x200c) >> 10) & 63) == 33)")
    if source.count(old) != 1:
        raise SystemExit("unrecognized USB probe; check event handling manually")
    scratch = Path(os.environ.get("TMPDIR", "/tmp/opencode"))
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sram-xhci-", dir=scratch) as tmp:
        tmp = Path(tmp)
        (tmp / "probe.c").write_text(source.replace(old, new))
        flags = ["-Os", "-march=armv8-a", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-ffunction-sections", "-fdata-sections"]
        subprocess.run(["aarch64-linux-gnu-gcc", *flags, "-c", "pre-ddr.S",
                        "-o", str(tmp / "entry.o")], cwd=demo, check=True)
        subprocess.run(["aarch64-linux-gnu-gcc", *flags, "-mgeneral-regs-only",
                        "-DXHCI_TEST", "-DXHCI_RING_TEST", "-c",
                        str(tmp / "probe.c"), "-o", str(tmp / "probe.o")],
                       check=True)
        subprocess.run(["aarch64-linux-gnu-ld", "-T", str(demo / "pre-ddr.ld"),
                        "--gc-sections", str(tmp / "entry.o"), str(tmp / "probe.o"),
                        "-o", str(tmp / "probe.elf")], check=True)
        subprocess.run(["aarch64-linux-gnu-objcopy", "-O", "binary",
                        str(tmp / "probe.elf"), str(tmp / "probe.bin")], check=True)
        cmd = [str(qemu), "-M", "rk3588,sram-boot=on", "-display", "none",
               "-serial", "stdio", "-monitor", "none", "-bios", str(tmp / "probe.bin"),
               "-device", "usb-serial,bus=usb-a.0,chardev=test",
               "-chardev", "null,id=test", "-d", "unimp,guest_errors",
               "-D", str(tmp / "qemu.log")]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            out, err = proc.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            proc.terminate()
            out, err = proc.communicate(timeout=5)
        text = out.decode(errors="replace")
        expected = ["DWC3 GSNPSID=5533300a", "No-op command completed successfully",
                    "address device event status=01000000", "SET_ADDRESS completion=01000000",
                    "descriptor event status=01000000", "descriptor word=01100112"]
        for marker in expected:
            if marker not in text:
                raise SystemExit(f"missing {marker!r}\n{text}\n{err.decode(errors='replace')}")
        if (tmp / "qemu.log").read_bytes():
            raise SystemExit("QEMU diagnostics:\n" + (tmp / "qemu.log").read_text())
        print("SRAM xHCI USB2: no-op, slot, address and descriptor DMA passed")
        print("Probe adapted for QEMU no-op event position; not a hardware-identical event trace")


if __name__ == "__main__":
    main()
