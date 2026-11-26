#!/usr/bin/env python3
"""Boot the combined image from SD and run a command, without modifying it."""
import os
import hashlib
import pathlib
import selectors
import subprocess
import sys
import tempfile
import time

root = pathlib.Path(__file__).resolve().parent.parent
image = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else root / 'build/rk3568/sd.img').resolve()
original_hash = hashlib.file_digest(image.open('rb'), 'sha256').digest()
with tempfile.TemporaryDirectory(prefix='rk3568-sd-', dir=os.environ.get('TMPDIR', '/tmp/opencode')) as tmp:
    log = pathlib.Path(tmp) / 'qemu.log'
    cmd = [str(root / 'build/qemu/qemu-system-aarch64'), '-M', 'rk3568', '-m', '2G',
           '-nographic', '-monitor', 'none', '-snapshot', '-drive',
           f'if=sd,format=raw,file={image}', '-d', 'guest_errors', '-D', str(log)]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    data = b''
    stopped = sent = False
    try:
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline and proc.poll() is None:
            for key, _ in sel.select(timeout=0.5):
                chunk = os.read(key.fd, 65536)
                if not chunk:
                    break
                data += chunk
                if not stopped and b'Hit any key to stop autoboot' in data:
                    proc.stdin.write(b' ')
                    proc.stdin.flush()
                    stopped = True
                if stopped and not sent and b'=> ' in data:
                    proc.stdin.write(b'echo RK3568_HELLO\n')
                    proc.stdin.flush()
                    sent = True
                if b'\nRK3568_HELLO\r\n' in data:
                    required = [b'fwver: v1.26', b'\nout\r\n', b'U-Boot SPL',
                                b'Trying to boot from MMC', b'Image u-boot ... sha256+ OK',
                                b'BL31:', b'U-Boot 2026', b'DRAM:  2 GiB\r\n',
                                b'RK3568_HELLO']
                    positions = [data.find(s) for s in required]
                    if min(positions) < 0 or positions != sorted(positions):
                        raise RuntimeError('missing or out-of-order boot stages')
                    if b'Synchronous Abort' in data or log.read_bytes():
                        raise RuntimeError('guest exception or QEMU diagnostic')
                    if hashlib.file_digest(image.open('rb'), 'sha256').digest() != original_hash:
                        raise RuntimeError('base SD image was modified')
                    for line in data.decode(errors='replace').splitlines():
                        if any(s in line for s in ('fwver:', 'Size=', 'U-Boot SPL',
                               'Trying to boot', 'Image u-boot', 'BL31:', 'U-Boot 2026',
                               'DRAM:', 'RK3568_HELLO')):
                            print(line)
                    print('RK3568 SD DDR -> SPL -> TF-A -> U-Boot: PASS')
                    sys.exit(0)
        raise RuntimeError('timed out or QEMU exited before the hello command')
    except RuntimeError as exc:
        sys.stdout.buffer.write(data)
        print(f'RK3568 SD boot: FAIL: {exc}', file=sys.stderr)
        if log.exists():
            print(log.read_text(errors='replace')[:4000], file=sys.stderr)
        sys.exit(1)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
