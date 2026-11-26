#!/usr/bin/env python3
"""Check the DWC3 idle run/stop status used by the RK3588 BootROM."""
import subprocess
import sys

qemu = sys.argv[1] if len(sys.argv) > 1 else "build/qemu/qemu-system-aarch64"
proc = subprocess.Popen(
    [qemu, "-M", "rk3588", "-S", "-display", "none", "-serial", "none",
     "-monitor", "none", "-qtest", "stdio", "-qtest-log", "/dev/null"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True)
irqs = {}

def qtest(command):
    proc.stdin.write(command + "\n")
    proc.stdin.flush()
    response = proc.stdout.readline().strip()
    while response.startswith("IRQ "):
        _, level, number = response.split()
        irqs[int(number)] = level == "raise"
        response = proc.stdout.readline().strip()
    if not response.startswith("OK"):
        raise RuntimeError(f"{command}: {response}")
    return response[2:].strip()

try:
    qtest("irq_intercept_in /machine/unattached/device[8]")
    for base in (0xfc000000, 0xfcd00000):
        irq = 220 if base == 0xfc000000 else 222
        dctl, dsts = base + 0xc704, base + 0xc70c
        for run in (0, 1, 0):
            qtest(f"writel {dctl:#x} {run << 31:#x}")
            status = int(qtest(f"readl {dsts:#x}"), 16)
            assert bool(status & (1 << 22)) == (not run), hex(status)
        # The BootROM acknowledges events here; a write is not storage.
        count = base + 0xc40c
        qtest(f"writel {count:#x} 4")
        assert int(qtest(f"readl {count:#x}"), 16) == 0
        # STARTNEWCONFIG command completion, polled from a 32-byte DMA ring.
        ring = 0x1000000
        qtest(f"writel {base + 0xc400:#x} {ring:#x}")
        qtest(f"writel {base + 0xc404:#x} 0")
        qtest(f"writel {base + 0xc408:#x} 32")
        qtest(f"writel {dctl:#x} 0x80000000")
        for i in range(10):
            qtest(f"writel {base + 0xc80c:#x} 0x509")
            assert irqs.get(irq), (irq, irqs)
            assert int(qtest(f"readl {base + 0xc80c:#x}"), 16) & 0x400 == 0
            assert int(qtest(f"readl {count:#x}"), 16) == 4
            event = int(qtest(f"readl {ring + (i % 8) * 4:#x}"), 16)
            assert event == (9 << 16) | (7 << 6), hex(event)
            qtest(f"writel {count:#x} 4")
            assert not irqs.get(irq), (irq, irqs)
        assert int(qtest(f"readl {count:#x}"), 16) == 0
        # Masking suppresses IRQ, not event DMA, and unmasking raises it.
        qtest(f"writel {base + 0xc408:#x} 0x80000020")
        qtest(f"writel {base + 0xc80c:#x} 0x509")
        assert int(qtest(f"readl {count:#x}"), 16) == 4
        assert not irqs.get(irq), (irq, irqs)
        qtest(f"writel {base + 0xc408:#x} 32")
        assert irqs.get(irq), (irq, irqs)
        qtest(f"writel {count:#x} 4")
        assert not irqs.get(irq), (irq, irqs)
        # Configure EP0 and EP1, as the MaskROM does before arming EP0.
        for ep in (0, 1):
            cmd = base + 0xc80c + ep * 0x10
            qtest(f"writel {cmd - 4:#x} 0x200")  # PAR0: 64-byte EP
            qtest(f"writel {cmd - 8:#x} {0x500 | (ep << 25):#x}")
            qtest(f"writel {cmd - 12:#x} 0")
            qtest(f"writel {cmd:#x} 0x401")
            assert int(qtest(f"readl {cmd:#x}"), 16) & 0x400 == 0
            qtest(f"writel {cmd - 4:#x} 1")  # one transfer resource
            qtest(f"writel {cmd:#x} 0x402")
            assert int(qtest(f"readl {cmd:#x}"), 16) & 0x400 == 0
        # Reconfiguring from index 2 must preserve EP1's configuration.
        qtest(f"writel {base + 0xc80c:#x} 0x20409")
        qtest(f"writel {base + 0xc81c:#x} 0x402")
        assert int(qtest(f"readl {base + 0xc81c:#x}"), 16) & 0x400 == 0
        # Arm a setup TRB. No USB packet has arrived, so DMA and completion
        # must not occur merely because the command was accepted.
        trb = 0x1001000
        qtest(f"writel {trb:#x} 0x12345678")
        qtest(f"writel {base + 0xc804:#x} {trb:#x}")
        qtest(f"writel {base + 0xc808:#x} 0")
        qtest(f"writel {base + 0xc80c:#x} 0x406")
        command = int(qtest(f"readl {base + 0xc80c:#x}"), 16)
        assert not command & 0x400 and (command >> 16) & 0x7f == 1
        assert int(qtest(f"readl {trb:#x}"), 16) == 0x12345678
        assert int(qtest(f"readl {count:#x}"), 16) == 0
        qtest(f"writel {base + 0xc80c:#x} 0x10408")
        assert not int(qtest(f"readl {base + 0xc80c:#x}"), 16) & 0x400
        qtest(f"writel {base + 0xc80c:#x} 0x509")
        assert irqs.get(irq)
        qtest(f"writel {dctl:#x} 0x40000000")
        assert not irqs.get(irq)
        assert int(qtest(f"readl {count:#x}"), 16) == 0
        assert int(qtest(f"readl {base + 0xc400:#x}"), 16) == 0
        assert int(qtest(f"readl {base + 0xc80c:#x}"), 16) == 0
        assert not int(qtest(f"readl {dctl:#x}"), 16) & (1 << 30)
    print("PASS: DWC3 idle DevCtrlHlt follows RunStop")
    print("PASS: DWC3 event-count acknowledgement does not create events")
    print("PASS: DWC3 command completion DMA, acknowledgement and ring wrap")
    print("PASS: DWC3 endpoint configuration and resource commands")
    print("PASS: DWC3 event IRQ masking and acknowledgement")
    print("PASS: DWC3 transfer arming and cancellation without host traffic")
    print("PASS: DWC3 soft reset clears modeled endpoint and event state")
finally:
    proc.terminate()
    proc.wait(timeout=10)
