#!/usr/bin/env python3
"""Boot the UNMODIFIED Armbian Orange Pi 5 image with zero host help.

P20 (plan.md): no `-bios`, no `-device loader`, no `-dtb`, and not a single
byte ever written to the guest's serial stdin - the disk's own chain has to do
everything, exactly as on real hardware:

    bootrom-lite (RKNS idbloader@0x8000 on the SD unit) -> TPL (DDR v1.20)
    -> SPL -> u-boot.itb@8MiB (own TF-A) -> U-Boot proper -> bootstd scan
    -> /boot/boot.scr -> Image + uInitrd + dtb from the rootfs -> login:

The image is attached through a qcow2 overlay backed by the pristine repo
file, so the base image cannot be written by the guest (and its sha256 is
asserted unchanged at the end anyway).

Usage:
  python3 scripts/run-opi5-raw.py [--timeout S] [--until LABEL] [--fresh]
                                   [--qemu PATH] [--keep]
Labels (stop conditions for --until): rom, spl, uboot, script, kernel, login
Exit code 0 only when `login` is reached (unless --until stops earlier).
"""
import argparse
import hashlib
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

signal.signal(signal.SIGTTIN, signal.SIG_IGN)
signal.signal(signal.SIGTTOU, signal.SIG_IGN)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DEFAULT_IMG = os.path.join(ROOT,
                           "Armbian_26.8.1_Orangepi5_resolute_current_6.18.43"
                            "_kde-plasma_desktop.img")
PRISTINE_SHA256 = "7c677b2b1ab2dbc7692623a4b7b5c8dcda01fea9483af700cb234d2d67496b7e"
LOGDIR = "/tmp/opencode"

# label -> (pattern, human description) in expected order
MARKERS = [
    ("rom",    b"rk3588: v2 idbloader on SD",
     "bootrom-lite staged the card's idbloader"),
    ("spl",    b"U-Boot SPL 2026",
     "TPL (DDR v1.20) + SPL ran from the card"),
    ("uboot",  b"Model: Xunlong Orange Pi 5",
     "U-Boot proper banner (own bl31 from u-boot.itb)"),
    ("script", b"Boot script loaded from",
     "bootstd/distro scan found /boot/boot.scr by itself"),
    ("kernel", b"Starting kernel",
     "booti launched the disk-loaded kernel"),
    ("login",  b"orangepi5 login:",
     "Linux login prompt on serial"),
]
FAILS = [
    (b"DRAM init failed",            "TPL DDR init failed"),
    (b"### ERROR ### Please RESET",  "U-Boot fatal error"),
    (b"SPL: failed to boot from all boot devices", "SPL found no boot device"),
    (b"Bad Linux ARM64 Image magic", "booti got non-Image bytes"),
    (b"Kernel panic",                "kernel panic"),
    (b"Resetting board",             "guest reset"),
]


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("image", nargs="?", default=DEFAULT_IMG)
    p.add_argument("--qemu", default=None)
    p.add_argument("--timeout", type=int, default=1500,
                   help="overall seconds (default 1500)")
    p.add_argument("--until", default="login",
                   choices=[m[0] for m in MARKERS],
                   help="stop as soon as this marker is seen")
    p.add_argument("--fresh", action="store_true",
                   help="recreate the qcow2 overlay from scratch")
    p.add_argument("--keep", action="store_true",
                   help="leave QEMU running at the stop marker")
    return p.parse_args()


def pick_qemu(explicit):
    for c in ([explicit] if explicit else [
        os.path.join(ROOT, "build", "qemu", "qemu-system-aarch64"),
        os.path.join(ROOT, "rk3588-qemu-x86_64.AppImage"),
    ]):
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    sys.exit("error: no QEMU binary found")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def make_scratch(image, fresh):
    """Per-image COW overlay so the pristine file is never written."""
    key = hashlib.sha256(os.path.abspath(image).encode()).hexdigest()[:8]
    scratch = f"/tmp/opencode/opi5-p20-{key}.qcow2"
    if os.path.exists(scratch):
        # Also refuses a write-locked overlay belonging to a live QEMU.
        info = subprocess.run(["qemu-img", "info", "--output=json", scratch],
                              capture_output=True, text=True, check=True)
        meta = json.loads(info.stdout)
        if (meta["format"] != "qcow2" or
                os.path.realpath(meta.get("full-backing-filename", "")) !=
                os.path.realpath(image)):
            sys.exit(f"error: unexpected backing file for {scratch}")
    if fresh and os.path.exists(scratch):
        os.unlink(scratch)
    if not os.path.exists(scratch):
        info = subprocess.run(["qemu-img", "info", "--output=json", image],
                              capture_output=True, text=True, check=True)
        fmt = json.loads(info.stdout)["format"]
        subprocess.run(["qemu-img", "create", "-f", "qcow2",
                        "-b", os.path.abspath(image), "-F", fmt,
                        scratch], check=True, stdout=subprocess.DEVNULL)
        print(f"[p20] scratch overlay {scratch} -> {os.path.basename(image)}"
              f" (fmt={fmt})", flush=True)
    return scratch


def check_boot_region(image, scratch):
    """Compare logical overlay bytes, not just the immutable backing file."""
    with tempfile.TemporaryDirectory(prefix="p20-boot-", dir=LOGDIR) as tmp:
        boot = os.path.join(tmp, "boot.bin")
        subprocess.run(["qemu-img", "dd", "-f", "qcow2", "-O", "raw",
                        f"if={scratch}", f"of={boot}", "bs=1048576", "count=16"],
                       check=True, stdout=subprocess.DEVNULL)
        with open(image, "rb") as f, open(boot, "rb") as g:
            if f.read(16 << 20) != g.read():
                raise RuntimeError("guest modified the first 16 MiB boot region")
    print("[p20] logical first 16 MiB unchanged", flush=True)


class Guest:
    def __init__(self, cmd, env=None):
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
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
                if len(self._buf) > 4 * 1024 * 1024:
                    del self._buf[: len(self._buf) - 4 * 1024 * 1024]
            try:
                sys.stdout.flush()
                out.write(chunk)
                out.flush()
            except ValueError:
                pass

    def since(self, mark=0):
        with self._lock:
            data = bytes(self._buf)
            start = self._total - len(data)
        return data[max(mark - start, 0):]

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


def hmp(sock, cmd, wait=1.0):
    """Send one HMP command over the unix monitor socket."""
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(wait)
            s.connect(sock)
            greeting = bytearray()
            while b"(qemu) " not in greeting:
                chunk = s.recv(4096)
                if not chunk:
                    return False
                greeting.extend(chunk)
            s.sendall((cmd + "\n").encode())
            # Drain readline's echo as well as the response.  Closing after
            # a fixed sleep can discard a long command before its newline
            # is processed, especially with an unread ANSI echo buffer.
            reply = bytearray()
            while not reply.endswith(b"(qemu) "):
                chunk = s.recv(4096)
                if not chunk:
                    return cmd == "quit"
                reply.extend(chunk)
            return b"Error" not in reply and b"unknown command" not in reply
    except OSError:
        return False


def check_cpus(mon):
    """Inspect Linux's online mask without changing guest registers/memory."""
    with tempfile.TemporaryDirectory(prefix="p20-cpus-", dir=LOGDIR) as tmp:
        sock = os.path.join(tmp, "gdb.sock")
        if not hmp(mon, f"gdbserver unix:{sock},server=on,wait=off"):
            raise RuntimeError("could not start read-only CPU proof")
        if not os.path.exists(sock):
            raise RuntimeError("monitor did not create the CPU-proof socket")
        result = subprocess.run(
            ["gdb-multiarch", "-q", "-nx", "-batch",
             "-ex", "set architecture aarch64",
             "-ex", f"target remote {sock}",
             "-x", os.path.join(HERE, "check-opi5-cpus.gdb"),
             "-ex", "detach"], capture_output=True, text=True, timeout=30)
        proof = "P20_CPU_PROOF: Linux __cpu_online_mask=0xff (8 CPUs)"
        if result.returncode != 0 or proof not in result.stdout:
            raise RuntimeError("CPU proof failed: " + result.stdout + result.stderr)
        print("[p20] " + proof, flush=True)


def main():
    args = parse_args()
    if not os.path.isfile(args.image):
        sys.exit(f"error: image not found: {args.image}")
    qemu = pick_qemu(args.qemu)
    print(f"[p20] QEMU : {qemu}", flush=True)
    print(f"[p20] image: {args.image}", flush=True)

    base_digest = sha256(args.image)
    if os.path.realpath(args.image) == os.path.realpath(DEFAULT_IMG):
        if base_digest != PRISTINE_SHA256:
            sys.exit("error: target image does not match the pristine SHA256")
    else:
        print("[p20] DIAGNOSTIC INPUT: not the pristine acceptance image",
              flush=True)
    print(f"[p20] pristine image sha256 {base_digest}", flush=True)

    scratch = make_scratch(args.image, args.fresh)
    mon = os.path.join(LOGDIR, "p20-monitor.sock")
    if os.path.exists(mon):
        probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            probe.connect(mon)
        except ConnectionRefusedError:
            pass
        else:
            sys.exit(f"error: monitor already in use: {mon}")
        finally:
            probe.close()
        os.unlink(mon)
    qlog = os.path.join(LOGDIR, "p20-qemu.log")
    if os.path.exists(qlog):
        os.unlink(qlog)

    cmd = [qemu, "-M", "rk3588", "-display", "none", "-serial", "stdio",
           "-monitor", f"unix:{mon},server,nowait",
           "-drive", f"if=sd,file={scratch},format=qcow2",
           "-d", "unimp,guest_errors,invalid_mem", "-D", qlog]

    env = os.environ.copy()
    if qemu.endswith(".AppImage") and not os.path.exists("/dev/fuse"):
        env["APPIMAGE_EXTRACT_AND_RUN"] = "1"
        print("[p20] no /dev/fuse - APPIMAGE_EXTRACT_AND_RUN=1", flush=True)

    print("[p20] launching: no -bios, no loaders, serial stdin = /dev/null",
          flush=True)
    t0 = time.time()
    g = Guest(cmd, env)

    want = [m for m in MARKERS
            if ([x[0] for x in MARKERS].index(m[0])
                <= [x[0] for x in MARKERS].index(args.until))]
    seen = []
    verdict = None
    try:
        while True:
            elapsed = time.time() - t0
            if elapsed > args.timeout:
                verdict = f"TIMEOUT after {int(elapsed)}s (saw {seen})"
                break
            if g.proc.poll() is not None:
                verdict = (f"qemu exited rc={g.proc.returncode} at "
                           f"{int(elapsed)}s (saw {seen})")
                break
            tail = g.since(0)
            for pat, why in FAILS:
                if pat in tail:
                    verdict = (f"FAILED at {why!r} "
                               f"({pat.decode(errors='replace')!r}) "
                               f"at {int(elapsed)}s, saw {seen}")
                    break
            if verdict:
                break
            for label, pat, _desc in want:
                if pat in tail and label not in seen:
                    seen.append(label)
                    print(f"\n[p20] MARKER {label} at {int(elapsed)}s: "
                          f"{pat.decode(errors='replace')!r}", flush=True)
                    if label == args.until:
                        try:
                            if label == "login":
                                check_cpus(mon)
                            verdict = (f"OK to '{args.until}' in "
                                       f"{int(elapsed)}s")
                        except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
                            verdict = f"FAILED: {exc}"
                        break
            if verdict:
                break
            if all(label in seen for label, _, _ in want):
                verdict = f"OK all markers in {int(elapsed)}s"
                break
            time.sleep(0.5)

        print(f"\n[p20] VERDICT: {verdict}", flush=True)
        print(f"[p20] markers seen: {seen}", flush=True)
        if args.keep and verdict and verdict.startswith("OK"):
            print("[p20] --keep: leaving QEMU running", flush=True)
            while g.proc.poll() is None:
                time.sleep(1)
    finally:
        if not args.keep or not (verdict and verdict.startswith("OK")):
            hmp(mon, "quit")
            g.stop()

    after = sha256(args.image)
    print(f"[p20] pristine image sha256 after run {after}", flush=True)
    if after != base_digest:
        sys.exit("[p20] FATAL: pristine image changed!")
    print("[p20] pristine image unchanged", flush=True)
    check_boot_region(args.image, scratch)

    n_log = os.path.getsize(qlog) if os.path.exists(qlog) else 0
    print(f"[p20] qemu -d log: {qlog} ({n_log} bytes)", flush=True)
    if n_log:
        with open(qlog, "rb") as f:
            tail = f.read()[-4000:]
        print("[p20] --- -d log tail ---", flush=True)
        sys.stdout.flush()
        os.write(sys.stdout.fileno(), tail)
        if args.until == "login":
            print("[p20] FAILED: diagnostic log is not clean", flush=True)
            return 1

    if verdict and verdict.startswith("OK"):
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
