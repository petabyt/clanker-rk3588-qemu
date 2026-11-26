#!/usr/bin/env python3
"""Test a guest GStreamer stateless H.264 pipeline against raw NV12 pixels.

Uses the existing snapshot-backed initramfs harness; it does not run a desktop.
The guest image must contain GStreamer and its v4l2codecs plugin. Supply an
arm64 gst-launch-1.0 binary matching the guest's GStreamer installation.
"""
import argparse
import gzip
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def arguments():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--qemu', type=Path, required=True)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--gst-launch', type=Path, required=True)
    p.add_argument('--clip', type=Path, required=True)
    p.add_argument('--reference', type=Path, required=True, help='packed NV12 frames')
    p.add_argument('--frames', type=int, default=300)
    p.add_argument('--timeout', type=int, default=400)
    p.add_argument('--child-dir', type=Path, help=argparse.SUPPRESS)
    return p.parse_args()


def child(args):
    spec = importlib.util.spec_from_file_location('harness', ROOT / 'scripts/test-vdec-initramfs.py')
    h = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(h)
    original_overlay = h.overlay
    trace = args.child_dir / 'vdec-trace.log'

    def overlay(*values):
        files = [('tmp/gst-launch-1.0', args.gst_launch, 0o755),
                 ('tmp/vdec-clip.mp4', args.clip, 0o644)]
        entries = [h.newc_entry(name, path.read_bytes(), stat.S_IFREG | mode, i + 1)
                   for i, (name, path, mode) in enumerate(files)]
        entries.append(h.newc_entry('TRAILER!!!', b'', 0, len(entries) + 1))
        return original_overlay(*values) + gzip.compress(b''.join(entries), mtime=0)

    original_popen = h.subprocess.Popen

    def popen(cmd, *values, **kwargs):
        if isinstance(cmd, list) and '-M' in cmd and Path(cmd[0]).resolve() == args.qemu.resolve():
            cmd += ['-trace', 'rk3588_vdec_result', '-D', str(trace)]
        return original_popen(cmd, *values, **kwargs)

    h.overlay = overlay
    h.subprocess.Popen = popen
    probe = b'''
echo GST_PIPELINE_BEGIN
mount --bind /dev /mnt/dev || echo GST_BIND_FAILED
mount --bind /proc /mnt/proc || echo GST_BIND_FAILED
mount --bind /sys /mnt/sys || echo GST_BIND_FAILED
mount --bind /tmp /mnt/tmp || echo GST_BIND_FAILED
set -o pipefail
timeout 180 chroot /mnt /usr/bin/env HOME=/tmp GST_REGISTRY=/tmp/gst-registry.bin GST_DEBUG_NO_COLOR=1 /tmp/gst-launch-1.0 -q filesrc location=/tmp/vdec-clip.mp4 ! qtdemux ! h264parse ! v4l2slh264dec ! fdsink fd=1 sync=false | md5sum
echo GST_PIPELINE_RC=$?
echo GST_PIPELINE_END
'''
    h.INIT = h.INIT.replace(b'echo VDEC_DEFERRED_BEGIN', probe + b'\necho VDEC_DEFERRED_BEGIN')
    sys.argv = ['test-vdec-initramfs', '--qemu', str(args.qemu.resolve()),
                '--image', str(args.image.resolve()), '--vdec0-power', '--expect-vdec0',
                '--timeout', str(args.timeout)]
    h.main()


def main():
    args = arguments()
    if args.child_dir:
        child(args)
        return
    for path in (args.qemu, args.image, args.gst_launch, args.clip, args.reference):
        if not path.is_file():
            raise SystemExit(f'missing file: {path}')
    with args.reference.open('rb') as f:
        expected = hashlib.file_digest(f, 'md5').hexdigest()
    with tempfile.TemporaryDirectory(prefix='vdec-gst-', dir='/tmp/opencode') as tmp:
        cmd = [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], '--child-dir', tmp]
        run = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=args.timeout + 240)
        log = run.stdout.decode(errors='replace')
        trace_path = Path(tmp) / 'vdec-trace.log'
        trace = trace_path.read_text() if trace_path.exists() else ''
        match = re.search(r'GST_PIPELINE_BEGIN.*?\b([0-9a-f]{32})  -\s+GST_PIPELINE_RC=(\d+)', log, re.S)
        good = trace.count('rk3588_vdec_result decoded=1')
        bad = trace.count('rk3588_vdec_result decoded=0')
        if (run.returncode or not match or match.group(1) != expected or
                match.group(2) != '0' or good != args.frames or bad or 'GST_BIND_FAILED' in log):
            print(log)
            print(f'FAIL: expected checksum {expected}, got {match.groups() if match else "no checksum"}; '
                  f'decoder completions success={good}, error={bad}', file=sys.stderr)
            raise SystemExit(1)
        print(f'PASS: GStreamer V4L2 stateless H.264 decoded {good} pictures; '
              f'NV12 checksum {expected} matches; no display')


if __name__ == '__main__':
    main()
