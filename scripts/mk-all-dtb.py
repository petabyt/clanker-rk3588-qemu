#!/usr/bin/env python3
"""Build the P19 "all devices" Armbian boot DTB.

Starts from the image's own vendor DTB (images/armbian/
rk3588s-orangepi-5-armbian.dtb = /boot/dtb/rockchip/
rk3588s-orangepi-5.dtb, byte-identical to the pristine .xz copy) and
applies exactly the edits the QEMU model needs - nothing is dropped, so
every device the kernel's DTB describes stays described and the model has
to answer for each one:

  1. eMMC mmc@fe2e0000: status "disabled" -> "okay".  The image ships
     with the dwcmshc node disabled (the board normally boots its kernel
     from SD); our runner attaches the disk to that controller
     (-drive if=sd,...,unit=1), so the node has to be live for
     root=/dev/mmcblk1p1 to exist.
  2. cpu-idle-states reference stripped from every /cpus/cpu@*.  This is
     the one known *model* gap rather than a device gap: TF-A's
     cortex_a76_core_pwr_dwn reads CPUECTLR_EL3 (s3_0_c15_c2_7), which
     TCG does not implement, so PSCI deep idle warm-resets the machine
     mid-boot (plan.md P13b, perf.md A4).  Pass --keep-idle-states to
     keep them once that register exists.
  3. Four virtio,mmio transports added at 0xfb200000+i*0x1000 (GIC SPIs
     476/477/478/479 - this DTB uses #interrupt-cells = <4>; the earlier
     92/93/94/98 collided with gpu@fb000000's job/mmu/gpu lines), the
     window src/rk3588.c now creates them in.  The real rockchip,rk3588-mali
     gpu@fb000000 node is left untouched: the model's 'gpu' storage stub
     answers GPU_ID = 0 so panthor fails probe with -ENXIO instead of
     reaching for firmware.

Usage:
  scripts/mk-all-dtb.py <vendor.dtb> <out.dtb> [--keep-idle-states]

Writes <out>.dts alongside <out>.dtb (dtc round-trip keeps this
reviewable in the repo, like the other derived DTBs).
"""
import argparse
import os
import subprocess
import sys

# address -> GIC SPI; order matches the transports in
# src/rk3588.c rk3588_create_virtio_gpu() (lowest address first).
# The SPIs must be unused by the board DT: 92/93/94 (= 0x5c..0x5e) are
# gpu@fb000000's job/mmu/gpu interrupts and 98 (0x62) is taken too, so the
# transports moved to 476..479 (0x1dc..0x1df, all free in this DTB and
# inside the machine's 480-SPI GIC).
TRANSPORTS = (
    (0xfb200000, 476),  # -device virtio-tablet-device
    (0xfb201000, 477),  # -device virtio-keyboard-device
    (0xfb202000, 478),  # -device virtio-gpu-device
    (0xfb203000, 479),  # -device virtio-net-device
)

VIRTIO_NODE = """\
\t/* scripts/mk-all-dtb.py: virtio transport for {what} (model side:
\t * rk3588_create_virtio_gpu(), window 0xfb200000.. just past the 2 MB
\t * Mali window, which stays to itself so gpu@fb000000 keeps failing
\t * panthor honestly).  reg size 0x200 = QEMU's virtio-mmio region. */
\tvirtio_mmio@{addr:x} {{
\t\tcompatible = "virtio,mmio";
\t\treg = <0x00 0x{addr:x} 0x00 0x200>;
\t\tdma-coherent;
\t\tinterrupts = <0x00 0x{spi:x} 0x04 0x00>;
\t\tstatus = "okay";
\t}};
"""

TRANSPORT_NAMES = (
    "virtio-tablet (input)",
    "virtio-keyboard (input)",
    "virtio-gpu (llvmpipe/zink render node + scanout)",
    "virtio-net",
)

DTC = "dtc"


def run(*argv):
    subprocess.run(argv, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src", help="pristine vendor DTB (from the image)")
    ap.add_argument("out", help="output DTB path")
    ap.add_argument("--keep-idle-states", action="store_true",
                    help="keep cpu-idle-states (needs CPUECTLR_EL3 in TCG)")
    ap.add_argument("--dts", default=None,
                    help="where to write the decompiled source "
                         "(default: next to <out>)")
    args = ap.parse_args()

    for p in (args.src,):
        if not os.path.isfile(p):
            sys.exit(f"error: missing {p}")

    out_dtb = args.out
    out_dts = args.dts or (os.path.splitext(out_dtb)[0] + ".dts")
    os.makedirs(os.path.dirname(os.path.abspath(out_dtb)), exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(out_dts)), exist_ok=True)

    # 1. decompile
    text = subprocess.run([DTC, "-I", "dtb", "-O", "dts", args.src],
                          check=True, capture_output=True).stdout.decode()

    # 2. eMMC: disabled -> okay (our rootfs rides that controller)
    eMMC_MARK = "mmc@fe2e0000 {"
    start = text.find(eMMC_MARK)
    if start < 0:
        sys.exit("error: mmc@fe2e0000 not found in the vendor DTB")
    end = text.find("\n\t};", start)
    body = text[start:end]
    if 'status = "disabled";' not in body:
        if 'status = "okay";' not in body:
            sys.exit("error: mmc@fe2e0000 has no status property")
        print("[mk-all-dtb] eMMC already okay")
    else:
        text = text[:start] + body.replace('status = "disabled";',
                                           'status = "okay";', 1) + text[end:]
        print("[mk-all-dtb] enabled eMMC (mmc@fe2e0000)")

    # 3. cpu-idle-states strip (unless asked to keep them)
    if args.keep_idle_states:
        print("[mk-all-dtb] keeping cpu-idle-states")
    else:
        kept = []
        lines = []
        for line in text.splitlines(True):
            if line.strip().startswith("cpu-idle-states = <"):
                continue
            lines.append(line)
        text = "".join(lines)
        print("[mk-all-dtb] stripped cpu-idle-states "
              "(TF-A CPUECTLR_EL3 UNDEF, plan.md P13b)")

    # 4. virtio transports, inserted just before the root node's closing brace
    nodes = "".join(VIRTIO_NODE.format(addr=a, spi=s, what=w)
                    for (a, s), w in zip(TRANSPORTS, TRANSPORT_NAMES))
    idx = text.rstrip().rfind("\n};")
    if idx < 0:
        sys.exit("error: could not find the root node's closing brace")
    text = text[:idx] + "\n" + nodes + text[idx:]
    print("[mk-all-dtb] added %d virtio,mmio nodes at 0x%x.."
          % (len(TRANSPORTS), TRANSPORTS[0][0]))

    # 5. compile back (dtc's phandle/cell warnings are noise from the
    # round-trip of a vendor blob - only surface real errors)
    with open(out_dts, "w") as fh:
        fh.write(text)
    comp = subprocess.run(
        [DTC, "-Wno-unit_address_vs_reg", "-I", "dts", "-O", "dtb",
         "-o", out_dtb, out_dts],
        capture_output=True, text=True)
    if comp.returncode:
        sys.stderr.write(comp.stderr)
        sys.exit(f"error: dtc failed on {out_dts}")

    # 6. sanity: the pieces we touched must read back the way we wrote them
    checks = [
        ("/mmc@fe2e0000", "status", "okay"),
        ("/virtio_mmio@fb200000", "compatible", "virtio,mmio"),
        ("/virtio_mmio@fb203000", "compatible", "virtio,mmio"),
        ("/gpu@fb000000", "compatible", "rockchip,rk3588-mali"),
    ]
    for node, prop, want in checks:
        got = subprocess.run(
            ["fdtget", "-t", "s", out_dtb, node, prop],
            capture_output=True, text=True)
        # fdtget concatenates a string list with spaces - compare the head
        if got.returncode or got.stdout.strip().split(" ").count(want) == 0:
            sys.exit(f"error: {node} {prop} = "
                     f"{got.stdout.strip() or '<missing>'!r}, want {want!r}")
    print(f"[mk-all-dtb] wrote {out_dtb} (+ {out_dts})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
