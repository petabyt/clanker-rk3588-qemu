#!/usr/bin/env python3
"""Boot the Armbian KDE Plasma desktop on the rk3588 QEMU model.

Shows the session in a GTK window on your desktop (keyboard and mouse
work through virtio input devices).  The guest's serial console is
mirrored into this terminal - once the kernel is up you can type into
it (login: root, empty password).  Ctrl-C stops QEMU.

Prerequisites (inside the repo this script lives in):
  rk3588-qemu-x86_64.AppImage   (or build/qemu/qemu-system-aarch64, or --qemu)
  atf/build/rk3588/release/bl31/bl31.elf
  u-boot/u-boot.bin, u-boot/dts/dt.dtb
  images/armbian/vmlinuz-6.18.43-current-rockchip64
  images/armbian/rk3588s-orangepi-5-vgpu-boot.dtb
  <image>  - the prepared Armbian KDE image (root pw empty, user opi
             autologin into the Plasma Wayland session)

Usage:  python3 scripts/run-desktop.py [image.img] [--qemu PATH]

Expect ~4 min to the sddm start and another ~5-10 min until the Plasma
session has fully come up (everything renders in software).
"""
import argparse
import os
import signal
import subprocess
import sys
import threading
import time

# Never let terminal job-control signals (SIGTTIN/SIGTTOU) stop us: this
# script may run under wrappers that leave it in a background process group,
# and a group-stop here would freeze the boot mid-dance.
signal.signal(signal.SIGTTIN, signal.SIG_IGN)
signal.signal(signal.SIGTTOU, signal.SIG_IGN)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DEFAULT_IMG = "Armbian_26.8.1_Orangepi5_resolute_current_6.18.43_kde-plasma_desktop.img"

BOOTARGS = "console=ttyS2,1500000 root=/dev/mmcblk1p1 rootwait rw loglevel=4 consoleblank=0"
SETENV = "setenv bootargs " + BOOTARGS
BOOTI = "booti 0x02000000 - 0x0a100000"
STOCK_MARKERS = (b"Welcome to", b"orangepi5 login:")


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("image", nargs="?", default=DEFAULT_IMG,
                   help="Armbian KDE desktop image (.img)")
    p.add_argument("--qemu", default=None,
                   help="qemu-system-aarch64 binary (default: auto)")
    return p.parse_args()


def pick_qemu(explicit):
    candidates = [explicit] if explicit else [
        os.path.join(ROOT, "rk3588-qemu-x86_64.AppImage"),
        os.path.join(ROOT, "build", "qemu", "qemu-system-aarch64"),
    ]
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    sys.exit("error: no QEMU found - expected rk3588-qemu-x86_64.AppImage or\n"
             "       build/qemu/qemu-system-aarch64 (run `make qemu` / `make appimage`),\n"
             "       or pass --qemu /path/to/qemu-system-aarch64")


def need(path, what):
    if not os.path.isfile(path):
        sys.exit(f"error: missing {what}: {path}")


class Guest:
    """QEMU with a mirrored serial console: pump thread tees guest output
    to this terminal and keeps a rolling window for pattern matching."""

    def __init__(self, cmd, env=None):
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT,
                                     cwd=ROOT, env=env)
        self._buf = bytearray()
        self._total = 0
        self._lock = threading.Lock()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        out = sys.stdout.buffer
        while True:
            try:
                chunk = self.proc.stdout.read1(4096)
            except (ValueError, OSError):
                break
            if not chunk:
                break
            with self._lock:
                self._buf.extend(chunk)
                self._total += len(chunk)
                if len(self._buf) > 65536:
                    del self._buf[: len(self._buf) - 65536]
            try:
                sys.stdout.flush()
                out.write(chunk)
                out.flush()
            except ValueError:
                pass

    def send(self, s):
        try:
            self.proc.stdin.write(s.encode())
            self.proc.stdin.flush()
        except (BrokenPipeError, ValueError, OSError):
            pass

    def mark(self):
        return self._total

    def since(self, mark):
        with self._lock:
            data = bytes(self._buf)
            start = self._total - len(data)
        if mark <= start:
            return data
        return data[mark - start:]

    def wait(self, pattern, timeout, mark=0):
        end = time.time() + timeout
        while time.time() < end:
            if pattern in self.since(mark):
                return True
            if self.proc.poll() is not None:
                return pattern in self.since(mark)
            time.sleep(0.15)
        return pattern in self.since(mark)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def dance(g):
    """Reach the U-Boot prompt, set bootargs, launch the kernel.
    Every step is verified by the guest's own echo - no blind timing.
    Returns None on success, a failure reason string otherwise."""
    # Phase 1: interrupt autoboot, land on the prompt (CRs, as the
    # proven drivers do, but gated on observed output).
    end = time.time() + 90
    while time.time() < end:
        if g.proc.poll() is not None:
            return "QEMU exited during boot"
        tail = g.since(0)
        if b"=> " in tail:
            break
        if any(m in tail for m in STOCK_MARKERS):
            return "the stock boot.scr booted (prompt never reached)"
        g.send("\r")
        time.sleep(0.35)
    else:
        return "no U-Boot prompt within 90s"

    # Phase 2: setenv, verified by U-Boot echoing the line back.
    for _ in range(4):
        g.send("\r")
        time.sleep(0.5)
        m = g.mark()
        g.send(SETENV + "\r")
        if g.wait(b"setenv bootargs console=", 7, m):
            break
    else:
        return "U-Boot did not echo the bootargs command"
    time.sleep(0.8)

    # Phase 3: booti, verified by 'Starting kernel ...'.
    for _ in range(3):
        g.send("\r")
        time.sleep(0.5)
        m = g.mark()
        g.send(BOOTI + "\r")
        if g.wait(b"Starting kernel", 25, m):
            return None
    return "booti did not start the kernel"


def main():
    args = parse_args()
    qemu = pick_qemu(args.qemu)
    print(f"[run-desktop] QEMU: {qemu}", flush=True)

    # AppImages mount via FUSE; hosts without /dev/fuse (containers) need
    # the extract-and-run fallback.
    env = os.environ.copy()
    if qemu.endswith(".AppImage") and not os.path.exists("/dev/fuse"):
        env["APPIMAGE_EXTRACT_AND_RUN"] = "1"
        print("[run-desktop] no /dev/fuse - using APPIMAGE_EXTRACT_AND_RUN=1",
              flush=True)

    bl31 = os.path.join(ROOT, "atf/build/rk3588/release/bl31/bl31.elf")
    uboot = os.path.join(ROOT, "u-boot/u-boot.bin")
    qdtb = os.path.join(ROOT, "u-boot/dts/dt.dtb")
    vmlinuz = os.path.join(ROOT, "images/armbian/vmlinuz-6.18.43-current-rockchip64")
    gdtb = os.path.join(ROOT, "images/armbian/rk3588s-orangepi-5-vgpu-boot.dtb")
    for p, w in ((bl31, "TF-A bl31"), (uboot, "u-boot"), (qdtb, "QEMU dtb"),
                 (vmlinuz, "kernel Image"), (gdtb, "vgpu boot dtb"), (args.image, "image")):
        need(p, w)

    have_desktop = bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))
    display = ["-display", "gtk"] if have_desktop else ["-vnc", ":0"]
    print("[run-desktop] display: " + ("GTK window (this terminal keeps the serial console)"
          if have_desktop else "VNC on localhost:5900 (no X/Wayland detected here)"),
          flush=True)

    cmd = [
        qemu, "-M", "rk3588,vgpu=on", "-serial", "stdio",
        "-bios", bl31,
        "-device", f"loader,file={uboot},addr=0x800000,force-raw=on",
        "-device", f"loader,file={vmlinuz},addr=0x02000000,force-raw=on",
        "-device", f"loader,file={gdtb},addr=0x0a100000,force-raw=on",
        "-dtb", qdtb,
        "-netdev", "user,id=net0",
        "-device", "virtio-net-device,netdev=net0",
        "-device", "virtio-gpu-device,id=vgpu,xres=1920,yres=1080",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
        "-drive", f"if=sd,file={args.image},format=raw,unit=1",
    ] + display

    guest = None
    for attempt in range(1, 4):
        guest = Guest(cmd, env)
        err = dance(guest)
        if err is None:
            break
        tail = guest.since(0)
        if b"not available" in tail and b"gtk" in tail:
            print("[run-desktop] display 'gtk' not available - retrying with VNC",
                  flush=True)
            display = ["-vnc", ":0"]
            cmd = cmd[:-2] + display
        print(f"[run-desktop] boot attempt {attempt}/3 failed: {err} - restarting QEMU",
              flush=True)
        guest.stop()
    else:
        sys.exit("[run-desktop] could not boot after 3 attempts - see the console "
                 "output above for the reason")

    print(flush=True)
    print("[run-desktop] kernel launched - sddm starts in ~4 min, the Plasma", flush=True)
    print("[run-desktop] session settles ~5-10 min after that (software rendering).", flush=True)
    print("[run-desktop] interact in the QEMU window; the console below is the guest", flush=True)
    print("[run-desktop] serial (login: root, empty password); Ctrl-C stops.", flush=True)
    print(flush=True)

    # Relay this terminal's lines to the guest serial console.
    def relay():
        try:
            for line in sys.stdin:
                guest.send(line.replace("\n", "\r"))
        except (ValueError, OSError):
            pass
    threading.Thread(target=relay, daemon=True).start()

    try:
        guest.proc.wait()
    except KeyboardInterrupt:
        print("\n[run-desktop] stopping QEMU ...", flush=True)
        guest.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
