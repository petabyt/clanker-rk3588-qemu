#!/usr/bin/env python3
"""Run a disposable stock Armbian image with PCI virgl and local VNC."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_IMAGE = ROOT / ("Armbian_26.8.1_Orangepi5_resolute_current_6.18.43"
                        "_kde-plasma_desktop.img")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", nargs="?", type=Path, default=DEFAULT_IMAGE)
    parser.add_argument("--qemu", type=Path,
                        default=ROOT / "rk3588-qemu-x86_64.AppImage")
    parser.add_argument("--rendernode", default="/dev/dri/renderD128")
    parser.add_argument("--vnc-display", type=int, default=77,
                        help="localhost VNC display number (default: 77)")
    args = parser.parse_args()
    if not 0 <= args.vnc_display <= 999:
        parser.error("VNC display must be between 0 and 999")
    image = args.image.resolve(strict=True)
    qemu = args.qemu.resolve(strict=True)
    if not os.access(args.rendernode, os.R_OK | os.W_OK):
        sys.exit(f"render node not accessible: {args.rendernode}")

    with tempfile.TemporaryDirectory(prefix="rk3588-virgl-") as tmp:
        disk = Path(tmp) / "armbian.raw"
        print("Creating disposable raw disk copy; this can take a while...",
              flush=True)
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", "--",
                        str(image), str(disk)], check=True)
        print(f"Connect a VNC viewer to localhost:{5900 + args.vnc_display}.\n"
              "Serial console is below; Ctrl-C stops QEMU. The disk copy "
              "is deleted on exit.", flush=True)
        env = os.environ.copy()
        env.setdefault("APPIMAGE_EXTRACT_AND_RUN", "1")
        cmd = [str(qemu), "-M", "rk3588,pcie-experimental=on,hdmi-connected=off",
               "-device", "virtio-gpu-gl-pci,id=vgpu,bus=dw-pcie,addr=0.0,"
                          "multifunction=on,"
                          "xres=1280,yres=720",
               "-device", "virtio-keyboard-pci,bus=dw-pcie,addr=0.1",
               "-device", "virtio-tablet-pci,bus=dw-pcie,addr=0.2",
               "-netdev", "user,id=net0",
               "-device", "virtio-net-pci,netdev=net0,bus=dw-pcie,addr=0.3",
               "-drive", f"if=sd,format=raw,file={disk}",
               "-display", f"egl-headless,rendernode={args.rendernode}",
               "-vnc", f"127.0.0.1:{args.vnc_display}",
               "-serial", "stdio", "-monitor", "none"]
        try:
            return subprocess.call(cmd, env=env)
        except KeyboardInterrupt:
            return 130


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
