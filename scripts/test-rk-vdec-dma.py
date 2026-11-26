#!/usr/bin/env python3
"""Test VDEC0 DMA reads and error IRQ through real guest MMIO and page tables."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--qemu', default='build/qemu/qemu-system-aarch64')
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix='rk-vdec-dma-', dir='/tmp/opencode') as tmp:
    tmp = Path(tmp)
    trace = tmp / 'trace.log'
    sockpath = tmp / 'qmp.sock'
    proc = subprocess.Popen([str(Path(args.qemu).resolve()), '-M', 'rk3588,vdec0-power=on',
                             '-S', '-display', 'none', '-serial', 'none', '-monitor', 'none',
                             '-qtest', 'stdio', '-qtest-log', '/dev/null',
                             '-qmp', f'unix:{sockpath},server=on,wait=off',
                             '-trace', 'rk3588_vdec_dma_probe', '-D', str(trace)],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(10)
    try:
        for _ in range(100):
            if proc.poll() is not None:
                raise RuntimeError(proc.stderr.read())
            try:
                sock.connect(str(sockpath))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(.05)
        else:
            raise RuntimeError('QMP socket did not become available')
        stream = sock.makefile('rwb', buffering=0)
        stream.readline()

        def qmp(command, arguments=None):
            stream.write((json.dumps({'execute': command, 'arguments': arguments or {}}) + '\n').encode())
            while True:
                reply = json.loads(stream.readline())
                if 'error' in reply:
                    raise RuntimeError(reply)
                if 'return' in reply:
                    return reply['return']

        irqs = {}

        def qtest(command):
            proc.stdin.write(command + '\n')
            proc.stdin.flush()
            reply = proc.stdout.readline().strip()
            while reply.startswith('IRQ '):
                _, level, num = reply.split()
                irqs[int(num)] = level == 'raise'
                reply = proc.stdout.readline().strip()
            if not reply.startswith('OK'):
                raise RuntimeError(f'{command}: {reply}')
            return reply[2:].strip()

        def write(addr, value):
            qtest(f'writel {addr:#x} {value:#x}')

        def read(addr):
            return int(qtest(f'readl {addr:#x}'), 16)

        qmp('qmp_capabilities')
        devices = qmp('qom-list', {'path': '/machine/unattached'})
        dev = next(d['name'] for d in devices if d['type'] == 'child<rk3588-vdec-experimental>')
        qtest(f'irq_intercept_out /machine/unattached/{dev} sysbus-irq')
        # IOVA 0x40000ff8 crosses two non-contiguous physical pages.
        root, table, page1, page2 = 0x01000000, 0x01001000, 0x02000000, 0x02100000
        iova = 0x40000ff8
        write(root + (iova >> 22) * 4, table | 1)
        write(table, page1 | 3)  # present, read-only
        write(table + 4, page2 | 3)
        write(page1 + 0xff8, 0x04030201)
        write(page1 + 0xffc, 0x08070605)
        write(page2, 0x0c0b0a09)
        write(page2 + 4, 0x100f0e0d)
        mmu, vdec = 0xfdc38700, 0xfdc38100
        write(mmu, root)
        write(mmu + 8, 0)  # enable paging
        write(vdec + 0x200, iova)
        write(vdec + 0x40, 16)
        write(vdec + 0x28, 1)
        assert read(vdec + 0x380) == 0x10  # error, not fake decode success
        assert irqs.get(0), 'decode error did not assert IRQ'
        write(vdec + 0x380, 0)
        assert not irqs.get(0), 'acknowledgement did not lower IRQ'
        # Deny the second page: DMA must fail, not use the physical IOVA.
        write(table + 4, page2 | 1)
        write(vdec + 0x28, 1)
        assert read(vdec + 0x380) == 0x10
        assert irqs.get(0)
        qmp('system_reset')
        assert read(vdec + 0x380) == 0
        assert not irqs.get(0), 'reset did not lower IRQ'
        assert read(mmu) == 0
        assert not read(mmu + 4) & 1, 'reset left IOMMU paging enabled'
        qmp('quit')
        proc.wait(timeout=10)
        log = trace.read_text()
        assert 'iova=0x40000ff8 len=16 result=0 sum=136' in log, log
        assert 'iova=0x40000ff8 len=16 result=1 sum=0' in log, log
        print('PASS: VDEC DMA translates non-contiguous pages, denies access, and signals error IRQ')
    finally:
        sock.close()
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=10)
