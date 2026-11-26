#!/usr/bin/env python3
"""Exercise the stock Armbian kernel's SD runtime PM from a RAM rootfs."""

import argparse
import gzip
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default=str(ROOT / "build/qemu/qemu-system-aarch64"))
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("desktop", ROOT / "scripts/run-desktop.py")
    desktop = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(desktop)

    kernel = ROOT / "images/armbian/vmlinuz-6.18.43-current-rockchip64"
    dtb = ROOT / "images/armbian/rk3588s-orangepi-5-armbian.dtb"
    initrd = ROOT / "images/initramfs.bin"
    bl31 = ROOT / "experiments/atf/build/rk3588/release/bl31/bl31.elf"
    uboot = ROOT / "experiments/u-boot/u-boot.bin"
    uboot_dtb = ROOT / "experiments/u-boot/dts/dt.dtb"
    for path in (kernel, dtb, initrd, bl31, uboot, uboot_dtb):
        if not path.is_file():
            raise SystemExit(f"missing {path}")
    desktop.SETENV = "setenv bootargs console=ttyS2,1500000 rdinit=/init loglevel=4"
    env = os.environ.copy()
    if args.qemu.endswith(".AppImage"):
        env.setdefault("APPIMAGE_EXTRACT_AND_RUN", "1")
    start = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="sd-runtime-pm-", dir="/tmp/opencode") as tmp:
        tmpdir = Path(tmp)
        init = tmpdir / "init"
        init.write_text("#!/bin/busybox sh\n"
                        "/bin/busybox --install -s /bin 2>/dev/null\n"
                        "mount -t proc none /proc\n"
                        "mount -t sysfs none /sys\n"
                        "mount -t devtmpfs none /dev\n"
                        "echo SDPM_SHELL_READY\n"
                        "exec /bin/sh\n")
        init.chmod(0o755)
        archive = subprocess.run(["cpio", "-o", "-H", "newc"], input=b"init\n",
                                 cwd=tmp, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, check=True).stdout
        test_initrd = tmpdir / "initrd.img"
        test_initrd.write_bytes(initrd.read_bytes() + gzip.compress(archive))
        desktop.BOOTI = (f"booti 0x02000000 0x0b000000:{test_initrd.stat().st_size:#x} "
                         "0x0a100000")
        disk = Path(tmp) / "sd.raw"
        with disk.open("wb") as f:
            f.truncate(64 * 1024 * 1024)
        cmd = [str(Path(args.qemu).resolve()), "-M", "rk3588", "-display", "none",
               "-serial", "stdio", "-monitor", "none", "-bios", str(bl31),
               "-dtb", str(uboot_dtb),
               "-device", f"loader,file={uboot},addr=0x800000,force-raw=on",
               "-device", f"loader,file={kernel},addr=0x02000000,force-raw=on",
               "-device", f"loader,file={dtb},addr=0x0a100000,force-raw=on",
               "-device", f"loader,file={test_initrd},addr=0x0b000000,force-raw=on",
               "-drive", f"if=sd,format=raw,file={disk}"]
        guest = desktop.Guest(cmd, env)
        try:
            error = desktop.dance(guest)
            if error:
                raise RuntimeError(error)
            if not guest.wait(b"SDPM_SHELL_READY", args.timeout):
                raise RuntimeError("initramfs shell did not start")
            print(f"\n[sd-pm] shell wall time: {time.monotonic() - start:.1f}s", flush=True)
            mark = guest.mark()
            command = ("( f=/sys/bus/platform/devices/fe2c0000.mmc/power; "
                       "echo SDPM_BEGIN; "
                       "for i in $(seq 1 30); do test -b /dev/mmcblk0 && break; sleep 1; done; "
                       "test -b /dev/mmcblk0 || { echo SDPM_FAIL:no-card; exit; }; "
                       "dd if=/dev/urandom of=/tmp/pattern bs=4096 count=1 2>/dev/null; "
                       "dd if=/tmp/pattern of=/dev/mmcblk0 bs=4096 count=1 conv=fsync 2>/dev/null "
                       "|| { echo SDPM_FAIL:write; exit; }; "
                       "echo auto > $f/control; "
                       "for n in 1 2 3; do "
                       "for i in $(seq 1 30); do s=$(cat $f/runtime_status); "
                       "test $s = suspended && break; test $s = error && break; sleep 1; done; "
                       "echo SDPM_CYCLE:$n:before=$s; "
                       "test $s = suspended || { echo SDPM_FAIL:not-suspended; exit; }; "
                       "dd if=/dev/mmcblk0 of=/tmp/readback bs=4096 count=1 iflag=direct 2>/dev/null "
                       "|| { echo SDPM_FAIL:read; exit; }; "
                       "cmp /tmp/pattern /tmp/readback || { echo SDPM_FAIL:mismatch; exit; }; "
                       "echo SDPM_CYCLE:$n:after=$(cat $f/runtime_status); "
                       "done; echo SDPM_PASS )\n")
            guest.send(command)
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                output = guest.since(mark)
                if b"\r\nSDPM_PASS\r\n" in output:
                    if b"failed to set domain 'sdmmc'" in guest.since(0):
                        raise RuntimeError("SDMMC power-domain transition timed out")
                    break
                if b"\r\nSDPM_FAIL:" in output:
                    raise RuntimeError("runtime-PM cycle failed; see guest output")
                if guest.proc.poll() is not None:
                    raise RuntimeError("QEMU exited during runtime-PM cycle")
                time.sleep(0.1)
            else:
                raise RuntimeError("runtime-PM cycle did not finish")
            print(f"[sd-pm] probe wall time: {time.monotonic() - start:.1f}s", flush=True)
        finally:
            print(f"[sd-pm] elapsed host wall: {time.monotonic() - start:.1f}s",
                  flush=True)
            guest.stop()


if __name__ == "__main__":
    main()
