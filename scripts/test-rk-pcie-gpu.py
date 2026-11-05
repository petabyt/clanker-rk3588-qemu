#!/usr/bin/env python3
"""Boot the unchanged Armbian disk and verify PCIe GPU DMA, MSI-X and scanout."""

import argparse
import json
import os
import re
import select
import socket
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
IMAGE = ROOT / ("Armbian_26.8.1_Orangepi5_resolute_current_6.18.43"
                "_kde-plasma_desktop.img")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="build/qemu/qemu-system-aarch64")
    parser.add_argument("--image", default=str(IMAGE))
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("--reboot", action="store_true",
                        help="reboot once before checking the GPU")
    args = parser.parse_args()
    qemu = str(Path(args.qemu).resolve())
    image = str(Path(args.image).resolve())
    os.makedirs("/tmp/opencode", exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="pcie-gpu-", dir="/tmp/opencode") as tmp:
        disk = Path(tmp) / "disk.qcow2"
        qmp = Path(tmp) / "qmp.sock"
        diagnostic = Path(tmp) / "qemu.log"
        screenshot = Path(tmp) / "scanout.ppm"
        subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "raw",
                        "-b", image, str(disk)], check=True)
        cmd = [qemu, "-M", "rk3588,pcie-experimental=on",
               "-device", "virtio-gpu-pci,id=vgpu,bus=dw-pcie,addr=0,xres=640,yres=480",
               "-display", "none", "-serial", "stdio", "-monitor", "none",
               "-qmp", f"unix:{qmp},server=on,wait=off",
               "-drive", f"if=sd,file={disk},format=qcow2",
               "-d", "guest_errors,unimp", "-D", str(diagnostic)]
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, bufsize=0)
        output = bytearray()
        sent = False
        completed = False
        reboot_at = None
        reboot_kernel_at = None
        try:
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                ready, _, _ = select.select([proc.stdout], [], [], 1)
                if ready:
                    data = os.read(proc.stdout.fileno(), 65536)
                    if not data:
                        break
                    output.extend(data)
                    if args.reboot and reboot_at is None and b"root@orangepi5:~#" in output:
                        proc.stdin.write(b"reboot\n")
                        proc.stdin.flush()
                        reboot_at = len(output)
                    if reboot_at is not None and reboot_kernel_at is None:
                        pos = output.find(b"Starting kernel", reboot_at)
                        if pos >= 0:
                            reboot_kernel_at = pos
                    shell_ready = (b"root@orangepi5:~#" in output if not args.reboot
                                   else reboot_kernel_at is not None and
                                   b"root@orangepi5:~#" in output[reboot_kernel_at:])
                    if not sent and shell_ready:
                        # Emit distinct markers instead of relying on the echoed
                        # command, and check the framebuffer write's exit status.
                        command = (
                            "grep --color=never 'virtio0-control' /proc/interrupts; "
                            "python3 -c 'f=open(\"/dev/fb0\",\"wb\",buffering=0); "
                            "f.write(bytes([0,0,255,0])*640*480); f.close()' "
                            "&& printf '\\nGPU_%s\\n' WRITE_OK; "
                            "grep --color=never 'virtio0-control' /proc/interrupts; "
                            "printf '\\nGPU_%s\\n' TEST_DONE\n"
                        )
                        proc.stdin.write(command.encode())
                        proc.stdin.flush()
                        sent = True
                    if sent and b"GPU_TEST_DONE\r\n" in output:
                        completed = True
                        break
                if proc.poll() is not None:
                    break
            if not completed or b"GPU_WRITE_OK\r\n" not in output:
                raise RuntimeError("guest did not complete the framebuffer write")

            lines = [line for line in output.decode(errors="replace").splitlines()
                     if "ITS-PCI-MSIX-0004:41:00.0" in line and
                     "virtio0-control" in line]
            if len(lines) < 2:
                raise RuntimeError("missing GPU MSI-X interrupt counts")
            counts = [sum(map(int, re.findall(r"\d+", line.split(":", 1)[1]
                                              .split("ITS-PCI-MSIX", 1)[0])))
                      for line in lines[-2:]]
            if counts[1] <= counts[0]:
                raise RuntimeError(f"GPU MSI-X count did not increase: {counts}")

            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.settimeout(30)
                sock.connect(str(qmp))
                stream = sock.makefile("rwb", buffering=0)
                json.loads(stream.readline())

                def execute(name, parameters=None):
                    request = {"execute": name}
                    if parameters is not None:
                        request["arguments"] = parameters
                    stream.write(json.dumps(request).encode() + b"\n")
                    while True:
                        reply = json.loads(stream.readline())
                        if "event" not in reply:
                            break
                    if "error" in reply:
                        raise RuntimeError(f"QMP {name}: {reply['error']}")

                execute("qmp_capabilities")
                execute("screendump", {"filename": str(screenshot), "device": "vgpu"})
            ppm = screenshot.read_bytes().split(b"\n", 3)
            if ppm[:3] != [b"P6", b"640 480", b"255"] or ppm[3] != b"\xff\x00\x00" * (640 * 480):
                raise RuntimeError("GPU scanout did not match the guest's red framebuffer")
            if diagnostic.read_bytes():
                raise RuntimeError(f"QEMU diagnostic log is not empty: {diagnostic.read_text()}")
            print(f"PASS: GPU MSI-X {counts[0]} -> {counts[1]}; 640x480 red scanout")
        except Exception:
            print(output.decode(errors="replace")[-12000:])
            raise
        finally:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    main()
