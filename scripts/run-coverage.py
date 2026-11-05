#!/usr/bin/env python3
"""P19 coverage audit boot: run the Armbian KDE image on the all-devices
DTB, wait for the serial root shell, then dump what the guest did with
every device and quit QEMU cleanly.

Collection (one serial session, markers make the sections sliceable):
  ===COV-DEVICES===   bus|name|/devicetree/path|driver for every sysfs
                      device (bound drivers and DT paths for the join)
  ===COV-DEFERRED===  debugfs devices_deferred (probe storm detector)
  ===COV-DMESG===     journalctl -k -b (full boot log, incl. probe
                      failures and their kernel timestamps)
  ===COV-LSMOD===     what module state the audit ended in

Host side afterwards:
  scripts/dtb-coverage.py <dtb> --guest logs/p19-guest.txt
  grep of qemu's -d unimp,guest_errors,invalid_mem log

Usage:
  python3 scripts/run-coverage.py [--image IMG] [--dtb DTB] [--qemu PATH]
"""
import argparse
import importlib.util
import os
import signal
import socket
import subprocess
import sys
import time

signal.signal(signal.SIGTTIN, signal.SIG_IGN)
signal.signal(signal.SIGTTOU, signal.SIG_IGN)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DEFAULT_IMG = "Armbian_26.8.1_Orangepi5_resolute_current_6.18.43_kde-plasma_desktop.img"
DEFAULT_DTB = "images/armbian/rk3588s-orangepi-5-all.dtb"

# The markers are assembled from concatenated words so that the tty's echo
# of this heredoc (the console echoes every line we type) never contains the
# literal marker - otherwise the first occurrence we slice on would be our
# own typed line instead of the guest's output.
GUEST_SCRIPT = r"""sleep __SETTLE__
cat >/tmp/p19.sh <<'COVEOF'
echo ===COV-"DMESG"===
if command -v journalctl >/dev/null 2>&1; then
  journalctl -k -b --no-pager -o short-precise 2>/dev/null || dmesg
else
  dmesg
fi
echo ===COV-"DEFERRED"===
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
cat /sys/kernel/debug/devices_deferred 2>/dev/null
echo ===COV-"LSMOD"===
lsmod
echo ===COV-"DEVICES"===
for d in /sys/bus/*/devices/*; do
  [ -e "$d" ] || continue
  d=${d%/}
  bus=${d#/sys/bus/}; bus=${bus%%/*}
  name=${d##*/}
  if [ -e "$d/driver" ]; then
    p=$(readlink -f "$d/driver"); drv=${p##*/}
  else
    drv=UNBOUND
  fi
  on=$(readlink -f "$d/of_node" 2>/dev/null)
  case "$on" in
    */devicetree/base*) dt=${on##*/devicetree/base} ;;
    *) dt=- ;;
  esac
  echo "$bus|$name|$dt|$drv"
done
echo ===COV-"END"===
COVEOF
bash /tmp/p19.sh
"""

MARKER_LOGIN = b"root@orangepi5:~#"
MARKER_PROMPT = b"login:"
MARKER_END = b"===COV-END==="


def load_rundesktop():
    path = os.path.join(HERE, "run-desktop.py")
    spec = importlib.util.spec_from_file_location("run_desktop", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", nargs="?", default=DEFAULT_IMG)
    ap.add_argument("--dtb", default=os.path.join(ROOT, DEFAULT_DTB))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--tag", default="p19", help="log file tag")
    ap.add_argument("--settle", type=int, default=110,
                    help="seconds to wait after login before dumping")
    return ap.parse_args()


def monitor_quit(sock_path, timeout=45):
    """Last-resort clean stop: guest never powers off by itself here."""
    for _ in range(int(timeout / 0.5)):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect(sock_path)
            s.sendall(b"quit\n")
            time.sleep(0.3)
            s.close()
            return True
        except OSError:
            time.sleep(0.5)
    return False


def main():
    args = parse_args()
    rd = load_rundesktop()
    qemu = rd.pick_qemu(args.qemu)
    print(f"[p19] QEMU: {qemu}", flush=True)

    bl31 = os.path.join(ROOT, "atf/build/rk3588/release/bl31/bl31.elf")
    uboot = os.path.join(ROOT, "u-boot/u-boot.bin")
    qdtb = os.path.join(ROOT, "u-boot/dts/dt.dtb")
    vmlinuz = os.path.join(ROOT, "images/armbian/vmlinuz-6.18.43-current-rockchip64")
    image = (args.image if os.path.isabs(args.image)
             else os.path.join(ROOT, args.image))
    for p, w in ((bl31, "TF-A bl31"), (uboot, "u-boot"), (qdtb, "QEMU dtb"),
                 (vmlinuz, "kernel"), (args.dtb, "all-devices DTB"),
                 (image, "image")):
        rd.need(p, w)

    os.makedirs(os.path.join(ROOT, "logs"), exist_ok=True)
    serial_path = os.path.join(ROOT, "logs", f"{args.tag}-serial.txt")
    guest_path = os.path.join(ROOT, "logs", f"{args.tag}-guest.txt")
    qlog = os.path.join(ROOT, "logs", f"{args.tag}-qemu.log")
    mon = f"/tmp/{args.tag}mon.sock"
    for p in (serial_path, guest_path, qlog):
        try:
            os.remove(p)
        except FileNotFoundError:
            pass

    env = os.environ.copy()
    if qemu.endswith(".AppImage") and not os.path.exists("/dev/fuse"):
        env["APPIMAGE_EXTRACT_AND_RUN"] = "1"

    cmd = [
        qemu, "-M", "rk3588,vgpu=on,pcie-link=off", "-serial", "stdio",
              "-display", "none",
        "-monitor", f"unix:{mon},server,nowait",
        "-bios", bl31,
        "-device", f"loader,file={uboot},addr=0x800000,force-raw=on",
        "-device", f"loader,file={vmlinuz},addr=0x02000000,force-raw=on",
        "-device", f"loader,file={args.dtb},addr=0x0a100000,force-raw=on",
        "-dtb", qdtb,
        "-netdev", "user,id=net0",
        "-device", "virtio-net-device,netdev=net0",
        "-device", "virtio-gpu-device,id=vgpu,xres=1920,yres=1080",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
        "-drive", f"if=sd,file={image},format=raw,unit=1",
        "-d", "unimp,guest_errors,invalid_mem", "-D", qlog,
    ]

    t0 = time.time()
    g = rd.Guest(cmd, env)

    def fail(msg):
        """Keep the evidence: save the whole serial log before bailing."""
        try:
            with open(serial_path, "wb") as fh:
                fh.write(g.since(0))
        except OSError:
            pass
        g.stop()
        sys.exit(msg)

    err = rd.dance(g)
    if err:
        fail(f"[p19] boot failed at the U-Boot dance: {err}")

    # kernel started: wait for the serial root shell (systemd autologin)
    if not g.wait(MARKER_LOGIN, 420, 0):
        g.send("\r")                    # prompt may have landed on the edge
        if not g.wait(MARKER_LOGIN, 60, 0):
            fail("[p19] no serial root shell within 420s of booti")
    print(f"[p19] root shell after {time.time() - t0:.1f}s", flush=True)

    m = g.mark()
    # CR line endings: the console converts CR->NL on input in canonical mode
    g.send(GUEST_SCRIPT.replace("\n", "\r").replace("__SETTLE__",
                                                     str(args.settle)))
    if not g.wait(MARKER_END, 300, m):
        fail("[p19] guest dump never finished (no ===COV-END===)")

    # slice the dump out of the raw serial stream (markers are only ever
    # in the guest's *output*, never in the echoed script text); the
    # device listing runs LAST in the script so late binders are final.
    data = g.since(m).replace(b"\r", b"")
    start = data.find(b"===COV-DMESG===")
    end = data.find(MARKER_END) + len(MARKER_END)
    if start < 0 or end <= len(MARKER_END):
        fail("[p19] could not locate the dump markers")
    with open(guest_path, "wb") as fh:
        fh.write(data[start:end])
    print(f"[p19] guest dump -> {guest_path} "
          f"({end - start} bytes, boot+dump {time.time() - t0:.1f}s)",
          flush=True)

    # stop QEMU: guest poweroff first, monitor quit as the fallback
    g.send("poweroff -f\r")
    try:
        g.proc.wait(timeout=60)
    except subprocess.TimeoutExpired:
        print("[p19] guest poweroff did not stop QEMU - monitor quit",
              flush=True)
        monitor_quit(mon)
        try:
            g.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            g.proc.kill()
            g.proc.wait(timeout=10)
    rc = g.proc.returncode
    print(f"[p19] qemu rc={rc} (total {time.time() - t0:.1f}s)", flush=True)

    # host-side summary of the QEMU log
    try:
        qtxt = open(qlog, encoding="utf-8", errors="replace").read()
    except FileNotFoundError:
        qtxt = ""
    bad = [ln for ln in qtxt.splitlines()
           if "guest_errors" in ln or "invalid" in ln or "unassigned" in ln]
    print(f"[p19] qemu -d log: {len(qtxt.splitlines())} lines, "
          f"{len(bad)} suspicious", flush=True)
    for ln in bad[:20]:
        print("   " + ln, flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
