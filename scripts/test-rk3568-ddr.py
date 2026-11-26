#!/usr/bin/env python3
"""Test the unmodified DDR binary returning under synthetic hardware."""
import hashlib
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent
if len(sys.argv) != 2:
    sys.exit("Usage: python3 scripts/test-rk3568-ddr.py /path/to/rk3568_ddr_1560MHz_v1.26.bin")
image = pathlib.Path(sys.argv[1]).resolve()
sha = hashlib.sha256(image.read_bytes()).hexdigest()
if sha != "acb4e054298f0f0c8471dd3d45de07a1c77b67e687ec6e77ede365a087f1d95a":
    sys.exit(f"Unexpected DDR image SHA256: {sha}")

with tempfile.TemporaryDirectory(prefix="rk3568-ddr-probe-") as tmp:
    tmp = pathlib.Path(tmp)
    elf = tmp / "probe.elf"
    log = tmp / "exceptions.log"
    subprocess.run(["aarch64-linux-gnu-gcc", "-nostdlib", "-Wl,-Ttext=0xfdcd1d00",
                    "-Wl,--build-id=none", "-o", str(elf),
                    str(root / "scripts/rk3568-ddr-probe.S")], check=True)
    cmd = [str(root / "build/qemu/qemu-system-aarch64"), "-M", "rk3568", "-m", "2G",
           "-nographic", "-monitor", "none", "-d", "int,guest_errors",
           "-D", str(log), "-device",
           f"loader,file={image},addr=0xfdcc1000,force-raw=on", "-device",
           f"loader,file={elf},cpu-num=0"]
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    timed_out = False
    try:
        try:
            output, _ = proc.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.terminate()
            output, _ = proc.communicate(timeout=5)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    text = output.decode(errors="replace")
    for line in text.splitlines():
        if ("fwver:" in line or "DDR3," in line or "Size=" in line or
                "change to:" in line or line in ("out", "DDR_PROBE_RETURN_OK",
                                                  "DDR_PROBE_RETURN_ERROR")):
            print(line)
    if not timed_out:
        sys.exit(f"FAIL: QEMU exited unexpectedly with status {proc.returncode}")
    if b"fwver: v1.26" not in output or b"LP4/4x derate" not in output:
        sys.exit("FAIL: expected DDR firmware output not observed")
    if b"\nDDR_PROBE_RETURN_OK\r\n" not in output:
        print(text)
        sys.exit("FAIL: DDR firmware did not return success")
    exceptions = log.read_text()
    if exceptions:
        print(exceptions[:2000], file=sys.stderr)
        sys.exit("FAIL: QEMU reported guest exceptions or errors")
    print("PASS: DDR image returned success with synthetic training")
