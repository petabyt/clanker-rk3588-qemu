#!/usr/bin/env python3
"""Run the unmodified sram-experiments combined display/USB image.

Hotplug when the controller starts, so a genuine port-change event precedes
the no-op completion as assumed by this particular hardware diagnostic.
This does not test a device attached before the controller starts.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", type=Path, required=True,
                        help="unmodified pre-usb-fb-bpp08-text.bin")
    parser.add_argument("--qemu", type=Path,
                        default=Path("build/qemu/qemu-system-aarch64"))
    args = parser.parse_args()
    root = Path(os.environ.get("TMPDIR", "/tmp/opencode"))
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sram-combined-", dir=root) as d:
        tmp = Path(d)
        serial, qlog, monitor, screen = (tmp / x for x in
                                          ("serial", "qemu.log", "qmp", "screen.ppm"))
        cmd = [str(args.qemu.resolve()), "-M", "rk3588,sram-boot=on",
               "-display", "none", "-serial", f"file:{serial}",
               "-qmp", f"unix:{monitor},server=on,wait=off", "-bios",
               str(args.image.resolve()), "-chardev", "null,id=test",
               "-d", "unimp,guest_errors", "-D", str(qlog)]
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 15
            while not monitor.exists() and time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise RuntimeError("QEMU exited before QMP started")
                time.sleep(0.01)
            with socket.socket(socket.AF_UNIX) as sock:
                sock.settimeout(5)
                sock.connect(str(monitor))
                stream = sock.makefile("rwb", buffering=0)
                json.loads(stream.readline())

                def qmp(name, arguments=None):
                    request = {"execute": name}
                    if arguments is not None:
                        request["arguments"] = arguments
                    stream.write((json.dumps(request) + "\n").encode())
                    while True:
                        reply = json.loads(stream.readline())
                        if "error" in reply:
                            raise RuntimeError(str(reply["error"]))
                        if "return" in reply:
                            return reply["return"]

                qmp("qmp_capabilities")
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    result = qmp("human-monitor-command", {
                        "command-line": "xp /1wx 0xfc400020"})
                    if "0x00000001" in result:
                        break
                    time.sleep(0.0005)
                else:
                    raise RuntimeError("xHCI did not start")
                qmp("device_add", {"driver": "usb-serial", "bus": "usb-a.0",
                                   "chardev": "test", "id": "usbtest"})
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    output = serial.read_text(errors="replace") if serial.exists() else ""
                    if "USB descriptor success=" in output:
                        break
                    time.sleep(0.02)
                else:
                    raise RuntimeError("descriptor result timed out\n" + output)
                for marker in ("No-op command completed successfully",
                               "SET_ADDRESS completion=01000000",
                               "descriptor event status=01000000",
                               "descriptor word=01100112",
                               "USB descriptor success=0000000000000001"):
                    if marker not in output:
                        raise RuntimeError("missing " + marker + "\n" + output)
                if output.count("guards OK (CPU view only)") < 2:
                    raise RuntimeError("missing framebuffer guard checks")
                qmp("screendump", {"filename": str(screen), "device": "hdmi"})
                with screen.open("rb") as f:
                    if [f.readline(), f.readline(), f.readline()] != [
                            b"P6\n", b"1920 1080\n", b"255\n"]:
                        raise RuntimeError("unexpected display geometry")
                    pixels = f.read()
                if b"\xff\xff\xff" not in pixels or b"\x00\x00\x00" not in pixels:
                    raise RuntimeError("black/white framebuffer pixels missing")
                if qlog.read_bytes():
                    raise RuntimeError("QEMU diagnostics:\n" + qlog.read_text())
                qmp("quit")
                if proc.wait(timeout=5) != 0:
                    raise RuntimeError("QEMU failed to exit")
                print("PASS: unmodified combined image, 1920x1080 black/white display, "
                      "two guard checks and USB descriptor DMA")
                print("USB device hotplugged after xHCI run; pre-attached event order differs")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            if proc.stderr:
                proc.stderr.close()


if __name__ == "__main__":
    main()
