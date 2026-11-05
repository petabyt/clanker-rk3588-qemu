#!/usr/bin/env python3
"""P19 device-coverage matrix: what does the model do for every node in
the guest DTB?

Static half (no boot needed):
  * parse the boot DTB (decompile with dtc) for every enabled node with a
    `reg` - that is the work list the user asked for: "a dtb with all the
    devices the kernel's rk3588 dtbs have, so we know they are all
    implemented";
  * parse src/rk3588-stubs.c for the storage/hiword stub windows;
  * join against the curated list of real models in src/ (MODELED below);
  * classify each node as model / stub / GAP (unmapped = landmine).

Optional dynamic half:
  --guest <dump> joins a serial dump produced by scripts/run-coverage.py
  (one line per sysfs device:  bus|name|dt-path|driver) so each node can
  be marked bound / unbound / absent from the guest.

Usage:
  scripts/dtb-coverage.py images/armbian/rk3588s-orangepi-5-all.dtb
  scripts/dtb-coverage.py ... --guest logs/p19-guest.txt --out logs/p19-coverage.txt
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Real device models (src/*.c) and the windows they claim.  Keep in sync
# with the rk3588_create_* calls in src/rk3588.c - a comment says which.
MODELED = [
    ("gic", 0xFE600000, 0x110000),          # rk3588_create_gic (GICD+GICR)
    ("dw-apb-uart0", 0xFD890000, 0x1000),   # rk3588_create_uarts, UART0
    ("dw-apb-uart2", 0xFEB50000, 0x1000),   #   console, serial_hd(0)
    ("rk3588-i2c0", 0xFD880000, 0x1000),    # rk3588_create_i2cs
    ("rk3588-i2c1", 0xFEA90000, 0x1000),
    ("rk3588-i2c2", 0xFEAA0000, 0x1000),
    ("rk3588-i2c3", 0xFEAB0000, 0x1000),
    ("rk3588-i2c4", 0xFEAC0000, 0x1000),
    ("rk3588-i2c5", 0xFEAD0000, 0x1000),    #   hid-over-i2c touchpad
    ("rk3588-i2c6", 0xFEC80000, 0x1000),    #   HYM8563 RTC
    ("rk3588-i2c7", 0xFEC90000, 0x1000),
    ("rk3588-i2c8", 0xFECA0000, 0x1000),
    ("rk3588-spi", 0xFEB20000, 0x1000),      # rk3588_create_spis (rk806 PMIC bus)
    ("sysbus-ohci0", 0xFC840000, 0x40000),  # rk3588_create_usb + usb-kbd
    ("sysbus-ohci1", 0xFC8C0000, 0x40000),  # rk3588_create_usb, empty hub
    ("rk3588-vop2", 0xFDD90000, 0x5600),    # rk3588_create_display_devices
    ("rk3588-hdmi", 0xFDE80000, 0x20000),
    ("rk3588-edp0", 0xFDEC0000, 0x1000),
    ("rk3588-edp1", 0xFDED0000, 0x1000),
    ("rk3588-timer", 0xFEAE0000, 0x1000),   # rk3588_create_timer
    ("pl330-dmac0", 0xFEA10000, 0x1000),    # rk3588_create_dmacs
    ("pl330-dmac1", 0xFEA30000, 0x1000),
    ("pl330-dmac2", 0xFED10000, 0x1000),
    ("rk3588-dwmshc", 0xFE2C0000, 0x4000),  # rk3588_create_sdmmc (SD slot)
    ("rk3588-sdhci", 0xFE2E0000, 0x10000),  # rk3588_create_emmc (root disk)
    ("virtio-mmio", 0xFB200000, 0x4000),    # rk3588_create_virtio_gpu, vgpu=on
    ("rk3588-iommu", 0xFDAB9000, 0x200),    # rk3588_create_display_devices
    ("rk3588-iommu", 0xFDABA000, 0x200),
    ("rk3588-iommu", 0xFDACA000, 0x200),
    ("rk3588-iommu", 0xFDADA000, 0x200),
    ("rk3588-iommu", 0xFDB50800, 0x200),
    ("rk3588-iommu", 0xFDBA0800, 0x200),
    ("rk3588-iommu", 0xFDBA4800, 0x200),
    ("rk3588-iommu", 0xFDBA8800, 0x200),
    ("rk3588-iommu", 0xFDBAC800, 0x200),
    ("rk3588-iommu", 0xFDC38700, 0x200),
    ("rk3588-iommu", 0xFDC40700, 0x200),
    ("rk3588-iommu", 0xFDD97E00, 0x200),
    ("rk3588-iommu", 0xFDD97F00, 0x200),
    ("bootrom/sram", 0xFF000000, 0x100000),  # rk3588_init_memory: sram
    ("bootrom/sram1", 0xFF100000, 0x10000),
]

# Not addressed by a device/stub but harmless: RAM-like or never touched.
INERT_BASES = [
    (0, 0xF0000000, "guest RAM (low)"),
    (0x1_0000_0000, 0x1_0000_0000, "guest RAM (above 4G)"),
]

# Intended outcome per compatible (P19 decision table).  Only the rows we
# actively judge are listed; everything else defaults to "either bind to a
# model or fail probe - never hang".
EXPECT = {
    "rockchip,rk3588-mali": "fail-fast: gpu pd off -> probe abort",
    "rockchip,rk3588-vdec": "fail-fast: pd rkvdec0/1 off -> probe abort",
    "rockchip,rk3588-vpu121": "fail-fast: pd vdpu off -> probe abort",
    "rockchip,rk3588-vepu121": "fail-fast: pd vdpu off -> probe abort",
    "rockchip,rk3588-av1": "inert: no driver in this kernel",
    "rockchip,rk3588-rknn-core": "inert: no driver in this kernel",
    "rockchip,rk3588-rga": "fail-fast: pd vdpu off -> probe abort",
    "rockchip,rk3588-iep": "fail-fast: version read -> -ENODEV",
    "rockchip,sfc": "fail-fast: reject probe (no bind)",
    "rockchip,snps-dwmac": "no PHY: -ENODEV, no MII scan retry",
    "snps,dw-apb-eth-axi": "no PHY: -ENODEV, no MII scan retry",
    "snps,dw-apb-ahb-eth-axi": "no PHY: -ENODEV, no MII scan retry",
    "rockchip,rk3588-pcie": "link-down (pcie-link=off), empty bus",
    "rockchip,rk3588-pcie2x1l0": "link-down (pcie-link=off), empty bus",
    "rockchip,rk3588-pcie2x1l1": "link-down (pcie-link=off), empty bus",
    "rockchip,rk3588-pcie2x1l2": "link-down (pcie-link=off), empty bus",
    "rockchip,rk3588-pcie3x4": "link-down (pcie-link=off), empty bus",
    "rockchip,rk3588-spi": "model: rk3588-spi + GIC SPI 328",
    "rockchip,rk806": "bind: rk806 over modelled SPI -> regulators",
    "snps,dwc3": "bind: root hub (0 ports), harmless",
    "generic-ohci": "model: sysbus-ohci (+kbd on host0)",
    "chipidea,usb2": "bind: empty root hub (1 port; 0 rejected)",
    "linux,ehci-platform": "bind: empty root hub (1 port; 0 rejected)",
    "snps,dw-ahb-usb-otg": "bind: root hub (0 ports), harmless",
    "rockchip,rk3588-dw-mshc": "model: SD slot (empty)",
    "rockchip,rk3588-dwcmshc": "model: eMMC, root disk",
    "rockchip,rk3588-dwc3-xhci": "bind: root hub, harmless",
    "snps,dw3100-ahb-xhci": "bind: root hub, harmless",
    "snps,dwc-otg": "bind: root hub, harmless",
}

BUS_DEFAULT = "model-or-fail (no row)"

# rk3588 places everything it decodes at or above this: guest RAM below
# the devices ends at 0xf0000000, and 0xfb000000..0xfc000000 is the Mali
# window + the virtio transports.  Anything whose first `reg` is lower is
# not SoC MMIO: cpus carry their unit address, i2c/spi children carry bus
# addresses, usb2phy sub-nodes carry offsets into their parent syscon, and
# reserved-memory/shmem lives in RAM.  Those are excluded from the matrix
# (they are backed by the parent's model or by RAM, never by a stub).
MMIO_LO = 0xFB000000


def parse_dtb(path):
    """-> list of dicts: path, compat(list), status, regs(list of int)."""
    text = subprocess.run(["dtc", "-I", "dtb", "-O", "dts", path],
                          check=True, capture_output=True).stdout.decode()
    nodes, stack, cur = [], [], None

    def flush():
        nonlocal cur
        if cur:
            nodes.append(cur)
        cur = None

    for line in text.splitlines():
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip("\t"))
        s = line.strip()
        m = re.match(r"^([A-Za-z0-9_@,.\-]+)\s*\{$", s)
        if m:
            while stack and stack[-1][0] >= indent:
                stack.pop()
            stack.append((indent, m.group(1)))
            flush()
            cur = {"path": "/" + "/".join(n for _, n in stack),
                   "compat": [], "status": "okay", "regs": []}
            continue
        if cur is None:
            continue
        if s.startswith("compatible = "):
            cur["compat"] = re.findall(r'"([^"]+)"', s)
        elif s.startswith("status = "):
            cur["status"] = re.findall(r'"([^"]+)"', s)[0]
        elif s.startswith("reg = "):
            cells = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", s)]
            # #address-cells=2/#size-cells=2 at root: (hi,lo,size_hi,size_lo)
            for i in range(0, len(cells), 4):
                chunk = cells[i:i + 4]
                if len(chunk) == 4:
                    cur["regs"].append((chunk[0] << 32) | chunk[1])
                elif len(chunk) == 2:
                    cur["regs"].append(chunk[0])
                elif len(chunk) == 1:
                    cur["regs"].append(chunk[0])
    flush()
    return nodes


def parse_stub_spans(srcdir):
    """-> list of (name, base, size) from src/rk3588-stubs.c."""
    src = open(os.path.join(srcdir, "rk3588-stubs.c")).read()
    spans = []
    for m in re.finditer(
            r'\.name\s*=\s*"([^"]+)"[^}]*?\.base\s*=\s*(0x[0-9a-fA-F]+)'
            r"\s*,\s*\.size\s*=\s*(0x[0-9a-fA-F]+)", src, re.S):
        spans.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    for m in re.finditer(r'MK_MASK\("([^"]+)",\s*(0x[0-9a-fA-F]+)', src):
        spans.append((m.group(1), int(m.group(2), 16), 0x1000))
    return spans


def covering(base, spans):
    for name, b, size in spans:
        if b <= base < b + size:
            return name
    return None


def load_guest(path):
    """-> {dt-path: driver} from the run-coverage dump."""
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        parts = line.rstrip("\n").split("|")
        if len(parts) != 4:
            continue
        _bus, _name, dtpath, driver = parts
        # Old dumps (before run-coverage.py got the `[ -e .../driver ]`
        # test) labelled every unbound device with the literal string
        # "driver", because readlink -f succeeds on a dangling path.
        if driver == "driver":
            driver = "UNBOUND"
        if dtpath and dtpath != "-":
            key = dtpath.rstrip("/") or "/"
            # Root-hub interfaces inherit the controller's of_node (a usb
            # bus entry with the same path, driver "usb"/hub); the
            # platform device's own binding is the interesting one, so
            # platform entries win when a path repeats.
            if key in out and _bus != "platform":
                continue
            out[key] = driver
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dtb", help="boot DTB (make all-dtb)")
    ap.add_argument("--guest", help="serial dump from run-coverage.py")
    ap.add_argument("--out", help="also write the report here")
    args = ap.parse_args()

    stubs = parse_stub_spans(os.path.join(ROOT, "src"))
    nodes = parse_dtb(args.dtb)
    guest = load_guest(args.guest) if args.guest else {}

    rows, gaps = [], []
    nonmmio = 0
    for n in nodes:
        if n["status"] not in ("okay", "ok", "reserved"):
            continue
        if not n["compat"] or not n["regs"]:
            continue                      # cpus, caches, clocks, ...
        base = n["regs"][0]
        if base < MMIO_LO:
            nonmmio += 1                  # bus/cpu/RAM, see MMIO_LO
            continue
        # model first: the behavioural iommu/virtio windows sit *inside*
        # surrounding storage spans (e.g. codecs @0xfdb50000+0x60000
        # contains the iommu at 0xfdb50800).
        back = covering(base, [
            (name, b, s) for name, b, s in MODELED]) or covering(base, stubs)
        if back:
            kind = ("model:" if any(back == m[0] for m in MODELED)
                    else "stub:") + back
        else:
            kind = "GAP"
            gaps.append(n)
        compat = n["compat"][0]
        expect = EXPECT.get(compat, BUS_DEFAULT)
        state = guest.get(n["path"])
        rows.append((n["path"], compat, f"0x{base:x}", kind, expect, state))

    # ---- report
    lines = []
    w = max([len(r[0]) for r in rows] + [8])
    lines.append(f"P19 device coverage - {os.path.basename(args.dtb)}")
    lines.append(f"  enabled MMIO nodes     : {len(rows)}")
    lines.append(f"  non-MMIO skipped       : {nonmmio} (cpu unit addrs, "
                 f"i2c/spi/syscon children, RAM)")
    lines.append(f"  backed by a model      : "
                 f"{sum(1 for r in rows if r[3].startswith('model:'))}")
    lines.append(f"  backed by a stub       : "
                 f"{sum(1 for r in rows if r[3].startswith('stub:'))}")
    lines.append(f"  UNMAPPED (GAP)         : {len(gaps)}")
    if guest:
        bound = sum(1 for r in rows if r[5] and r[5] != "UNBOUND")
        lines.append(f"  seen bound in the guest: {bound}")
        lines.append(f"  seen unbound           : "
                     f"{sum(1 for r in rows if r[5] == 'UNBOUND')}")
        lines.append(f"  absent from sysfs      : "
                     f"{sum(1 for r in rows if r[5] is None)}")
    lines.append("")
    hdr = (f"{'node':<{w}}  {'compatible':<32}  {'base':<12}  "
           f"{'backing':<24}  {'expected'}")
    lines.append(hdr)
    lines.append("-" * len(hdr))
    for path, compat, base, kind, expect, state in rows:
        tail = ""
        if state is not None:
            tail = "  | " + ("bound=" + state if state != "UNBOUND"
                             else "UNBOUND")
        lines.append(f"{path:<{w}}  {compat:<32}  {base:<12}  "
                     f"{kind:<24}  {expect}{tail}")
    if gaps:
        lines.append("")
        lines.append("UNMAPPED windows (an access here is an invalid access):")
        for n in gaps:
            lines.append(f"  0x{n['regs'][0]:x}  {n['compat'][0]}  {n['path']}")
    report = "\n".join(lines) + "\n"
    sys.stdout.write(report)
    if args.out:
        with open(args.out, "w") as fh:
            fh.write(report)
    return 1 if gaps else 0


if __name__ == "__main__":
    sys.exit(main())
